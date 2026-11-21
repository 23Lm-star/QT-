# -*- coding: utf-8 -*-
# REQ-AUTH-CONNECTION-001 阶段二：Redis 会话与验证码底层探针。
# 覆盖计划 §4 通过条件：
#   1) 同一 clientId 跨连接读取到相同验证码冷却/锁定状态（键按 clientId 归属）
#   2) 并发提交同一验证码最多一个成功（两进程竞态 + Lua 原子消费）
#   3) 会话 TTL、tokenHash、版本校验正确（含覆盖登录、revoked 状态、条件撤销）
#   4) 账号可枚举并撤销全部会话（sessions:{user} 索引 + 脏成员清理）
#   5) Redis 断开/未配置时没有连接内存认证旁路（down/disabled 模式全部失败关闭）
# 依赖：本机 Redis（CLOUDDISK_REDIS_URL 或默认 redis://127.0.0.1:6379）、Qt 5.14.2 MinGW 工具链。
# 运行：py -3.13 -X utf8 project-docs/evidence/REQ-AUTH-CONNECTION-001/redis_session_probe.py
# 退出码：0=全部通过；1=存在失败；2=环境未就绪（Redis 不可达或工具链缺失，不得视为通过）。
import os
import re
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[3]
SERVER = ROOT / "Server"
HERE = Path(__file__).resolve().parent
HARNESS = HERE / "redis_store_harness.cpp"

QT_BIN = Path(r"C:\Qt\5.14.2\mingw73_64\bin")
MINGW_BIN = Path(r"C:\Qt\Tools\mingw730_64\bin")

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name + (("" if ok or not detail else " | " + str(detail))))
    sys.stdout.flush()


def run_harness(exe, mode_args, env_url=None, timeout=60):
    env = dict(os.environ)
    env["PATH"] = f"{QT_BIN};{MINGW_BIN};" + env.get("PATH", "")
    if env_url is None:
        env.pop("CLOUDDISK_REDIS_URL", None)
    else:
        env["CLOUDDISK_REDIS_URL"] = env_url
    proc = subprocess.run([str(exe)] + mode_args, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout, env=env)
    return proc


def parse_checks(proc):
    fails_line = None
    for line in (proc.stdout or "").splitlines():
        if line.startswith("CHECK "):
            parts = line.split(" ", 3)
            name, verdict = parts[1], parts[2]
            detail = parts[3][2:] if len(parts) > 3 and parts[3].startswith("|") else ""
            check("H " + name, verdict == "PASS", detail)
        elif line.startswith("FAILS "):
            fails_line = int(line.split()[1])
    if fails_line is None:
        check("H harness 正常输出", False,
              ((proc.stderr or proc.stdout or "")[-300:]).strip())
    return fails_line


# ---------- 0. 环境检查 ----------
def redis_target():
    url = os.environ.get("CLOUDDISK_REDIS_URL") or "redis://127.0.0.1:6379"
    u = urlparse(url)
    return u.hostname or "127.0.0.1", u.port or 6379, u.password or ""


def redis_command(host, port, password, *args):
    s = socket.create_connection((host, port), timeout=2)
    try:
        out = b""
        if password:
            out += _redis_call(s, ["AUTH", password])
        return _redis_call(s, list(args))
    finally:
        s.close()


def _redis_call(s, args):
    req = f"*{len(args)}\r\n".encode()
    for a in args:
        b = a.encode() if isinstance(a, str) else a
        req += f"${len(b)}\r\n".encode() + b + b"\r\n"
    s.sendall(req)
    import time
    time.sleep(0.05)
    data = b""
    s.settimeout(1)
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
            if data.endswith(b"\r\n"):
                break
    except socket.timeout:
        pass
    return data


def env_checks():
    host, port, password = redis_target()
    try:
        resp = redis_command(host, port, password, "PING")
    except OSError as e:
        check("E0 Redis 可达", False, f"{host}:{port} {e}")
        return False
    ok = b"+PONG" in resp
    check("E0 Redis 可达", ok, resp[:32])
    check("E1 工具链存在",
          (QT_BIN / "qmake.exe").exists() and (MINGW_BIN / "mingw32-make.exe").exists())
    check("E2 Server.pro 已纳入 sessionstore",
          "sessionstore.cpp" in (SERVER / "Server.pro").read_text(encoding="utf-8"))
    return ok


# ---------- 1. 静态检查：键与脚本注入面 ----------
def static_checks():
    rs = (SERVER / "redisstore.cpp").read_text(encoding="utf-8")
    mh = (SERVER / "msghandler.cpp").read_text(encoding="utf-8")
    plan_ops = ["evalCaptchaConsume", "evalCaptchaFailure", "setSession", "getSession",
                "evalSessionRevokeIfMatch", "revokeUserSessions", "evalOnlineDeleteIfVersion"]
    for op in plan_ops:
        check(f"S1 RedisStore 实现 {op}", f"bool RedisStore::{op}(" in rs)
    # 用户输入不得拼进 EVAL 脚本正文（只允许 ARGV/KEYS 传参）
    eval_calls = re.findall(r'command\w*\(\{"EVAL",(.*?)\}\)', rs, re.S)
    bad = [c for c in eval_calls if re.search(r'\+\s*(input|token|json|versionNeedle|hashNeedle|client)', c)]
    check("S2 EVAL 脚本无输入拼接", not bad, bad[:1])
    check("S3 验证码走原子消费", "evalCaptchaConsume" in mh and "evalCaptchaFailure" in mh)
    check("S4 移除旧 caMsg 可打印身份",
          "if (c < 0x21 || c > 0x7e) return QString::number(connId);" not in mh)
    check("S5 失败关闭（无内存回退日志语义）",
          "回退连接内存状态" not in mh and "保留连接内存状态" not in mh)


