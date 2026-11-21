# -*- coding: utf-8 -*-
# REQ-AUTH-CONNECTION-001 阶段三：服务端账号与连接生命周期 e2e。
# 覆盖：注册/登录携带 clientId、一次性恢复凭据、119 退出转匿名、121 断线恢复、
#       互踢 123 事件与版本兜底、注销全量撤销、心跳会话校验、旧客户端兼容、
#       服务端重启拒绝恢复、Redis 故障失败关闭、Redis 未配置无恢复旁路。
# 运行形态：脚本自管服务端生命周期（server_ctl 按 PID，端口被占报错不盲杀）；
#           exe=dist/Server.exe，cwd=dist；MySQL 读 Server/connect.config；
#           Redis 用本机 127.0.0.1:6379（故障场景用拒绝端口，不动真实 Redis 进程）。
# 只清理本脚本创建的账号/会话/目录。运行：py -3.13 -X utf8 本文件
import hashlib
import os
import secrets
import shutil
import socket
import struct
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DIST = REPO / "dist"
EXE = DIST / "Server.exe"
FS = DIST / "filesys"
sys.path.insert(0, str(REPO / "project-docs" / "evidence" / "_harness"))
import server_ctl  # noqa: E402
import pymysql  # noqa: E402

HOST, PORT, HDR, PWD = "127.0.0.1", 5000, 76, "123456"
REDIS_HOST, REDIS_PORT = "127.0.0.1", 6379
RUN = secrets.token_hex(4)
RESULTS = []
MADE_USERS = []
MADE_CIDS = []


def check(label, cond, detail=""):
    RESULTS.append((label, bool(cond)))
    print(("[PASS] " if cond else "[FAIL] ") + label + (" :: " + str(detail) if detail != "" else ""))
    sys.stdout.flush()


def uid(tag):
    name = f"ac3{tag}{RUN}"
    MADE_USERS.append(name)
    return name


def new_cid():
    cid = secrets.token_hex(16)
    MADE_CIDS.append(cid)
    return cid


# ---------------- DB ----------------
def db():
    cfg = (REPO / "Server" / "connect.config").read_text(encoding="utf-8").splitlines()
    return pymysql.connect(host=cfg[3], port=int(cfg[4]), user=cfg[6], password=cfg[7],
                           database=cfg[5], autocommit=True, connect_timeout=5)


def q1(sql, args=()):
    with db() as c, c.cursor() as cur:
        cur.execute(sql, args)
        return cur.fetchone()


def exec_sql(sql, args=()):
    with db() as c, c.cursor() as cur:
        return cur.execute(sql, args)


def lv(name):
    r = q1("SELECT login_version FROM user_info WHERE name=%s", (name,))
    return r[0] if r else None


def online(name):
    r = q1("SELECT online FROM user_info WHERE name=%s", (name,))
    return r[0] if r else None


# ---------------- Redis（最小 RESP 客户端） ----------------
def redis_call(*args):
    s = socket.create_connection((REDIS_HOST, REDIS_PORT), timeout=3)
    try:
        req = f"*{len(args)}\r\n".encode()
        for a in args:
            b = a.encode() if isinstance(a, str) else a
            req += f"${len(b)}\r\n".encode() + b + b"\r\n"
        s.sendall(req)
        s.settimeout(2)
        data = b""
        while True:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            data += chunk
            if data.endswith(b"\r\n"):
                break
        return data
    finally:
        s.close()


def redis_get(key):
    r = redis_call("GET", key)
    if r.startswith(b"$-1"):
        return None
    if r.startswith(b"$"):
        return r.split(b"\r\n", 1)[1][:-2]
    return None


def redis_ttl(key):
    r = redis_call("TTL", key)
    return int(r[1:-2]) if r.startswith(b":") else None


# ---------------- PDU / 连接 ----------------
def f32(s):
    return s.encode()[:31].ljust(32, b"\0")


def creds(name):
    return f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()


def pdu(t, msg=b"", dat=b""):
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat[:64].ljust(64, b"\0") + msg


def unpack(buf):
    out = []
    while len(buf) >= HDR:
        tl, ml, ty = struct.unpack("<III", buf[:12])
        if tl < HDR or tl != HDR + ml or len(buf) < tl:
            break
        out.append((ty, buf[12:76], buf[76:tl]))
        buf = buf[tl:]
    return out, buf


