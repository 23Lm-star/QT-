# -*- coding: utf-8 -*-
# REQ-RESUME-AUDIT-001 M1/M2/M3 定向验证探针（直连 TCP 5000，测试专用账号 rstest_*）
# 前置：CLOUDDISK_CAPTCHA_TEXT_FOR_TESTS=1 的 dist/Server.exe 已启动；MySQL mydb 可达。
# 夹具影响：创建 rstest_* 用户；写 dist/filesys/.staging/*.part；改写该用户自己的 upload_session 行。
import hashlib
import os
import socket
import struct
import subprocess
import time

REPO = r"E:\qt\xiangmu"
DIST = os.path.join(REPO, "dist")
STAGE = os.path.join(DIST, "filesys", ".staging")
MYSQL = [r"C:\Program Files\MySQL\MySQL Server 5.7\bin\mysql.exe",
         "-uroot", "-p123456", "mydb", "-N", "-B", "-e"]

MAGIC = 0x5253554D   # RSUM
UP_SIZE_OFF, UP_RESUME_OFF, UP_MAGIC_OFF, TID_OFF = 32, 32, 40, 48
ACK_NEXT_OFF, ACK_DONE_OFF = 8, 16

T_REGIST, T_LOGIN, T_CAPTCHA = 1, 3, 44
T_INIT, T_DATA = 29, 31

results = []
def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(("[PASS] " if ok else "[FAIL] ") + name + (" :: " + detail if detail else ""))

def mkpdu(t, ca=b"", msg=b""):
    return struct.pack("<III", 76 + len(msg), len(msg), t) + ca.ljust(64, b"\0")[:64] + msg

def recv_pdu(s):
    hdr = b""
    while len(hdr) < 12:
        c = s.recv(12 - len(hdr))
        if not c:
            return None
        hdr += c
    total, mlen, t = struct.unpack("<III", hdr)
    body = b""
    while len(body) < 64 + mlen:
        c = s.recv(64 + mlen - len(body))
        if not c:
            return None
        body += c
    return t, body[:64], body[64:64 + mlen]

class Conn:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", 5000), timeout=20)
    def send(self, t, ca=b"", msg=b""):
        self.s.sendall(mkpdu(t, ca, msg))
    def recv(self):
        return recv_pdu(self.s)
    def close(self):
        try:
            self.s.close()
        except OSError:
            pass

def do_login(c, user):
    for _ in range(6):
        c.send(T_CAPTCHA)
        t, ca, msg = c.recv()
        rc = struct.unpack("<i", ca[:4])[0]
        if rc == 0:
            captcha = msg.split(b"\0")[0].decode("latin1")
            break
        wait = struct.unpack("<i", ca[4:8])[0] + 1
        print("  验证码冷却 %ds，等待…" % wait)
        time.sleep(wait)
    c.send(T_LOGIN, user.encode().ljust(64, b"\0")[:64], captcha.encode())
    t, ca, _ = c.recv()
    return t, struct.unpack("<i", ca[:4])[0]

def sql(q):
    r = subprocess.run(MYSQL + [q], capture_output=True, text=True, encoding="utf-8")
    if r.returncode != 0:
        raise RuntimeError("mysql failed: " + r.stderr.strip()[:200])
    return r.stdout.strip()

USER = "rstest%d" % (int(time.time()) % 100000)
USER_DIR = (DIST + "/filesys/" + USER).replace("\\", "/")   # 29 的 caMsg=物理目录（须在用户根沙箱内）
CHUNK = 64 * 1024
FSIZE = 200000            # 4 块：64K+64K+64K+7680

def staging_path(owner, parent, display, size):
    key = "|".join([owner, parent, display, str(size)]).encode()
    tok = hashlib.sha1(key).hexdigest()
    return os.path.join(STAGE, tok + ".part")

def send_init(c, name, size, tid, dir_path=USER_DIR):
    ca = name.encode().ljust(32, b"\0")[:32]
    ca += struct.pack("<q", size) + struct.pack("<I", MAGIC)
    ca = ca.ljust(TID_OFF, b"\0") + struct.pack("<Q", tid)
    c.send(T_INIT, ca, dir_path.encode())
    t, ca, _ = c.recv()
    code = struct.unpack("<i", ca[:4])[0]
    has_magic = struct.unpack("<I", ca[UP_MAGIC_OFF:UP_MAGIC_OFF + 4])[0] == MAGIC
    off = struct.unpack("<q", ca[UP_RESUME_OFF:UP_RESUME_OFF + 8])[0] if has_magic else 0
    return t, code, off

