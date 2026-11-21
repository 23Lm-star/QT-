# -*- coding: utf-8 -*-
# 群非秒传最终安置复核定向探针（审计修复二：attachGroupFile 事务内前置群状态/成员校验）。
# 场景：C 正控（群正常 → 安置成功）；A 窗口内解散群 → 安置拒绝；
#       B 窗口内上传者被移出群 → 安置拒绝；门完好复核（解散/失权后 101 init 即被拒）。
# 运行前置：以 CLOUDDISK_CAPTCHA_TEXT_FOR_TESTS=1 启动 dist/Server.exe，TCP 5000 无无关服务。
#   py -3.13 -X utf8 project-docs/evidence/REQ-EXT-GROUP-003/group_place_recheck_probe.py
# 夹具影响：创建 gplc* 测试用户/群/group_file/user_file/upload_session 行与
#   dist/filesys/.staging/*.part，仅触碰测试账号自身数据；开跑前按前缀清理上一轮。
import hashlib
import os
import socket
import struct
import subprocess
import sys
import time

HOST, PORT = "127.0.0.1", 5000
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
MAGIC = 0x5253554D                      # RSUM
UP_MAGIC_OFF, TID_OFF, ACK_NEXT_OFF = 40, 48, 8
T_CAPTCHA, T_REGIST, T_LOGIN = 44, 1, 3
T_GCREATE, T_GCREATE_R = 60, 61
T_INVITE, T_INVITE_R = 62, 63
T_ACCEPT, T_ACCEPT_R = 64, 65
T_REMOVE, T_REMOVE_R = 70, 71
T_DISSOLVE, T_DISSOLVE_R = 76, 77
T_GF_INIT, T_GF_INIT_R = 101, 102
T_DATA, T_DATA_R = 31, 32
CHUNK = 64 * 1024
TS = int(time.time())
U1, U2 = "gplc%d_a" % TS, "gplc%d_b" % TS   # U1=群主，U2=成员/上传者
DIST = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "dist"))
NAMES = ("ctrl.bin", "dis.bin", "rm.bin")
PAYLOADS = {n: os.urandom(CHUNK * 3) for n in NAMES}   # 各 3 块，正文随机 → 摘要不互通
_TID = [0x474F4C00]                                    # 客户端自选传输编号基址（GOL 前缀，逐次递增）
def next_tid():
    _TID[0] += 1
    return _TID[0]

results = []
def check(name, ok, detail=""):
    results.append(bool(ok))
    print(("[PASS] " if ok else "[FAIL] ") + name + (" :: " + detail if detail else ""))

def sql(q):
    r = subprocess.run([MYSQL] + DB + [q], capture_output=True, text=True, encoding="utf-8")
    if r.returncode != 0:
        raise RuntimeError("mysql failed: " + (r.stderr or "").strip()[:200])
    return r.stdout.strip()

def mkpdu(t, ca=b"", msg=b""):
    return struct.pack("<III", 76 + len(msg), len(msg), t) + ca.ljust(64, b"\0")[:64] + msg

def recv_pdu(s, want=None):
    deadline = time.time() + 25
    while time.time() < deadline:
        hdr = b""
        while len(hdr) < 12:
            s.settimeout(max(1, deadline - time.time()))
            c = s.recv(12 - len(hdr))
            if not c:
                raise ConnectionError("对端关闭")
            hdr += c
        total, mlen, t = struct.unpack("<III", hdr)
        body = b""
        while len(body) < 64 + mlen:
            s.settimeout(max(1, deadline - time.time()))
            body += s.recv(64 + mlen - len(body))
        ca, msg = body[:64], body[64:64 + mlen]
        if want is None or t == want:
            return t, ca, msg
    raise TimeoutError("等待 type=%s 超时" % want)

def u16(x): return struct.pack("<H", x)
def u32(x): return struct.pack("<I", x)
def u64(x): return struct.pack("<Q", x)
def string(x):
    b = x.encode() if isinstance(x, str) else x
    return u32(len(b)) + b
def envelope(gid, body=b""):
    return u16(1) + u16(20) + u64(int(time.time_ns() & ((1 << 63) - 1))) + u64(gid) + body

def fetch_captcha(s):
    for _ in range(6):
        s.sendall(mkpdu(T_CAPTCHA))
        t, ca, msg = recv_pdu(s)
        if struct.unpack("<i", ca[:4])[0] == 0:
            return msg.split(b"\0")[0].decode("latin1")
        wait = struct.unpack("<i", ca[4:8])[0] + 1
        print("  验证码冷却 %ds，等待…" % wait)
        time.sleep(wait)
    raise RuntimeError("验证码不可用")

def register_login(user):
    s = socket.create_connection((HOST, PORT), timeout=25)
    name = user.encode().ljust(64, b"\0")[:64]   # name[0,32) + 密码槽[32,64)=NUL 明文（同 m4_probe 口径）
    s.sendall(mkpdu(T_REGIST, name, fetch_captcha(s).encode()))
    t, ca, _ = recv_pdu(s)
    rc = struct.unpack("<i", ca[:4])[0]
    if t != 2 or rc != 0:
        raise RuntimeError("注册失败 rc=%d" % rc)
    s.sendall(mkpdu(T_LOGIN, name, fetch_captcha(s).encode()))
    t, ca, _ = recv_pdu(s)
    rc = struct.unpack("<i", ca[:4])[0]
    if t != 4 or rc != 0:
        raise RuntimeError("登录失败 rc=%d" % rc)
    return s

