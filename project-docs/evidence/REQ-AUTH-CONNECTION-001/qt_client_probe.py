# -*- coding: utf-8 -*-
# REQ-AUTH-CONNECTION-001 阶段四：Qt 桌面端会话链路探针。
# offscreen 驱动真实 Client 代码（qt_client_harness.cpp 链接 Client 全部源文件），
# 对接真实 Server + MySQL + Redis。覆盖：
#   注册/登录新布局与 token 落盘、119 退出转匿名、123 被踢清凭据、
#   重启客户端后 121 恢复、篡改 token 恢复失败回退、48 注销全清。
# QSettings/快照均指向隔离临时目录，不碰用户真实配置；只清理自建账号。
# 运行：py -3.13 -X utf8 本文件（需先构建 dist/Server.exe；harness 自动构建）
import hashlib
import os
import secrets
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
DIST = REPO / "dist"
EXE = DIST / "Server.exe"
CLIENT_DIR = REPO / "Client"
QT_BIN = Path(r"C:\Qt\5.14.2\mingw73_64\bin")
MINGW_BIN = Path(r"C:\Qt\Tools\mingw730_64\bin")
sys.path.insert(0, str(REPO / "project-docs" / "evidence" / "_harness"))
import server_ctl  # noqa: E402
import pymysql  # noqa: E402

HOST, PORT, HDR, PWD = "127.0.0.1", 5000, 76, "123456"
REDIS_HOST, REDIS_PORT = "127.0.0.1", 6379
RUN = secrets.token_hex(4)
RESULTS = []
MADE_USERS = []


def check(label, cond, detail=""):
    RESULTS.append((label, bool(cond)))
    print(("[PASS] " if cond else "[FAIL] ") + label + (" :: " + str(detail) if detail != "" else ""))
    sys.stdout.flush()


def uid(tag):
    name = f"ac4{tag}{RUN}"
    MADE_USERS.append(name)
    return name


def db():
    cfg = (REPO / "Server" / "connect.config").read_text(encoding="utf-8").splitlines()
    return pymysql.connect(host=cfg[3], port=int(cfg[4]), user=cfg[6], password=cfg[7],
                           database=cfg[5], autocommit=True, connect_timeout=5)


def exec_sql(sql, args=()):
    with db() as c, c.cursor() as cur:
        return cur.execute(sql, args)


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


def f32(s):
    return s.encode()[:31].ljust(32, b"\0")


def creds(name):
    # REQ-PWD-PLAIN-001：服务端收到明文密码后在内存中计算 MD5；
    # 旁路第二连接必须与 Qt 登录表单保持同一 caData 语义。
    return f32(name) + f32(PWD)


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

    def ask(self, pkt, want, timeout=8):
        self.s.sendall(pkt)
        got, end = [], time.time() + timeout
        while not [g for g in got if g[0] == want] and time.time() < end:
            self.s.settimeout(0.3)
            try:
                d = self.s.recv(262144)
                if not d:
                    break
                self.buf += d
            except socket.timeout:
                pass
            ps, self.buf = unpack(self.buf)
            got.extend(ps)
        return got

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def get_captcha(conn, cid):
    r = conn.ask(pdu(44, dat=cid.encode().ljust(64, b"\0")), 45)
    if not r:
        return None
    return r[0][2].split(b"\0")[0].decode()