def send_chunk(c, tid, off, data):
    # 31 布局：chunkOffset[0,8) + …[8,32)保留 + RSUM[40,44) + tid[48,56)
    ca = struct.pack("<q", off) + b"\0" * 32 + struct.pack("<I", MAGIC)
    ca = ca.ljust(TID_OFF, b"\0") + struct.pack("<Q", tid)
    c.send(T_DATA, ca, data)
    t, ca, _ = c.recv()
    code = struct.unpack("<i", ca[:4])[0]
    nxt = struct.unpack("<q", ca[ACK_NEXT_OFF:ACK_NEXT_OFF + 8])[0]
    done = ca[ACK_DONE_OFF] != 0
    return t, code, nxt, done

def upload_from(c, tid, payload, start, end):
    off = start
    while off < end:
        n = min(CHUNK, end - off)
        t, code, nxt, done = send_chunk(c, tid, off, payload[off:off + n])
        if code != 0:
            return off, code, nxt, done
        off = nxt
    return off, code, nxt, done

# ---------- 注册登录 ----------
c = Conn()
c.send(T_CAPTCHA)
t, ca, msg = c.recv()
captcha = msg.split(b"\0")[0].decode("latin1")
check("验证码获取", t == 45 and struct.unpack("<i", ca[:4])[0] == 0, "len=%d" % len(captcha))
name_ca = USER.encode().ljust(64, b"\0")[:64]
c.send(T_REGIST, name_ca, captcha.encode())
t, ca, _ = c.recv()
check("注册", t == 2 and struct.unpack("<i", ca[:4])[0] == 0, "user=" + USER)
c.send(T_LOGIN, name_ca)
t, ca, _ = c.recv()
t, rc = do_login(c, USER)
check("登录", t == 4 and rc == 0, "rc=%d" % rc)

payload = os.urandom(FSIZE)

# ---------- 阶段一：传 2 块后断线（不优雅关闭，模拟崩溃） ----------
t, code, off = send_init(c, "m4.bin", FSIZE, 1001)
check("29 初始化受理", t == 30 and code == 0 and off == 0, "code=%d off=%d" % (code, off))
off, code, nxt, done = upload_from(c, 1001, payload, 0, 2 * CHUNK)
check("断线前已确认 2 块", off == 2 * CHUNK, "off=%d code=%d" % (off, code))
c.close()

part = staging_path(USER, "/", "m4.bin", FSIZE)
check("暂存文件定位", os.path.isfile(part), part)
phys = os.path.getsize(part)
check("暂存物理=已确认", phys == 2 * CHUNK, "phys=%d" % phys)

# 模拟崩溃半块方向：DB 节流滞后（confirmed=1 块），物理超前 + 500B 尾部垃圾
sql("UPDATE upload_session SET confirmed_offset=%d WHERE owner_name='%s' AND staging_path='%s'"
    % (CHUNK, USER, part.replace("\\", "/")))

# ---------- T1：物理尾部追加垃圾 → 29 起点必须被 DB 封顶 ----------
with open(part, "ab") as f:
    f.write(b"\xde\xad\xbe\xef" * 125)          # +500B 垃圾
phys2 = os.path.getsize(part)
c = Conn()
t, rc = do_login(c, USER)
t, code, off = send_init(c, "m4.bin", FSIZE, 1002)
check("T1 起点按 confirmed_offset 封顶（物理超前方向）", code == 0 and off == CHUNK,
      "phys=%d db=%d got=%d（期望 %d）" % (phys2, CHUNK, off, CHUNK))

# ---------- T2：复用路径不得清空 client_sha256 ----------
poison = "ff" * 32
sql("UPDATE upload_session SET client_sha256='%s' WHERE owner_name='%s' AND staging_path='%s'"
    % (poison, USER, part.replace("\\", "/")))
# 再走一次 29（新 tid）触发 restartUploadSession
t, code, off = send_init(c, "m4.bin", FSIZE, 1003)
sha_now = sql("SELECT client_sha256 FROM upload_session WHERE owner_name='%s' AND staging_path='%s'"
              % (USER, part.replace("\\", "/")))
check("T2 复用后摘要保留", code == 0 and sha_now == poison,
      "db_sha=%s" % sha_now[:16])
check("T2 起点仍按封顶", off == CHUNK, "off=%d（期望 %d）" % (off, CHUNK))

# ---------- T3：毒摘要下传完 → 最终 32 必须失败（转正被拒且客户端可见） ----------
off, code, nxt, done = upload_from(c, 1003, payload, off, FSIZE)
check("T3 毒摘要下最终 32 = 失败", code != 0,
      "final code=%d done=%d nxt=%d" % (code, done, nxt))