def group(c, typ, rt, gid=0, body=b""):
    c.sendall(mkpdu(typ, b"", envelope(gid, body)))
    t, ca, msg = recv_pdu(c, rt)
    code = struct.unpack("<i", msg[20:24])[0]     # 信封 20B + code i32
    return code, msg

def gf_init(c, gid, size, sha, name, tid):
    """101 初始化（请求 caData[48,56) 须自带 transfer id）。返回 (code, hit, tid, next_offset)。"""
    ca = b"\0" * TID_OFF + struct.pack("<Q", tid)
    c.sendall(mkpdu(T_GF_INIT, ca, envelope(gid, u64(size) + string(sha) + string(name))))
    t, ca, msg = recv_pdu(c, T_GF_INIT_R)
    code = struct.unpack("<i", msg[20:24])[0]
    if code != 0:
        return code, None, 0, 0
    hit = msg[24] != 0
    nxt = struct.unpack("<Q", msg[56:64])[0]
    echo = struct.unpack("<Q", ca[TID_OFF:TID_OFF + 8])[0]
    if echo != tid:
        return -1, None, 0, 0   # 102 回带 tid 与请求不一致视为异常
    return code, hit, tid, nxt

def send_chunk(c, tid, off, data):
    ca = struct.pack("<q", off) + b"\0" * 32 + struct.pack("<I", MAGIC)
    ca = ca.ljust(TID_OFF, b"\0") + struct.pack("<Q", tid)
    c.sendall(mkpdu(T_DATA, ca, data))
    t, ca, _ = recv_pdu(c, T_DATA_R)
    code = struct.unpack("<i", ca[:4])[0]
    nxt = struct.unpack("<q", ca[ACK_NEXT_OFF:ACK_NEXT_OFF + 8])[0]
    return code, nxt

def upload_full(c, gid, name):
    """完整非秒传上传（正控）。返回 (code, tid)。"""
    p = PAYLOADS[name]
    code, hit, tid, nxt = gf_init(c, gid, len(p), hashlib.sha256(p).hexdigest(), name, next_tid())
    if code != 0 or hit:
        return code, tid
    off = 0
    while off < len(p):
        code, nxt = send_chunk(c, tid, off, p[off:off + CHUNK])
        if code != 0:
            return code, tid
        off = nxt
    return 0, tid

def upload_partial(c, gid, name):
    """半程上传：只发前 n-1 块，留末块作为"窗口后最终安置"触发器。返回 (code, tid, next_off)。"""
    p = PAYLOADS[name]
    code, hit, tid, nxt = gf_init(c, gid, len(p), hashlib.sha256(p).hexdigest(), name, next_tid())
    if code != 0 or hit:
        return code, tid, 0
    off = 0
    while off + CHUNK < len(p):
        code, nxt = send_chunk(c, tid, off, p[off:off + CHUNK])
        if code != 0:
            return code, tid, off
        off = nxt
    return 0, tid, off

def staging_path(user, display, size):
    key = "|".join([user, "/", display, str(size)]).encode()
    return os.path.join(DIST, "filesys", ".staging", hashlib.sha1(key).hexdigest() + ".part").replace("\\", "/")

def place_asserts(tag, uid, gid, name):
    """安置被拒的统一断言：无群引用、无个人条目、会话 failed。"""
    p = PAYLOADS[name]
    gf = sql("SELECT COUNT(*) FROM group_file WHERE group_id=%d AND display_name='%s'" % (gid, name))
    uf = sql("SELECT COUNT(*) FROM user_file WHERE owner_name='%s' AND display_name='%s' AND status='active'"
             % (uid, name))
    st = sql("SELECT status FROM upload_session WHERE staging_path='%s'" % staging_path(uid, name, len(p)))
    check(tag + "-群引用未落", gf == "0", "count=%s" % gf)
    check(tag + "-个人条目已撤销", uf == "0", "count=%s" % uf)
    check(tag + "-会话判失败", st == "failed", "status=%s" % st)

# ================= 环境准备 =================
print("=== 环境准备 ===")
for u in (U1, U2):
    sql("DELETE FROM user_file WHERE owner_name='%s';" % u)
    sql("DELETE FROM upload_session WHERE owner_name='%s';" % u)
sql("DELETE gf FROM group_file gf JOIN group_info gi ON gi.group_id=gf.group_id WHERE gi.creator_name LIKE 'gplc%%';")
sql("DELETE FROM group_file WHERE uploader_name LIKE 'gplc%%';")
sql("DELETE FROM user_file WHERE owner_name LIKE 'gplc%%';")
sql("DELETE FROM upload_session WHERE owner_name LIKE 'gplc%%';")
sql("DELETE FROM file_object WHERE sha256 IN (%s);"
    % ",".join("'%s'" % hashlib.sha256(p).hexdigest() for p in PAYLOADS.values()))