def build_harness(workdir):
    # 复用 Client.pro 的源/头/表单/资源列表，去掉 main.cpp，加 harness
    pro_src = (CLIENT_DIR / "Client.pro").read_text(encoding="utf-8")
    def section(name):
        import re
        m = re.search(name + r"\s*\+=\s*((?:[^\n]*\\\n)*[^\n]*)", pro_src)
        items = [x.strip().rstrip("\\").strip() for x in m.group(1).replace("\\\n", " ").split()]
        return [x for x in items if x]
    sources = [s for s in section("SOURCES") if s != "main.cpp"]
    headers = section("HEADERS")
    forms = section("FORMS")
    resources = section("RESOURCES")
    cdir = str(CLIENT_DIR).replace("\\", "/")
    lines = ["TEMPLATE = app", "TARGET = qt_client_harness",
             "QT += core gui network concurrent sql widgets",
             "CONFIG += c++11 utf8_source console", "CONFIG -= app_bundle",
             f"INCLUDEPATH += {cdir}",
             "DEFINES += QT_DEPRECATED_WARNINGS"]
    lines.append("SOURCES += " + " ".join(f"{cdir}/{s}" for s in sources)
                 + " " + str(HERE / "qt_client_harness.cpp").replace("\\", "/"))
    lines.append("HEADERS += " + " ".join(f"{cdir}/{h}" for h in headers))
    lines.append("FORMS += " + " ".join(f"{cdir}/{f}" for f in forms))
    lines.append("RESOURCES += " + " ".join(f"{cdir}/{r}" for r in resources))
    (workdir / "harness.pro").write_text("\n".join(lines) + "\n", encoding="utf-8")
    env = dict(os.environ)
    env["PATH"] = f"{QT_BIN};{MINGW_BIN};" + env.get("PATH", "")
    q = subprocess.run([str(QT_BIN / "qmake.exe"), "harness.pro", "CONFIG+=release", "CONFIG-=debug"],
                       cwd=workdir, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
    if q.returncode != 0:
        check("B0 qmake harness", False, (q.stderr or q.stdout or "")[-400:])
        return None
    m = subprocess.run([str(MINGW_BIN / "mingw32-make.exe"), "-j4"],
                       cwd=workdir, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
    exe = workdir / "release" / "qt_client_harness.exe"
    check("B1 harness 编译", m.returncode == 0 and exe.exists(),
          (m.stderr or m.stdout or "")[-600:] if not exe.exists() else "")
    if exe.exists():
        # offscreen 平台插件随 exe 目录分发（dist 只有 windows 插件；Qt 安装目录不随探针走）
        shutil.copytree(QT_BIN.parent / "plugins" / "platforms",
                        exe.parent / "platforms", dirs_exist_ok=True)
    return exe if exe.exists() else None


def run_mode(exe, mode, user=None, feed_cid=True, wait_ready=None, timeout=90, extra_env=None):
    """启动 harness 一个模式进程；feed_cid=True 时轮询 cid 文件并从 Redis 取验证码答案喂给它。
    wait_ready: 等到 stdout 出现该标记后调用回调（用于 kicked 场景第二连接登录）。"""
    env = dict(os.environ)
    env["PATH"] = f"{QT_BIN};{MINGW_BIN};" + env.get("PATH", "")
    env["QT_QPA_PLATFORM"] = "minimal"
    env["CLOUDDISK_CAPTCHA_TEXT_FOR_TESTS"] = "1"
    if mode == "logoff":
        env["HARNESS_DIRECT_LOGOFF"] = "1"
    if extra_env:
        env.update(extra_env)
    args = [str(exe), mode] + ([user, PWD] if user else [])
    errf = open(exe.parent / f"harness_{mode}_{os.getpid()}.err", "w", encoding="utf-8")
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=errf,
                         text=True, encoding="utf-8", errors="replace", env=env, cwd=str(exe.parent))
    cid_file = answer_file = None
    fed_req = 0
    last_code = {}
    ready_called = False
    lines = []
    end = time.time() + timeout
    while time.time() < end:
        line = p.stdout.readline()
        if line:
            lines.append(line.rstrip())
            if line.startswith("HARNESS_CID_FILE "):
                cid_file = Path(line.split(" ", 1)[1].strip())
            elif line.startswith("HARNESS_ANSWER_FILE "):
                answer_file = Path(line.split(" ", 1)[1].strip())
            elif wait_ready and not ready_called and wait_ready[0] in line:
                ready_called = True
                wait_ready[1]()
        if p.poll() is not None:
            # 读尽剩余输出
            rest = p.stdout.read()
            lines.extend(rest.splitlines())
            break
        # 喂验证码答案：请求序号同步——harness 每点一次"获取验证码"递增 req 文件；
        # 探针清冷却键后等 Redis 码值变化（新码已写入）再喂，杜绝喂到上一轮旧码。
        if feed_cid and cid_file and answer_file and cid_file.exists():
            cid = cid_file.read_text().strip()
            req_file = cid_file.parent / "req.txt"
            req = int(req_file.read_text().strip() or "0") if req_file.exists() else 0
            if cid and req > fed_req:
                redis_call("DEL", f"cap:{cid}:cool")
                old_code = last_code.get(cid)
                # 等新码写入（44 处理完成）：码键存在且与上次喂过的不同
                deadline = time.time() + 5
                while time.time() < deadline:
                    code = redis_get(f"cap:{cid}:code")
                    if code and code != old_code:
                        answer_file.write_bytes(code)
                        last_code[cid] = code
                        fed_req = req
                        break
                    time.sleep(0.03)
        time.sleep(0.05)
    else:
        p.kill()
        lines.append("TIMEOUT")
    fails = None
    for ln in lines:
        if ln.startswith("CHECK "):
            parts = ln.split(" ", 3)
            ok = parts[2] == "PASS"
            detail = parts[3][2:] if len(parts) > 3 and parts[3].startswith("|") else ""
            check(f"[{mode}] {parts[1]}", ok, detail)
        elif ln.startswith("FAILS "):
            fails = int(ln.split()[1])
    if fails is None:
        check(f"[{mode}] harness 正常退出", False, "\n".join(lines[-6:]))
    return p.returncode, lines


def cleanup_user(u):
    exec_sql("DELETE FROM login_record WHERE username=%s", (u,))
    exec_sql("DELETE FROM user_info WHERE name=%s", (u,))
    shutil.rmtree(DIST / "filesys" / u, ignore_errors=True)


def main():
    print(f"== qt_client_probe (REQ-AUTH-CONNECTION-001 阶段四) run={RUN}")
    if not EXE.exists():
        print("FATAL: dist/Server.exe 不存在")
        return 3
    os.environ["CLOUDDISK_CAPTCHA_TEXT_FOR_TESTS"] = "1"
    os.environ["CLOUDDISK_REDIS_URL"] = f"redis://{REDIS_HOST}:{REDIS_PORT}"
    workdir = REPO / "build" / "probe-qt-client"
    workdir.mkdir(parents=True, exist_ok=True)
    exe = build_harness(workdir)
    if exe is None:
        return 1
    ctl = server_ctl.ServerCtl(str(EXE), str(DIST))
    if not ctl.start():
        print("FATAL: " + str(ctl.last_error))
        return 3
    try:
        u = uid("a")
        # 1) 注册+登录：token 落盘（QSettings 隔离 INI 在 harness 进程临时目录内）
        rc, _ = run_mode(exe, "login", u)
        check("S1 login 模式退出码 0", rc == 0, rc)
        # 2) 119 退出（新进程重新登录后点退出按钮）
        rc, _ = run_mode(exe, "logout", u)
        check("S2 logout 模式退出码 0", rc == 0, rc)
        # 3) 被踢：kickwait 进程登录后，第二连接用同账号新 clientId 登录触发 123
        cid_b = secrets.token_hex(16)

        def kick():
            c = Conn()
            ok = False
            last = "?"
            for _ in range(3):
                redis_call("DEL", f"cap:{cid_b}:cool")
                code = get_captcha(c, cid_b)
                if not code:
                    last = "no-captcha"
                    continue
                r = c.ask(pdu(3, msg=code.encode() + b"\0" + cid_b.encode(), dat=creds(u)), 4)
                last_code = struct.unpack("<i", r[0][1][:4])[0] if r else -99
                if r and last_code == 0:
                    ok = True
                    break
                last = f"login-code={last_code}"
            check("S3a 第二设备登录成功（触发互踢）", ok, last)
            c.close()
            redis_call("DEL", f"cap:{cid_b}:cool")

        rc, _ = run_mode(exe, "kickwait", u, wait_ready=("HARNESS_READY kickwait", kick), timeout=120)
        check("S3 kickwait 模式退出码 0", rc == 0, rc)
        redis_call("DEL", f"session:{cid_b}")
        # 4) 重启客户端恢复：login 进程保存 INI → 新进程凭据恢复（121）
        u2 = uid("r")
        ini = workdir / "saved.ini"
        rc, _ = run_mode(exe, "login", u2, extra_env={"HARNESS_SAVE_INI": str(ini)})
        check("S4a 恢复前置登录退出码 0", rc == 0 and ini.exists(), rc)
        rc, _ = run_mode(exe, "resume", u2, feed_cid=False,
                         extra_env={"HARNESS_PRIOR_INI": str(ini), "HARNESS_SAVE_INI": str(ini)})
        check("S4 resume 模式退出码 0", rc == 0, rc)
        # 5) 篡改 token：新进程用保存的 INI（篡改在 harness 内做）→ 122 失败清凭据回登录页
        rc, _ = run_mode(exe, "badtoken", u2, feed_cid=False,
                         extra_env={"HARNESS_PRIOR_INI": str(ini)})
        check("S5 badtoken 模式退出码 0", rc == 0, rc)
        cleanup_user(u2)
        # 6) 注销（新进程重新登录后走 48 确认链路）
        rc, _ = run_mode(exe, "logoff", u)
        check("S6 logoff 模式退出码 0", rc == 0, rc)
        cleanup_user(u)
    finally:
        ctl.stop()
        for u in MADE_USERS:
            try:
                cleanup_user(u)
            except Exception:
                pass
        pass  # shutil.rmtree(workdir, ignore_errors=True)  # 排查期保留
    fails = [n for n, ok in RESULTS if not ok]
    print(f"== 总结: {len(RESULTS)} 项, FAIL {len(fails)} 项")
    for n in fails:
        print("  FAIL " + n)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