class Conn:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=10)
        self.buf = b""
        self.eof = False

    def pump(self, timeout=0.4):
        self.s.settimeout(timeout)
        try:
            d = self.s.recv(262144)
            if not d:
                self.eof = True
            else:
                self.buf += d
        except socket.timeout:
            pass
        except OSError:
            self.eof = True

    def packets(self):
        ps, self.buf = unpack(self.buf)
        return ps

    def ask(self, pkt, want, timeout=8):
        try:
            self.s.sendall(pkt)
        except OSError:
            self.eof = True
        got, end = [], time.time() + timeout
        while not [g for g in got if g[0] == want] and time.time() < end and not self.eof:
            self.pump(0.2)
            got.extend(self.packets())
        return got

    def drain(self, seconds=0.8):
        end = time.time() + seconds
        got = []
        while time.time() < end and not self.eof:
            self.pump(0.2)
            got.extend(self.packets())
        return got

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def i32(b, off=0):
    return struct.unpack("<i", b[off:off + 4])[0]


def i64(b, off=0):
    return struct.unpack("<q", b[off:off + 8])[0]


def get_captcha(c, cid=None):
    dat = cid.encode().ljust(64, b"\0") if cid else b""
    r = c.ask(pdu(44, dat=dat), 45)
    if not r:
        return None, None
    code = i32(r[0][1])
    text = r[0][2].split(b"\0")[0].decode() if r[0][2] else ""
    return code, text


def clear_cooldown(cid):
    # 测试夹具特权：只删本脚本自建 clientId 的冷却键，绕过 60s 冷却以便快速重复登录
    if cid:
        redis_call("DEL", f"cap:{cid}:cool")


def do_register(c, name, cid=None):
    clear_cooldown(cid)
    cap, text = get_captcha(c, cid)   # cid=None → 空载荷，验证码状态按连接级（旧兼容）
    if cap != 0:
        return cap
    msg = text.encode() + b"\0" + (cid.encode() if cid else b"")
    r = c.ask(pdu(1, msg=msg, dat=creds(name)), 2)
    return i32(r[0][1]) if r else -99


def do_login(c, name, cid=None):
    """返回 (ret, token, loginName)；token 为空串=无恢复凭据（旧兼容/失败）"""
    clear_cooldown(cid)
    cap, text = get_captcha(c, cid)   # cid=None → 空载荷（旧兼容）
    if cap != 0:
        return cap, "", ""
    msg = text.encode() + b"\0" + (cid.encode() if cid else b"")
    r = c.ask(pdu(3, msg=msg, dat=creds(name)), 4)
    if not r:
        return -99, "", ""
    ret = i32(r[0][1])
    token = r[0][2].split(b"\0")[0].decode() if r[0][2] else ""
    lname = r[0][1][32:64].split(b"\0")[0].decode()
    return ret, token, lname


def do_logout(c, cid):
    r = c.ask(pdu(119, dat=cid.encode().ljust(64, b"\0")), 120)
    return i32(r[0][1]) if r else -99


def do_resume(c, cid, token):
    msg = token.encode() + b"\0"
    r = c.ask(pdu(121, msg=msg, dat=cid.encode().ljust(64, b"\0")), 122)
    if not r:
        return -99, 0, ""
    ret = i32(r[0][1])
    ver = i64(r[0][1], 4)
    name = r[0][1][32:64].split(b"\0")[0].decode()
    return ret, ver, name


def do_logoff(c, name):
    r = c.ask(pdu(48, dat=creds(name)), 49)
    return i32(r[0][1]) if r else -99


def heartbeat(c, cid=None):
    dat = cid.encode().ljust(64, b"\0") if cid else b""
    r = c.ask(pdu(50, dat=dat), 51)
    return i32(r[0][1]) if r else -99


def biz_probe(c):
    """用 5/6 查找用户作业务探针：返回是否拿到正常响应（未被踢/未拒绝）"""
    r = c.ask(pdu(5, dat=f32("nobody_xxx")), 6)
    return (not c.eof) and any(p[0] == 6 for p in r)