# ---------- 2. 构建 harness ----------
def build_harness(workdir):
    pro = workdir / "harness.pro"
    pro.write_text(f"""TEMPLATE = app
TARGET = redis_store_harness
QT += core network
QT -= gui
CONFIG += console c++11
CONFIG -= app_bundle
INCLUDEPATH += {str(SERVER).replace(chr(92), '/')}
SOURCES += {str(HARNESS).replace(chr(92), '/')} \\
    {str(SERVER / 'redisstore.cpp').replace(chr(92), '/')} \\
    {str(SERVER / 'sessionstore.cpp').replace(chr(92), '/')}
HEADERS += {str(SERVER / 'redisstore.h').replace(chr(92), '/')} \\
    {str(SERVER / 'sessionstore.h').replace(chr(92), '/')}
""", encoding="utf-8")
    env = dict(os.environ)
    env["PATH"] = f"{QT_BIN};{MINGW_BIN};" + env.get("PATH", "")
    qmake = subprocess.run([str(QT_BIN / "qmake.exe"), str(pro),
                            "CONFIG+=release", "CONFIG-=debug"],
                           cwd=workdir, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", env=env)
    if qmake.returncode != 0:
        check("B0 qmake harness", False, (qmake.stderr or qmake.stdout or "")[-400:])
        return None
    check("B0 qmake harness", True)
    mk = subprocess.run([str(MINGW_BIN / "mingw32-make.exe"), "-j4"],
                        cwd=workdir, capture_output=True, text=True,
                        encoding="utf-8", errors="replace", env=env)
    exe = workdir / "release" / "redis_store_harness.exe"
    check("B1 harness 编译", mk.returncode == 0 and exe.exists(),
          (mk.stderr or mk.stdout or "")[-800:] if not exe.exists() else "")
    return exe if exe.exists() else None


# ---------- 3. 行为场景 ----------
def behavior_checks(exe, url):
    nonce = secrets.token_hex(12)  # 24 hex，harness 拼成 32 位 clientId
    rc = parse_checks(run_harness(exe, ["selftest", nonce], env_url=url))
    check("H-selftest 退出码 0", rc == 0, rc)

    # 并发消费竞态：两个进程同时消费同一验证码，至多一个成功
    race_id = secrets.token_hex(16)
    code = "RACE" + secrets.token_hex(1)
    setup = run_harness(exe, ["race-setup", race_id, code], env_url=url)
    check("R0 race 预置", "SETUP OK" in setup.stdout)
    p1 = subprocess.Popen([str(exe), "race-consume", race_id, code],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                          env={**os.environ, "PATH": f"{QT_BIN};{MINGW_BIN};" + os.environ.get("PATH", ""),
                               "CLOUDDISK_REDIS_URL": url})
    p2 = subprocess.Popen([str(exe), "race-consume", race_id, code],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                          env={**os.environ, "PATH": f"{QT_BIN};{MINGW_BIN};" + os.environ.get("PATH", ""),
                               "CLOUDDISK_REDIS_URL": url})
    o1, _ = p1.communicate(timeout=30)
    o2, _ = p2.communicate(timeout=30)
    wins = sum(1 for o in (o1, o2) if "RACE_OUTCOME 0" in o)
    check("R1 并发消费至多一个成功", wins == 1, f"wins={wins} o1={o1.strip()} o2={o2.strip()}")
    state = run_harness(exe, ["race-state", race_id], env_url=url)
    m = re.search(r"RACE_CODE_TTL (-?\d+)", state.stdout)
    check("R2 消费成功后验证码作废", m is not None and int(m.group(1)) == -2,
          state.stdout.strip())

    # Redis 故障：已配置但端口拒绝 —— 全部操作失败关闭
    host, port, _ = redis_target()
    down_url = f"redis://{host}:{port + 1000}"
    rc = parse_checks(run_harness(exe, ["down"], env_url=down_url))
    check("H-down 退出码 0", rc == 0, rc)

    # 未配置 Redis —— SessionStore 拒绝（无内存认证旁路）
    rc = parse_checks(run_harness(exe, ["disabled"], env_url=None))
    check("H-disabled 退出码 0", rc == 0, rc)


def main():
    print(f"== redis_session_probe (REQ-AUTH-CONNECTION-001 阶段二) root={ROOT}")
    if not env_checks():
        print("== 环境未就绪（Redis 不可达），按未验证处理 ==")
        return 2
    static_checks()
    host, port, _ = redis_target()
    url = f"redis://{host}:{port}"
    # 构建目录必须在 ASCII 路径下（qmake 不识别中文用户 temp 目录）
    workdir = ROOT / "build" / "probe-redis-harness"
    workdir.mkdir(parents=True, exist_ok=True)
    try:
        exe = build_harness(workdir)
        if exe is None:
            print("== harness 构建失败，终止 ==")
            return 1
        behavior_checks(exe, url)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    fails = [n for n, ok in RESULTS if not ok]
    print(f"== 总结: {len(RESULTS)} 项, FAIL {len(fails)} 项")
    for n in fails:
        print(f"  FAIL {n}")
    if fails:
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
