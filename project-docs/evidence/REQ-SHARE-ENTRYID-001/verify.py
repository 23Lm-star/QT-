# -*- coding: utf-8 -*-
# REQ-SHARE-ENTRYID-001 分享链接源定位改绑条目标识 端到端定向套件。
#
# 覆盖面（口径见 docs/changes/REQ-SHARE-ENTRYID-001/记录.md）：
#   S1     表结构/迁移脚本静态检查（file_id 列、存量清空）
#   E1     创建链接后重命名源 → 链接仍可解析、按新路径查询有效（不再联动平移）
#   E2     移动源到子目录 → 链接仍可解析、按新路径查询有效
#   E3     删除所在目录（子树软删） → 链接行同事务清理、解析失败
#   E4     好友分享邀请后源条目改名 → 接收者同意仍按条目标识放行
#   E5     同一握手重复接受 → 拒绝（授权一次性消费）
#
# 运行形态同 REQ-EXT-SHARE-001：脚本自管服务端生命周期；Server.exe 取 <repo>/dist/Server.exe。
# 运行：py -3.13 -X utf8 project-docs/evidence/REQ-SHARE-ENTRYID-001/verify.py
# 依赖：CLOUDDISK_CAPTCHA_TEXT_FOR_TESTS=1（文本旁路）、CLOUDDISK_REDIS_URL 指向可用 Redis
# （登录携 clientId 建会话，未配置时升级客户端登录失败关闭）。
import hashlib
import os
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

os.environ["CLOUDDISK_CAPTCHA_TEXT_FOR_TESTS"] = "1"
os.environ.setdefault("CLOUDDISK_REDIS_URL", "redis://127.0.0.1:6379")

HOST, PORT, HDR, PWD = "127.0.0.1", 5000, 76, "123456"
T_CAP, T_CAP_R, T_REG, T_REG_R, T_LOGIN, T_LOGIN_R = 44, 45, 1, 2, 3, 4
T_MKDIR, T_MKDIR_R = 19, 20
T_DEL, T_DEL_R = 25, 26
T_REN, T_REN_R = 27, 28
T_UP_INIT, T_UP_INIT_R, T_UP_DATA, T_UP_DATA_R = 29, 30, 31, 32
T_SHARE, T_SHARE_R, T_ACCEPT, T_ACCEPT_R = 33, 34, 36, 37
T_CPL, T_CPL_R, T_RES, T_RES_R, T_QUERY, T_QUERY_R = 52, 53, 54, 55, 56, 57
ST_VALID, ST_LINK_INVALID, ST_WRITE_FAILED = 0, -2019, -4006
UPLOAD_MAGIC = 0x5253554D   # "RSUM"

OWNER, OTHER = "se1user", "se1other"
RESULTS = []


def check(label, cond, detail=""):
    RESULTS.append((label, bool(cond), detail))
    print(("[PASS] " if cond else "[FAIL] ") + label + (" :: " + str(detail) if detail != "" else ""))
    sys.stdout.flush()


def f32(s):
    return s.encode()[:31].ljust(32, b"\0")


def i32(v):
    return struct.pack("<i", v)


def u32(v):
    return struct.pack("<I", v)


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

    def pump(self, timeout=0.4):
        self.s.settimeout(timeout)
        try:
            d = self.s.recv(262144)
            if not d:
                raise ConnectionError("peer closed")
            self.buf += d
        except socket.timeout:
            pass

    def ask(self, pkt, want, timeout=20):
        self.s.sendall(pkt)
        end = time.time() + timeout
        out = []
        while time.time() < end:
            self.pump(0.2)
            ps, self.buf = unpack(self.buf)
            out.extend(ps)
            if any(p[0] == want for p in out):
                return out
        return out

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def code_of(pkt_list, want):
    for ty, ca, _ in pkt_list:
        if ty == want:
            return struct.unpack("<i", ca[:4])[0]
    return None


def db():
    cfg = (REPO / "Server" / "connect.config").read_text(encoding="utf-8").splitlines()
    return pymysql.connect(host=cfg[3], port=int(cfg[4]), user=cfg[6], password=cfg[7],
                           database=cfg[5], autocommit=True)


def q1(sql, args=()):
    with db() as c, c.cursor() as cur:
        cur.execute(sql, args)
        return cur.fetchone()


def exec_sql(sql, args=()):
    with db() as c, c.cursor() as cur:
        cur.execute(sql, args)


def next_cid(seq):
    return "%032x" % (int(time.time() * 1000) * 16 + seq)


def register(name, cid):
    c = Conn()
    cap = c.ask(pdu(T_CAP, dat=cid.encode().ljust(64, b"\0")), T_CAP_R)
    code = cap[0][2].split(b"\0")[0].decode()
    c.ask(pdu(T_REG, msg=code.encode() + b"\0" + cid.encode(),
              dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()), T_REG_R)
    c.close()