# ---------------- 场景 ----------------
def phase_normal():
    print("--- 阶段 A：Redis 正常 ---")
    # A1 注册+登录携带 clientId，一次性 token
    u1 = uid("a")
    cid1 = new_cid()
    c = Conn()
    check("A1 注册（携带 clientId）成功", do_register(c, u1, new_cid()) == 0)
    ret, token, lname = do_login(c, u1, cid1)
    check("A2 登录成功且回传 64 位 hex 恢复凭据",
          ret == 0 and len(token) == 64 and lname == u1, f"ret={ret} toklen={len(token)}")
    sess = redis_get(f"session:{cid1}")
    check("A3 Redis 会话存在且只存摘要不存明文",
          sess is not None and token.encode() not in sess
          and hashlib.sha256(token.encode()).hexdigest().encode() in sess)
    check("A4 会话 TTL≈1800s", 1790 <= (redis_ttl(f"session:{cid1}") or 0) <= 1800,
          redis_ttl(f"session:{cid1}"))
    check("A5 sessions 索引含 clientId",
          redis_call("SISMEMBER", f"sessions:{u1}", cid1).strip() == b":1")
    check("A6 登录后 DB online=1", online(u1) == 1)

    # A7 心跳：正确 clientId 续期；错误 clientId 拒绝
    check("A7 心跳（正确 clientId）PONG 码 0", heartbeat(c, cid1) == 0)
    check("A8 心跳（错误 clientId）回 -3015", heartbeat(c, new_cid()) == -3015)

    # A9 非法 clientId 登录：-3015 且不消费验证码
    badcid = "G" * 32
    cid_probe = new_cid()
    cap, text = get_captcha(c, cid_probe)
    r = c.ask(pdu(3, msg=text.encode() + b"\0" + badcid.encode(), dat=creds(u1)), 4)
    check("A9 非法 clientId 登录回 -3015", r and i32(r[0][1]) == -3015)
    r = c.ask(pdu(3, msg=text.encode() + b"\0" + cid_probe.encode(), dat=creds(u1)), 4)
    check("A10 同一验证码未被非法请求消费（改合法 clientId 后登录成功）",
          r and i32(r[0][1]) == 0)
    # 上面登录把版本推到 2 并覆盖会话到 cid_probe；重新用 cid1 登录拿回干净状态
    ret, token, _ = do_login(c, u1, cid1)
    check("A11 重新登录恢复 cid1 会话", ret == 0 and len(token) == 64)
    c.close()
    time.sleep(0.6)

    # A12 断线恢复：物理断开后会话仍在，新连接 121 恢复同版本
    ver_before = lv(u1)
    c2 = Conn()
    rret, rver, rname = do_resume(c2, cid1, token)
    check("A12 断线后 121 恢复成功（版本不变、账号回带）",
          rret == 0 and rver == ver_before and rname == u1,
          f"ret={rret} ver={rver} expect={ver_before}")
    check("A13 恢复后业务请求可用", biz_probe(c2))
    check("A14 恢复后 DB online=1", online(u1) == 1)
    check("A15 恢复续期会话 TTL", 1790 <= (redis_ttl(f"session:{cid1}") or 0) <= 1800)

    # A16 恢复失败路径
    c3 = Conn()
    check("A16 错误 token 恢复回 -3016", do_resume(c3, cid1, "a" * 64)[0] == -3016)
    check("A17 非法 clientId 恢复回 -3015", do_resume(c3, "z" * 32, token)[0] == -3015)
    r = c3.ask(pdu(121, msg=token.encode()[:60] + b"\0", dat=cid1.encode().ljust(64, b"\0")), 122)
    check("A18 截断 token 恢复回 -3016", r and i32(r[0][1]) == -3016)
    check("A19 恢复失败不产生半认证（业务被拒 -1001）",
          (lambda rr: rr and i32(rr[0][1]) == -1001)(c3.ask(pdu(5, dat=f32("nobody_xxx")), 6)))
    c3.close()

    # A20 恢复接管：旧物理连接仍存活时，新连接恢复同会话 → 旧连接被关
    c4 = Conn()
    rret, _, _ = do_resume(c4, cid1, token)
    time.sleep(0.6)
    c2.drain(0.6)   # eof 需实际 pump 才可见（sleep 不触发读）
    check("A20 存活旧连接被恢复接管关闭", rret == 0 and c2.eof)
    check("A21 接管后新连接业务可用", biz_probe(c4))

    # A22 互踢：新设备登录 → 旧连接收 123 后断开；旧 token 恢复被版本门拒绝
    cidB = new_cid()
    cB = Conn()
    retB, tokenB, _ = do_login(cB, u1, cidB)
    ev = c4.drain(1.0)
    got123 = [p for p in ev if p[0] == 123]
    check("A22 新设备登录成功", retB == 0 and len(tokenB) == 64)
    check("A23 旧连接收到 123 失效事件（原因 -3017 + 旧版本）",
          got123 and i32(got123[0][1]) == -3017 and i64(got123[0][1], 4) == ver_before,
          f"pkts={[p[0] for p in ev]}")
    check("A24 旧连接随后被断开", c4.eof)
    c5 = Conn()
    check("A25 旧 token 恢复被版本门拒绝 -3017", do_resume(c5, cid1, token)[0] == -3017)
    c5.close()

    # A27 119 退出：会话撤销、连接转匿名、可重新登录
    check("A27 119 退出成功", do_logout(cB, cidB) == 0)
    check("A28 退出后会话键删除", redis_get(f"session:{cidB}") is None)
    check("A29 退出后在线键删除", redis_get(f"online:{u1}") is None)
    check("A30 退出后业务请求被拒 -1001（匿名）",
          (lambda rr: rr and i32(rr[0][1]) == -1001)(cB.ask(pdu(5, dat=f32("nobody_xxx")), 6)))
    check("A31 退出后旧 token 恢复失败 -3017", do_resume(Conn(), cidB, tokenB)[0] == -3017)
    retRe, tokRe, _ = do_login(cB, u1, cidB)
    check("A32 匿名连接重新登录成功", retRe == 0 and len(tokRe) == 64)

    # A33 注销：全部会话撤销
    cidC = new_cid()
    cC = Conn()
    retC, tokenC, _ = do_login(cC, u1, cidC)   # 版本再+1，踢 cB
    time.sleep(0.5)
    check("A33 注销前两设备会话都在",
          redis_get(f"session:{cidB}") is not None and redis_get(f"session:{cidC}") is not None)
    check("A34 48 注销成功", do_logoff(cC, u1) == 0)
    check("A35 注销后账号全部会话键删除",
          redis_get(f"session:{cidB}") is None and redis_get(f"session:{cidC}") is None)
    check("A36 注销后 sessions 索引清空",
          redis_call("SCARD", f"sessions:{u1}").strip() in (b":0", b"$-1"))
    check("A37 注销后任一 token 恢复失败", do_resume(Conn(), cidC, tokenC)[0] in (-3016, -3017))
    cB.close(); cC.close(); c2.close(); c4.close()
    time.sleep(0.8)
    check("A38 注销后账号行已删除（在线态随行消失）",
          q1("SELECT name FROM user_info WHERE name=%s", (u1,)) is None)

    # A39 旧客户端兼容：无 clientId 注册/登录成功、无 token、无会话；119 仍可退出
    # 旧路径冷却按连接级：注册与登录分连接（与既有旧端行为一致）
    u2 = uid("b")
    cReg = Conn()
    check("A39 旧布局注册成功", do_register(cReg, u2) == 0)
    cReg.close()
    cL = Conn()
    retL, tokL, _ = do_login(cL, u2)
    check("A40 旧布局登录成功且无恢复凭据", retL == 0 and tokL == "")
    check("A41 旧路径不建立 Redis 会话",
          all(redis_get(f"session:{cid}") is None or u2.encode() not in (redis_get(f"session:{cid}") or b"")
              for cid in MADE_CIDS))
    check("A42 旧连接业务可用", biz_probe(cL))
    check("A43 旧连接 119 退出成功（无会话可撤销）", do_logout(cL, new_cid()) == 0)
    cL.close()
    time.sleep(0.5)
    exec_sql("DELETE FROM login_record WHERE username=%s", (u2,))
    exec_sql("DELETE FROM user_info WHERE name=%s", (u2,))
    shutil.rmtree(FS / u2, ignore_errors=True)

    # A44 会话过期模拟：直接删会话键 → 恢复 -3017（TTL 到期同路径）
    u3 = uid("c")
    cidE = new_cid()
    cE = Conn()
    do_register(cE, u3, new_cid())
    retE, tokenE, _ = do_login(cE, u3, cidE)
    cE.close()
    time.sleep(0.5)
    redis_call("DEL", f"session:{cidE}")
    check("A44 会话键过期/删除后恢复回 -3017", do_resume(Conn(), cidE, tokenE)[0] == -3017)
    exec_sql("DELETE FROM login_record WHERE username=%s", (u3,))
    exec_sql("DELETE FROM user_info WHERE name=%s", (u3,))
    shutil.rmtree(FS / u3, ignore_errors=True)
    return u1, cid1, token