sql("DELETE FROM group_invitation WHERE group_id IN (SELECT group_id FROM group_info WHERE creator_name LIKE 'gplc%%');")
sql("DELETE FROM group_member WHERE user_name LIKE 'gplc%%';")
sql("DELETE FROM group_info WHERE creator_name LIKE 'gplc%%';")

s1 = register_login(U1)
s2 = register_login(U2)
check("E0 双账号就绪", True, "%s/%s" % (U1, U2))

# ================= C 正控：群正常 → 非秒传安置成功 =================
print("=== C 正控 ===")
code, msg = group(s1, T_GCREATE, T_GCREATE_R, 0, string("gplc群1"))
GID1 = struct.unpack("<Q", msg[24:32])[0] if code == 0 else 0
check("C0 建群", code == 0 and GID1 > 0, "code=%d gid=%d" % (code, GID1))
code, d = group(s1, T_INVITE, T_INVITE_R, GID1, string(U2) + string("gplc-i1"))
IID = struct.unpack("<Q", d[24:32])[0] if code == 0 else 0
code, d = group(s2, T_ACCEPT, T_ACCEPT_R, 0, u64(IID))
check("C0b 成员加入", code == 0, "code=%d" % code)
code, tid = upload_full(s2, GID1, "ctrl.bin")
check("C1 正控安置成功", code == 0, "code=%d" % code)
check("C2 群引用落库",
      sql("SELECT COUNT(*) FROM group_file WHERE group_id=%d AND display_name='ctrl.bin' AND status='active' AND uploader_name='%s'"
          % (GID1, U2)) == "1", "")
check("C3 个人条目落库",
      sql("SELECT COUNT(*) FROM user_file WHERE owner_name='%s' AND display_name='ctrl.bin' AND status='active'" % U2) == "1", "")
check("C4 会话 done",
      sql("SELECT status FROM upload_session WHERE staging_path='%s'" % staging_path(U2, "ctrl.bin", len(PAYLOADS["ctrl.bin"]))) == "done", "")

# ================= A 解散窗口：末块前解散 → 安置拒绝 =================
print("=== A 解散窗口 ===")
code, tid, off = upload_partial(s2, GID1, "dis.bin")
check("A0 半程受理", code == 0 and off == CHUNK * 2, "code=%d off=%d" % (code, off))
code, msg = group(s1, T_DISSOLVE, T_DISSOLVE_R, GID1)
check("A1 解散受理", code == 0, "code=%d" % code)
code, nxt = send_chunk(s2, tid, off, PAYLOADS["dis.bin"][off:])
check("A2 末块应答失败（安置被拒）", code != 0, "code=%d" % code)
place_asserts("A3", U2, GID1, "dis.bin")
code, hit, tid2, nxt = gf_init(s2, GID1, len(PAYLOADS["ctrl.bin"]),
                               hashlib.sha256(PAYLOADS["ctrl.bin"]).hexdigest(), "after.bin", next_tid())
check("A4 解散后 101 init 被拒（P3 门完好）", code == -2102, "code=%d" % code)

# ================= B 移除窗口：末块前移除上传者 → 安置拒绝 =================
print("=== B 移除窗口 ===")
code, msg = group(s1, T_GCREATE, T_GCREATE_R, 0, string("gplc群2"))
GID2 = struct.unpack("<Q", msg[24:32])[0] if code == 0 else 0
check("B0 二群建立", code == 0 and GID2 > 0, "gid=%d" % GID2)
code, d = group(s1, T_INVITE, T_INVITE_R, GID2, string(U2) + string("gplc-i2"))
IID2 = struct.unpack("<Q", d[24:32])[0] if code == 0 else 0
code, d = group(s2, T_ACCEPT, T_ACCEPT_R, 0, u64(IID2))
check("B0b 成员加入", code == 0, "code=%d" % code)
code, tid, off = upload_partial(s2, GID2, "rm.bin")
check("B1 半程受理", code == 0 and off == CHUNK * 2, "code=%d off=%d" % (code, off))
code, msg = group(s1, T_REMOVE, T_REMOVE_R, GID2, string(U2))
check("B2 移除受理", code == 0, "code=%d" % code)
code, nxt = send_chunk(s2, tid, off, PAYLOADS["rm.bin"][off:])
check("B3 末块应答失败（安置被拒）", code != 0, "code=%d" % code)
place_asserts("B4", U2, GID2, "rm.bin")
code, hit, tid3, nxt = gf_init(s2, GID2, len(PAYLOADS["ctrl.bin"]),
                               hashlib.sha256(PAYLOADS["ctrl.bin"]).hexdigest(), "after2.bin", next_tid())
check("B5 失权后 101 init 被拒（P2 门完好）", code == -2101, "code=%d" % code)

s1.close(); s2.close()
print("==== 汇总: %d/%d ====" % (sum(results), len(results)))
sys.exit(0 if all(results) else 1)