def login(name, cid):
    c = Conn()
    cap = c.ask(pdu(T_CAP, dat=cid.encode().ljust(64, b"\0")), T_CAP_R)
    code = cap[0][2].split(b"\0")[0].decode()
    r = c.ask(pdu(T_LOGIN, msg=code.encode() + b"\0" + cid.encode(),
                  dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()), T_LOGIN_R)
    ret = code_of(r, T_LOGIN_R)
    if ret != 0:
        c.close()
        return None
    return c


def upload(c, owner, name, data, tid):
    dat = bytearray(64)
    dat[0:32] = f32(name)
    dat[32:40] = struct.pack("<q", len(data))
    dat[40:44] = u32(UPLOAD_MAGIC)
    dat[48:56] = struct.pack("<q", tid)
    r = c.ask(pdu(T_UP_INIT, msg=("./filesys/" + owner).encode(), dat=bytes(dat)), T_UP_INIT_R)
    assert r and code_of(r, T_UP_INIT_R) == 0, "29 init rejected"
    d = bytearray(64)
    d[40:44] = u32(UPLOAD_MAGIC)
    d[48:56] = struct.pack("<q", tid)
    r = c.ask(pdu(T_UP_DATA, msg=data, dat=bytes(d)), T_UP_DATA_R, timeout=30)
    assert r and code_of(r, T_UP_DATA_R) == 0, "31 upload rejected"


def cpl(c, rel, action=None, seconds=0):
    tail = b""
    if action:
        tail = action.encode() + b"\0" + str(seconds).encode()
    r = c.ask(pdu(T_CPL, msg=rel.encode() + b"\0" + tail), T_CPL_R)
    code = code_of(r, T_CPL_R)
    link = r[0][2].split(b"\0")[0].decode() if r and r[0][2] else ""
    return code, link


def resolve(c, token):
    r = c.ask(pdu(T_RES, msg=token.encode()), T_RES_R, timeout=60)
    code = code_of(r, T_RES_R)
    return code, (r[0][2].decode(errors="replace") if r and r[0][2] else "")


def query(c, rel):
    r = c.ask(pdu(T_QUERY, dat=f32(OWNER), msg=rel.encode()), T_QUERY_R)
    code = code_of(r, T_QUERY_R)
    link = r[0][2].split(b"\0")[0].decode() if r and r[0][2] else ""
    return code, link


def landed_entry(owner, json_txt):
    import re
    m = re.search(r'"saved_path":"([^"]*)"', json_txt or "")
    if not m:
        return False
    name = m.group(1).rsplit("/", 1)[-1]
    return q1("SELECT COUNT(*) FROM user_file WHERE owner_name=%s "
              "AND parent_path='/分享链接/' AND display_name=%s AND status='active'",
              (owner, name))[0] > 0


def token_of(link):
    return link.split("cloud-disk://share/")[-1]


# ---------------- 静态 ----------------
print("=== S 静态 ===")
schema = (REPO / "Server" / "share_link.sql").read_text(encoding="utf-8")
mig = (REPO / "Server" / "migrate_share_entryid_001.sql").read_text(encoding="utf-8")
check("S1 share_link 源定位=file_id 且无 relative_path 列",
      "file_id" in schema and "relative_path" not in schema)
check("S2 迁移脚本存在且为清空口径",
      "DELETE FROM share_link" in mig and "2026112101" in mig and "DROP COLUMN relative_path" in mig)

# ---------------- 环境 ----------------
logf = REPO / "build" / "share_entryid_server.log"
ctl = server_ctl.ServerCtl(str(EXE), str(DIST), log_path=str(logf))
if not ctl.start():
    print("FATAL: " + str(ctl.last_error))
    sys.exit(3)