def phase_restart(u1, cid1, token):
    print("--- 阶段 B：服务端重启后拒绝恢复 ---")
    # u1 已注销；重新注册登录拿新会话，再重启
    c = Conn()
    do_register(c, u1, new_cid())
    ret, token, _ = do_login(c, u1, cid1)
    c.close()
    check("B1 重启前登录成功", ret == 0 and len(token) == 64)
    return token


def phase_redis_down():
    print("--- 阶段 C：Redis 故障失败关闭 ---")
    c = Conn()
    cap, _ = get_captcha(c, new_cid())
    check("C1 Redis 不可达时验证码请求回 -1003", cap == -1003, cap)
    u = uid("d")
    ret, _, _ = do_login(c, u, new_cid())
    check("C2 Redis 不可达时登录失败关闭 -1003", ret == -1003, ret)
    r = c.ask(pdu(121, msg=b"a" * 64 + b"\0", dat=new_cid().encode().ljust(64, b"\0")), 122)
    check("C3 Redis 不可达时恢复回 -1003", r and i32(r[0][1]) == -1003)
    check("C4 故障期间无半认证连接（业务 -1001）",
          (lambda rr: rr and i32(rr[0][1]) == -1001)(c.ask(pdu(5, dat=f32("nobody_xxx")), 6)))
    c.close()