status = sql("SELECT status FROM upload_session WHERE owner_name='%s' AND staging_path='%s'"
             % (USER, part.replace("\\", "/")))
check("T3 会话状态 failed", status == "failed", "status=%s" % status)
c.close()

# ---------- T1c：无会话行但有 .part 残留 → 起点为 0（审计修复判别用例） ----------
# 修复前：无行 → cap=-1 → 按物理大小续传（off=2000）；修复后：cap=0 → Truncate 重建。
F4 = 5000
part4 = staging_path(USER, "/", "m4c.bin", F4)
os.makedirs(STAGE, exist_ok=True)
with open(part4, "wb") as f:
    f.write(b"stale" * 400)                      # 2000B 残留，从无会话行
c = Conn()
t, rc = do_login(c, USER)
t, code, off = send_init(c, "m4c.bin", F4, 3001)
check("T1c 无行残留 → 起点为 0", code == 0 and off == 0,
      "off=%d（修复前按物理续传 off=2000）" % off)
c.close()

# ---------- T3b：尾部即完成路径（DeferredInit30 迟到补发 30） ----------
F2 = 5000
payload2 = os.urandom(F2)
c = Conn()
t, rc = do_login(c, USER)
t, code, off = send_init(c, "m4b.bin", F2, 2001)
check("第二文件初始化", code == 0 and off == 0, "off=%d" % off)
t, code, nxt, done = send_chunk(c, 2001, 0, payload2[:3000])
check("第二文件首块", code == 0 and nxt == 3000, "nxt=%d" % nxt)
c.close()
part2 = staging_path(USER, "/", "m4b.bin", F2)
with open(part2, "ab") as f:
    f.write(payload2[3000:])                     # 手工补齐到总长（模拟断点已在尾部）
# 模拟"已确认到尾、转正前断线"：row confirmed=FSIZE（S10 完成态口径），cap 才会等于总长
sql("UPDATE upload_session SET confirmed_offset=%d WHERE owner_name='%s' AND staging_path='%s'"
    % (F2, USER, part2.replace("\\", "/")))
c = Conn()
t, rc = do_login(c, USER)
t, code, off = send_init(c, "m4b.bin", F2, 2002)
# 新行为：30 不即时回，等复算落定后迟到补发 —— recv 下一条即补发的 30
check("T3b 尾部完成 30 迟到补发（成功）", t == 30 and code == 0 and off == F2,
      "type=%d code=%d off=%d" % (t, code, off))
row = sql("SELECT status FROM upload_session WHERE owner_name='%s' AND display_name='m4b.bin' ORDER BY session_id DESC LIMIT 1"
          % USER)
check("T3b 会话 done", row == "done", "status=%s" % row)
c.close()

# ---------- T3c：毒摘要 + 暂存已在尾部 → 迟到 30 必须回失败（判别 M3 新通道） ----------
# 改前实现：29 即时回 30 code=0 off=总长（假成功）；修复后：30 在转正落定后按结果补发。
F3 = 70000
payload3 = os.urandom(F3)
c = Conn()
t, rc = do_login(c, USER)
t, code, off = send_init(c, "m4d.bin", F3, 4001)
check("T3c 初始化", code == 0 and off == 0, "off=%d" % off)
t, code, nxt, done = send_chunk(c, 4001, 0, payload3[:CHUNK])
check("T3c 首块", code == 0 and nxt == CHUNK, "nxt=%d" % nxt)
c.close()
part3 = staging_path(USER, "/", "m4d.bin", F3)
with open(part3, "ab") as f:
    f.write(payload3[CHUNK:])                    # 手工补齐到总长
sql("UPDATE upload_session SET client_sha256='%s', confirmed_offset=%d "
    "WHERE owner_name='%s' AND staging_path='%s'"
    % (poison, F3, USER, part3.replace("\\", "/")))
c = Conn()
t, rc = do_login(c, USER)
t, code, off = send_init(c, "m4d.bin", F3, 4002)
check("T3c 尾部完成+毒摘要 → 迟到 30 回失败", t == 30 and code != 0,
      "type=%d code=%d off=%d" % (t, code, off))
row = sql("SELECT status FROM upload_session WHERE owner_name='%s' AND display_name='m4d.bin' "
          "ORDER BY session_id DESC LIMIT 1" % USER)
check("T3c 会话 failed", row == "failed", "status=%s" % row)
c.close()

print("==== 汇总: %d/%d ====" % (sum(1 for _, ok, _ in results if ok), len(results)))
sys_exit = 0 if all(ok for _, ok, _ in results) else 1
raise SystemExit(sys_exit)