try:
    for u in (OWNER, OTHER):
        exec_sql("DELETE FROM share_link WHERE owner_name=%s", (u,))
        exec_sql("DELETE FROM user_file WHERE owner_name=%s", (u,))
        exec_sql("DELETE FROM user_info WHERE name=%s", (u,))
    shutil.rmtree(FS / OWNER, ignore_errors=True)
    shutil.rmtree(FS / OTHER, ignore_errors=True)
    register(OWNER, next_cid(1))
    register(OTHER, next_cid(2))
    A = login(OWNER, next_cid(3))
    B = login(OTHER, next_cid(4))
    check("A0 夹具就绪", A is not None and B is not None)
    (FS / OWNER).mkdir(parents=True, exist_ok=True)

    upload(A, OWNER, "f1.bin", b"ENTRYID-CONTENT-1", 901)

    # ---------------- E1 重命名不断链 ----------------
    print("=== E1 重命名不断链 ===")
    code, link1 = cpl(A, "f1.bin")
    tok1 = token_of(link1)
    check("E1.0 首次创建成功", code == ST_VALID and len(tok1) == 22, "code=%s" % code)
    r = A.ask(pdu(T_REN, msg=u32(1) + ("./filesys/" + OWNER).encode(),
                  dat=f32("f1.bin") + f32("f1r.bin")), T_REN_R)
    check("E1.1 重命名成功", code_of(r, T_REN_R) == 0)
    code, link = query(A, "f1r.bin")
    check("E1.2 改名后按新路径查询仍 VALID", code == ST_VALID, "code=%s" % code)
    code, saved = resolve(B, tok1)
    check("E1.3 改名后旧 token 解析仍成功并落地条目",
          code == ST_VALID and landed_entry(OTHER, saved), "code=%s %s" % (code, saved[:60]))

    # ---------------- E2 移动不断链 ----------------
    print("=== E2 移动不断链 ===")
    r = A.ask(pdu(T_MKDIR, msg=("./filesys/" + OWNER).encode(), dat=f32("sub")), T_MKDIR_R)
    check("E2.0 建子目录成功", code_of(r, T_MKDIR_R) == 0)
    # movefile 的 srcRel/dstRel 为 owner 根相对路径（srcRel 含文件名，dstRel 为目标目录）
    mv = "f1r.bin\0sub\0"
    r = A.ask(pdu(38, msg=mv.encode(), dat=f32("f1r.bin")), 39)
    check("E2.1 移动成功", code_of(r, 39) == 0, "code=%s" % code_of(r, 39))
    code, _ = query(A, "sub/f1r.bin")
    check("E2.2 移动后按新路径查询仍 VALID", code == ST_VALID, "code=%s" % code)
    code, saved = resolve(B, tok1)
    check("E2.3 移动后旧 token 解析仍成功", code == ST_VALID, "code=%s" % code)

    # ---------------- E3 目录删除级联失效 ----------------
    print("=== E3 目录删除级联失效 ===")
    r = A.ask(pdu(T_DEL, msg=("./filesys/" + OWNER + "/sub").encode(), dat=u32(0)), T_DEL_R)
    check("E3.0 删除子目录成功", code_of(r, T_DEL_R) == 0)
    rows = q1("SELECT COUNT(*) FROM share_link WHERE owner_name=%s", (OWNER,))[0]
    check("E3.1 链接行已随子树软删清理", rows == 0, "rows=%s" % rows)
    code, _ = resolve(B, tok1)
    check("E3.2 删除后旧 token 解析失败", code == ST_LINK_INVALID, "code=%s" % code)

    # ---------------- E4/E5 握手改名窗口 ----------------
    print("=== E4 握手窗口内改名仍放行 ===")
    upload(A, OWNER, "f2.bin", b"ENTRYID-CONTENT-2", 902)
    invite = f32(OWNER) + i32(1)
    msg33 = f32(OTHER) + ("./filesys/" + OWNER + "/f2.bin").encode() + b"\0"
    A.ask(pdu(T_SHARE, msg=msg33, dat=invite), T_SHARE_R)
    time.sleep(0.5)
    r = A.ask(pdu(T_REN, msg=u32(1) + ("./filesys/" + OWNER).encode(),
                  dat=f32("f2.bin") + f32("f2r.bin")), T_REN_R)
    check("E4.0 邀请后源改名成功", code_of(r, T_REN_R) == 0)
    r = B.ask(pdu(T_ACCEPT, msg=("./filesys/" + OWNER + "/f2.bin").encode() + b"\0",
                  dat=f32(OTHER)), T_ACCEPT_R, timeout=60)
    check("E4.1 接收者按条目标识放行（37=0）", code_of(r, T_ACCEPT_R) == 0)
    # 落地名=邀请时的文件名（提交路径末段），不取改名后的服务端名
    b_rows = q1("SELECT COUNT(*) FROM user_file WHERE owner_name=%s AND display_name=%s "
                "AND status='active'", (OTHER, "f2.bin"))[0]
    check("E4.2 接收者根下按邀请名落地条目", b_rows == 1, "rows=%s" % b_rows)
    r = B.ask(pdu(T_ACCEPT, msg=("./filesys/" + OWNER + "/f2.bin").encode() + b"\0",
                  dat=f32(OTHER)), T_ACCEPT_R, timeout=60)
    check("E5 同一握手重复接受被拒", code_of(r, T_ACCEPT_R) == ST_WRITE_FAILED)
finally:
    ctl.stop()
if logf.exists():
    lines = logf.read_bytes().decode("gbk", errors="replace").splitlines()
    keep = [ln for ln in lines if ("movefile" in ln or "38(" in ln or "39(" in ln or "拒绝" in ln)]
    print("=== movefile log ===")
    for ln in keep[-12:]:
        print(ln)

good = sum(1 for _, o, _ in RESULTS if o)
print("RESULT %d/%d %s" % (good, len(RESULTS), "PASS" if good == len(RESULTS) else "FAIL"))
sys.exit(0 if good == len(RESULTS) else 1)
