# REQ-EXT-PERF-002 服务端复算哈希移出主线程：针对性验收套件。
# 运行形态遵循 project-docs/evidence/E2E-RUNFORM.md：cwd 任意；脚本自管理 dist/Server.exe（按 PID），
# 串行独占 127.0.0.1:5000，DB = mydb（mysql CLI），运行树 = dist/filesys。
#
# 覆盖 PRD 验收项：
#   竞态三  P2-04 复算在途 107 → 延迟中止，无幽灵条目
#   竞态四  P2-05 复算在途断线 → 结果丢弃/状态一致，服务存活
#   守卫五  P2-02 摘要不符 → 拒绝转正、staging 删除、语义与改前一致
#   性能一  P2-06 256MiB 复算在途，第二连接 FLUSH 延迟 < 120ms（改前同步复算必 >300ms）
#   迟到应答 P2-01(32) / P2-03(98) / P2-07(102) 三条转正路径端到端
import hashlib, os, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_CAP, T_CAP_R, T_LOGIN, T_LOGIN_R = 1, 44, 45, 3, 4
T_MKDIR, T_MKDIR_R, T_FLUSH, T_FLUSH_R = 19, 20, 23, 24
T_UPINIT, T_UPDATA, T_UPINIT_R, T_UPDATA_R = 29, 31, 30, 32
T_FC, T_FC_R = 97, 98
T_ABORT, T_ABORT_R = 107, 108
T_GCREATE, T_GCREATE_R = 60, 61
T_GF_INIT, T_GF_INIT_R = 101, 102
MAGIC = 0x5253554d
FCHK_MAGIC, FCHK_VERSION = 0x4643484b, 1
ID_OFF = 48
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
STAGING = os.path.join(DIST, "filesys", ".staging")
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl
_SRV = server_ctl.ServerCtl(EXE, DIST, HOST, PORT)

U = "pf2user"                     # 主测试账号
P = "pf2probe"                    # 第二连接账号（同账号互踢，探针必须用另一账号）
DST = f"./filesys/{U}/p1"
SHA = lambda b: hashlib.sha256(b).hexdigest()
CHUNK = 1024 * 1024

def f32(s): return s.encode("utf-8")[:31].ljust(32, b"\0")
def creds(n, p="123456"): return f32(n) + hashlib.md5(p.encode()).hexdigest().encode()
def pdu(t, msg=b"", dat=b""):
    dat = dat[:64].ljust(64, b"\0")
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat + msg
def u16(x): return struct.pack("<H", x)
def u32(x): return struct.pack("<I", x)
def u64(x): return struct.pack("<Q", x)
def i64(x): return struct.pack("<q", x)
def string(x): return u32(len(x)) + x
def u32at(b, o): return struct.unpack("<I", b[o:o + 4])[0]
def u64at(b, o): return struct.unpack("<Q", b[o:o + 8])[0]
def i64at(b, o): return struct.unpack("<q", b[o:o + 8])[0]
def eid(dat, off=ID_OFF):
    v = u64at(dat, off)
    return v - (1 << 64) if v >= (1 << 63) else v
def envelope(gid, body=b""):
    return u16(1) + u16(20) + u64(int(time.time_ns() & ((1 << 63) - 1))) + u64(gid) + body

class Conn:
    def __init__(s):
        s.sock = socket.create_connection((HOST, PORT), timeout=10)
        s.buf = b""; s.pending = []
    def pump(s, t):
        s.sock.settimeout(t)
        try:
            d = s.sock.recv(1 << 20)
            if d: s.buf += d
        except (socket.timeout, OSError): pass
    def packets(s):
        out = []
        while len(s.buf) >= HDR:
            tl, ml, ty = struct.unpack("<III", s.buf[:12])
            if tl < HDR or tl != HDR + ml or len(s.buf) < tl: break
            out.append((ty, s.buf[12:76], s.buf[76:tl])); s.buf = s.buf[tl:]
        s.pending.extend(out); return out
    def wait(s, ty, timeout=8.0):
        dl = time.time() + timeout
        while time.time() < dl:
            for i, p in enumerate(s.pending):
                if p[0] == ty: return s.pending.pop(i)
            s.pump(0.2); s.packets()
        return None
    def take(s, t=0.1):
        s.pump(t); s.packets(); out = s.pending[:]; s.pending = []; return out
    def send(s, pkt): s.sock.sendall(pkt)
    def close(s):
        try: s.sock.close()
        except OSError: pass