def phase_redis_disabled():
    print("--- 阶段 D：Redis 未配置（旧兼容登录可用，无恢复旁路） ---")
    u = uid("e")
    cReg = Conn()
    check("D1 未配置 Redis 旧布局注册成功", do_register(cReg, u) == 0)
    cReg.close()
    c = Conn()
    ret, tok, _ = do_login(c, u)
    check("D2 未配置 Redis 旧布局登录成功且无 token", ret == 0 and tok == "")
    check("D3 未配置 Redis 业务可用", biz_probe(c))
    r = c.ask(pdu(121, msg=b"a" * 64 + b"\0", dat=new_cid().encode().ljust(64, b"\0")), 122)
    check("D4 未配置 Redis 恢复回 -1003（无内存认证旁路）", r and i32(r[0][1]) == -1003)
    c.close()
    time.sleep(0.4)
    exec_sql("DELETE FROM login_record WHERE username=%s", (u,))
    exec_sql("DELETE FROM user_info WHERE name=%s", (u,))
    shutil.rmtree(FS / u, ignore_errors=True)


def cleanup():
    for u in MADE_USERS:
        exec_sql("DELETE FROM login_record WHERE username=%s", (u,))
        exec_sql("DELETE FROM user_info WHERE name=%s", (u,))
        shutil.rmtree(FS / u, ignore_errors=True)
        redis_call("DEL", f"sessions:{u}", f"online:{u}")
    for cid in MADE_CIDS:
        redis_call("DEL", f"session:{cid}",
                   f"cap:{cid}:code", f"cap:{cid}:cool", f"cap:{cid}:fail", f"cap:{cid}:lock")


def main():
    if not EXE.exists():
        print("FATAL: dist/Server.exe 不存在，先构建")
        return 3
    os.environ["CLOUDDISK_CAPTCHA_TEXT_FOR_TESTS"] = "1"   # 测试部署旁路：自动化需要文本答案
    os.environ["CLOUDDISK_REDIS_URL"] = f"redis://{REDIS_HOST}:{REDIS_PORT}"
    ctl = server_ctl.ServerCtl(str(EXE), str(DIST))
    if not ctl.start():
        print("FATAL: " + str(ctl.last_error))
        return 3
    try:
        u1, cid1, token = phase_normal()
        token = phase_restart(u1, cid1, token)
        ctl.stop()
        if not ctl.start():
            print("FATAL: 重启失败 " + str(ctl.last_error))
            return 3
        c = Conn()
        check("B2 服务端重启后旧会话恢复被拒 -3017", do_resume(c, cid1, token)[0] == -3017)
        c.close()
        ctl.stop()
        # 阶段 C：Redis 指向拒绝端口（不动真实 Redis）
        os.environ["CLOUDDISK_REDIS_URL"] = f"redis://{REDIS_HOST}:{REDIS_PORT + 1000}"
        if not ctl.start():
            print("FATAL: 故障场景启动失败 " + str(ctl.last_error))
            return 3
        phase_redis_down()
        ctl.stop()
        # 阶段 D：不配置 Redis
        os.environ.pop("CLOUDDISK_REDIS_URL", None)
        if not ctl.start():
            print("FATAL: 无 Redis 场景启动失败 " + str(ctl.last_error))
            return 3
        phase_redis_disabled()
    finally:
        ctl.stop()
        cleanup()
    fails = [n for n, ok in RESULTS if not ok]
    print(f"== 总结: {len(RESULTS)} 项, FAIL {len(fails)} 项")
    for n in fails:
        print("  FAIL " + n)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