def sql(q):
    return subprocess.run([MYSQL] + DB + [q], capture_output=True, text=True).stdout.strip()

def login_conn(name):
    c = Conn()
    c.send(pdu(T_CAP)); r = c.wait(T_CAP_R)
    code = r[2].split(b"\0")[0] if r else b""
    c.send(pdu(T_REG, msg=code + b"\0", dat=creds(name))); c.wait(T_REG + 1, 4)
    c.close()
    c = Conn()
    c.send(pdu(T_CAP)); r = c.wait(T_CAP_R); code = r[2].split(b"\0")[0] if r else b""
    c.send(pdu(T_LOGIN, msg=code + b"\0", dat=creds(name))); r = c.wait(T_LOGIN_R, 6)
    ret = struct.unpack("<i", r[1][:4])[0] if r else -99
    return c, ret

def mkdir(c, parent, name):
    c.send(pdu(T_MKDIR, msg=parent.encode(), dat=f32(name)))
    return c.wait(T_MKDIR_R)

def up_init(c, tid, dirPath, name, size):
    c.send(pdu(T_UPINIT, msg=dirPath.encode(),
               dat=f32(name) + i64(size) + u32(MAGIC) + b"\0" * 4 + u64(tid)))
    return c.wait(T_UPINIT_R)

def up_chunk(c, tid, off, data, wait=True):
    c.send(pdu(T_UPDATA, msg=data,
               dat=i64(off) + b"\0" * 32 + u32(MAGIC) + b"\0" * 4 + u64(tid)))
    return c.wait(T_UPDATA_R) if wait else None

def ack_ok(r): return r is not None and r[1][0] != 0
def ack_next(r): return i64at(r[1], 8) if r else -1
def ack_complete(r): return bool(r[1][16]) if r else False

def abort(c, tid):
    c.send(pdu(T_ABORT, dat=b"\0" * ID_OFF + u64(tid)))
    return c.wait(T_ABORT_R)

def fc(c, sha, name, dirPath, size, reqId):
    nb, db, rb = name.encode(), dirPath.encode(), reqId.encode()
    c.send(pdu(T_FC, msg=sha.encode() + nb + db + rb,
               dat=u32(FCHK_VERSION) + u32(FCHK_MAGIC) + i64(size) + u32(64)
                   + u32(len(nb)) + u32(len(db)) + u32(len(rb))))
    return c.wait(T_FC_R, 12)

def gf_init(c, tid, gid, size, sha, name):
    c.send(pdu(T_GF_INIT, msg=envelope(gid, u64(size) + string(sha.encode()) + string(name.encode())),
               dat=b"\0" * ID_OFF + u64(tid)))
    return c.wait(T_GF_INIT_R, 12)

def flush_names(c, dirpath, timeout=6.0):
    c.sock.sendall(pdu(T_FLUSH, msg=dirpath.encode()))
    dl = time.time() + timeout
    while time.time() < dl:
        for ty, dat, msg in c.take(0.2):
            if ty != T_FLUSH_R: continue
            return sorted(msg[i * 36:i * 36 + 36][:32].split(b"\0")[0].decode("utf-8", "replace")
                          for i in range(len(msg) // 36))
    return None

def sess(display):
    out = sql(f"SELECT status FROM upload_session WHERE owner_name='{U}' AND display_name='{display}' "
              f"ORDER BY session_id DESC LIMIT 1")
    return out.splitlines()[0].strip() if out else ""

def entry_active(display, parent="/p1/"):
    return sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{U}' AND parent_path='{parent}' "
               f"AND display_name='{display}' AND status='active'") == "1"

def obj_ready(sha):
    return sql(f"SELECT COUNT(*) FROM file_object WHERE sha256='{sha}' AND status='ready'") == "1"

def part_path(owner, parent, name, size):
    key = f"{owner}|{parent}|{name}|{size}".encode("utf-8")
    return os.path.join(STAGING, hashlib.sha1(key).hexdigest() + ".part")

def write_part(owner, parent, name, payload):
    p = part_path(owner, parent, name, len(payload))
    os.makedirs(STAGING, exist_ok=True)
    with open(p, "wb") as f:
        f.write(payload)
    return p

results = []
def check(sid, desc, cond, detail=""):
    results.append((sid, desc, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {sid} {desc} :: {detail}")

# ---------- 确定性测试数据 ----------
PL1 = (b"PF2-01-ASYNC-COMMIT-" * 200000)[:3 * CHUNK]                  # 3 MiB
PL_BAD = (b"PF2-02-MISMATCH-" * 250000)[:2 * CHUNK]                   # 2 MiB（声明摘要故意为错）
PL_TAIL = (b"PF2-03-TAIL97-" * 250000)[:2 * CHUNK]                    # 2 MiB（断点已在尾）
PL_RACE = (b"PF2-04-RACE192-" + b"\0" * (CHUNK - 15)) * 192            # 192 MiB（复算窗口确定性）
PL_GONE = (b"PF2-05-GONE192-" + b"\0" * (CHUNK - 15)) * 192            # 192 MiB
BLK = (b"PF2-06-BIG256-" + b"\0" * (CHUNK - 13))                      # 1 MiB 重复块
PL_BIG = BLK * 256                                                     # 256 MiB
PL_GRP = (b"PF2-07-GROUP102-" * 500000)[:8 * CHUNK]                   # 8 MiB
S1, SBAD, STAIL, SRACE, SGONE, SBIG, SGRP = (SHA(x) for x in
                                            (PL1, PL_BAD, PL_TAIL, PL_RACE, PL_GONE, PL_BIG, PL_GRP))
NAMES = ("pf201.bin", "pf202bad.bin", "pf203tail.bin", "pf204race.bin",
         "pf205gone.bin", "pf206big.bin", "pf207grp.bin")
SHAS = (S1, SBAD, STAIL, SRACE, SGONE, SBIG, SGRP)

def cleanup():
    """前置/后置等价清理：只动本套件账号与本组 sha 的数据。
    顺序必须先是引用表（user_file/group_file）后 file_object——同一 DELETE 里多 sha
    批量删除受外键约束，任何一条仍被引用都会让整条语句原子失败、全部残留。"""
    shalist = ",".join(f"'{s}'" for s in SHAS)
    sql(f"DELETE gf FROM group_file gf WHERE gf.object_id IN "
        f"(SELECT object_id FROM file_object WHERE sha256 IN ({shalist}));")
    sql(f"DELETE uf FROM user_file uf WHERE uf.object_id IN "
        f"(SELECT object_id FROM file_object WHERE sha256 IN ({shalist}));")
    sql(f"DELETE uf FROM user_file uf WHERE uf.owner_name IN ('{U}','{P}');")
    sql(f"DELETE gf FROM group_file gf JOIN `group` g ON g.group_id=gf.group_id "
        f"WHERE gf.uploader_name='{U}';")
    sql(f"DELETE us FROM upload_session us WHERE us.owner_name IN ('{U}','{P}');")
    for row in sql(f"SELECT storage_path FROM file_object WHERE sha256 IN ({shalist})").splitlines():
        p = os.path.join(DIST, row.strip())
        try: os.remove(p)
        except OSError: pass
    sql(f"DELETE FROM file_object WHERE sha256 IN ({shalist});")
    for owner, parent, name, size in (
            (U, "/p1/", NAMES[2], len(PL_TAIL)), (U, "/p1/", NAMES[5], len(PL_BIG)),
            (U, "/", NAMES[6], len(PL_GRP)),
            (U, "/p1/", NAMES[3], len(PL_RACE)), (U, "/p1/", NAMES[4], len(PL_GONE))):
        try: os.remove(part_path(owner, parent, name, size))
        except OSError: pass

# ================= 启动 =================
cleanup()
assert _SRV.start(), f"Server 启动失败: {_SRV.last_error}"
c, ret = login_conn(U)
check("P2-00-login", f"{U} 注册登录", ret >= 0, f"ret={ret}")
mkdir(c, f"./filesys/{U}", "p1")
check("P2-00-mkdir", "目录 p1 建立", True, "")

# ================= P2-01：29/31 末块 → 异步转正 → 迟到 32 =================
print("=== P2-01 正常转正（32 迟到回）===")
r = up_init(c, 1101, DST, NAMES[0], len(PL1))
check("P2-01-init", "29 受理", r is not None and r[1][0] != 0, f"init={r is not None}")
last = None
for i in range(0, len(PL1), CHUNK):
    last = up_chunk(c, 1101, i, PL1[i:i + CHUNK])
check("P2-01-ack", "末块 32 success=1 complete=1 next=size（复算落定后才回）",
      ack_ok(last) and ack_complete(last) and ack_next(last) == len(PL1),
      f"ok={ack_ok(last)} comp={ack_complete(last)} next={ack_next(last)}")
time.sleep(0.5)
check("P2-01-entry", "条目 active + 对象 ready + 会话 done",
      entry_active(NAMES[0]) and obj_ready(S1) and sess(NAMES[0]) == "done",
      f"entry={entry_active(NAMES[0])} obj={obj_ready(S1)} sess={sess(NAMES[0])}")

# ================= P2-02：守卫五 —— 异步路径摘要不符 ===print("=== P2-02 守卫：97 声明错摘要 → 真实内容上传 → 拒绝转正 ===")
rq = "pf2r2a0001"
r = fc(c, SHA(b"WRONG-DIGEST-FOR-GUARD-TEST-0000000000"), NAMES[1], DST, len(PL_BAD), rq)
miss = r is not None and ack_ok(r) and r[1][1] == 0
r = up_init(c, 1201, DST, NAMES[1], len(PL_BAD))
last = None
for i in range(0, len(PL_BAD), CHUNK):
    last = up_chunk(c, 1201, i, PL_BAD[i:i + CHUNK])
time.sleep(0.6)
check("P2-02-reject", "复算不符：会话 failed、无条目、无对象（错误语义同改前）",
      miss and sess(NAMES[1]) == "failed" and not entry_active(NAMES[1]) and not obj_ready(SBAD),
      f"miss={miss} sess={sess(NAMES[1])} entry={entry_active(NAMES[1])}")
pp = part_path(U, "/p1/", NAMES[1], len(PL_BAD))
check("P2-02-staging-removed", "不符暂存 .part 已删除（同改前守卫处置）", not os.path.exists(pp), pp)

# ================= P2-03：97 断点回放到尾 → 迟到 98（hit=1）=================
print("=== P2-03 97 到尾复算（98 迟到回）===")
part = write_part(U, "/p1/", NAMES[2], PL_TAIL)
rq3 = "pf2r3a0001"
r = fc(c, STAIL, NAMES[2], DST, len(PL_TAIL), rq3)
f8 = None
if r:
    nameLen = u32at(r[1], 32)
    f8 = dict(ret=r[1][0] != 0, hit=r[1][1] != 0,
              fileId=i64at(r[1], 8), nextOffset=i64at(r[1], 24),
              name=r[2][:nameLen].decode(), echo=r[2][nameLen:].decode())
check("P2-03-late98", "98 迟到回 hit=1 fileId>0 next=size 且回带 reqId",
      f8 is not None and f8["ret"] and f8["hit"] and f8["fileId"] > 0
      and f8["nextOffset"] == len(PL_TAIL) and f8["echo"] == rq3, f"{f8}")
check("P2-03-state", "条目 active + 对象 ready + 会话 done",
      entry_active(NAMES[2]) and obj_ready(STAIL) and sess(NAMES[2]) == "done",
      f"sess={sess(NAMES[2])}")

# ================= P2-04 竞态三：复算在途 107 → 延迟中止（确定性大文件窗口）=================
# 29 INIT 命中"暂存已在尾"→ 即时回 30、整文件复算在途（192MiB ≈ 250-500ms 窗口）；
# 随后立即 107 → 必然落在 hashInFlight 期 → pendingAbort → 落定后执行延迟中止。
print("=== P2-04 复算在途 107（竞态三）===")
write_part(U, "/p1/", NAMES[3], PL_RACE)
r = up_init(c, 1401, DST, NAMES[3], len(PL_RACE))
r108 = abort(c, 1401)
time.sleep(2.5)
st, ea = sess(NAMES[3]), entry_active(NAMES[3])
check("P2-04-abort", "108 ret=true → 会话 expired/failed、无幽灵条目、对象未建",
      r108 is not None and r108[1][0] != 0 and st in ("expired", "failed")
      and not ea and not obj_ready(SRACE),
      f"108={r108 and r108[1][0]} sess={st} entry={ea} obj={obj_ready(SRACE)}")

# ================= P2-05 竞态四：复算在途断线 → 结果丢弃；重放收敛 =================
print("=== P2-05 复算在途断线（竞态四）===")
write_part(U, "/p1/", NAMES[4], PL_GONE)
r = up_init(c, 1501, DST, NAMES[4], len(PL_GONE))
c.close()                                        # 30 已回、复算在途：结果将路由到已消失连接
time.sleep(2.0)
c2, ret2 = login_conn(U)
check("P2-05-alive", "断线复算后服务存活（重登成功、无崩溃）", ret2 >= 0, f"ret={ret2}")
st, ea = sess(NAMES[4]), entry_active(NAMES[4])
check("P2-05-discarded", "迟到复算被丢弃：会话停在 verifying、无幽灵条目/对象",
      st == "verifying" and not ea and not obj_ready(SGONE), f"sess={st} entry={ea}")
r = up_init(c2, 1502, DST, NAMES[4], len(PL_GONE))  # 重连同参数重放 → 再次到尾复算 → 正常落定
time.sleep(3.0)
st, ea = sess(NAMES[4]), entry_active(NAMES[4])
check("P2-05-replay-converges", "重放收敛：会话 done、条目 active、对象 ready",
      st == "done" and ea and obj_ready(SGONE), f"sess={st} entry={ea}")

# ================= P2-06 性能一：256MiB 复算在途，第二连接响应性 =================
print("=== P2-06 复算在途第二连接延迟（性能一代理指标）===")
pc, ret = login_conn(P)                         # 探针连接：另一账号（同账号互踢）
ppath = write_part(U, "/p1/", NAMES[5], PL_BIG)
rq6 = "pf2r6a0001"
cs = c2                                          # U 连接发起 97
cs.sock.sendall(pdu(T_FC, msg=SBIG.encode() + NAMES[5].encode() + DST.encode() + rq6.encode(),
                    dat=u32(FCHK_VERSION) + u32(FCHK_MAGIC) + i64(len(PL_BIG)) + u32(64)
                        + u32(len(NAMES[5])) + u32(len(DST)) + u32(len(rq6))))
time.sleep(0.12)                                 # 让服务端完成 97 进入、复算已在途
t0 = time.perf_counter()
names = flush_names(pc, f"./filesys/{P}", timeout=8.0)
probe_ms = (time.perf_counter() - t0) * 1000
r = cs.wait(T_FC_R, timeout=15)
late_hit = r is not None and r[1][0] != 0 and r[1][1] != 0 and i64at(r[1], 24) == len(PL_BIG)
check("P2-06-probe", f"复算在途第二连接 FLUSH 延迟 {probe_ms:.0f}ms < 120ms"
      "（改前主线程同步复算 256MiB ≈ 300-600ms，必然超阈）",
      names is not None and probe_ms < 120.0, f"latency={probe_ms:.0f}ms")
check("P2-06-late98", "256MiB 复算落定后 98 迟到回 hit=1", late_hit,
      f"98={r is not None and r[1][0] != 0}")
check("P2-06-state", "大文件条目 active + 对象 ready",
      entry_active(NAMES[5]) and obj_ready(SBIG), "")

# ================= P2-07：群 101 到尾 → 迟到 102（hit=1 + 群引用）=================
print("=== P2-07 群 101 到尾复算（102 迟到回）===")
c3, ret = login_conn(U)
c3.send(pdu(T_GCREATE, msg=envelope(0, string(b"pf2grp"))))
r = c3.wait(T_GCREATE_R)
GID = u64at(r[2], 24) if r and u32at(r[2], 20) == 0 else 0
check("P2-07-group", "测试群建立", GID > 0, f"gid={GID}")
write_part(U, "/", NAMES[6], PL_GRP)
r = gf_init(c3, 1701, GID, len(PL_GRP), SGRP, NAMES[6])
code = u32at(r[2], 20) if r else -999
hit = r is not None and len(r[2]) > 24 and r[2][24] == 1
gfid = u64at(r[2], 36) if r else 0
nxt = i64at(r[2], 52) if r else -1
check("P2-07-late102", "102 迟到回 code=0 hit=1 groupFileId>0 next=size 且回带 transfer_id",
      r is not None and code == 0 and hit and gfid > 0 and nxt == len(PL_GRP) and eid(r[1]) == 1701,
      f"code={code} hit={hit} gfid={gfid} next={nxt}")
gr = sql(f"SELECT COUNT(*) FROM group_file WHERE group_id={GID} AND group_file_id={gfid}")
check("P2-07-state", "群引用行存在 + 会话 done", gr == "1" and sess(NAMES[6]) == "done",
      f"gf_rows={gr} sess={sess(NAMES[6])}")

# ================= 汇总与清理 =================
pc.close(); c3.close()
stop_server_ok = _SRV.stop()
cleanup()
print("\n=== 汇总 ===")
passed = sum(1 for _, _, ok in results if ok)
for sid, desc, ok in results:
    if not ok: print(f"  FAIL -> {sid} {desc}")
print(f"{passed}/{len(results)} passed")
sys.exit(0 if passed == len(results) else 1)
