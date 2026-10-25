# -*- coding: utf-8 -*-
"""REQ-WEB-001 M2 端到端编排：启动真实 Server.exe + 网关，跑双用户好友/私聊 e2e，
按 PID 收尾并清理测试夹具（两个 webt* 用户及其 friend/message_info/login_record 行）。

运行：仓库根目录  py -3.13 -X utf8 web/tests/run_m2_e2e.py
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl  # noqa: E402

HOST, PORT = "127.0.0.1", 5000
GW_PORT = 8080
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
GATEWAY_DIR = os.path.join(REPO, "web", "gateway")
E2E = os.path.join(HERE, "chat_e2e.mjs")

MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0


def sql(q):
    r = subprocess.run([MYSQL] + DB + [q], capture_output=True, text=True,
                       creationflags=CREATE_NO_WINDOW)
    if r.returncode != 0:
        print("[warn] SQL 失败:", q, (r.stderr or "").strip())
        return None
    return (r.stdout or "").strip()


def wait_port(port, timeout=12.0):
    dl = time.time() + timeout
    while time.time() < dl:
        if server_ctl.port_busy("127.0.0.1", port):
            return True
        time.sleep(0.3)
    return False


def kill_pid(pid):
    if pid:
        subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True,
                       creationflags=CREATE_NO_WINDOW)


def cleanup_users(users):
    quoted = ",".join("'%s'" % u for u in users)
    sql("DELETE FROM message_info WHERE seed_name IN (%s) OR rev_name IN (%s)" % (quoted, quoted))
    sql("DELETE FROM friend WHERE name IN (%s) OR friendName IN (%s)" % (quoted, quoted))
    sql("DELETE FROM login_record WHERE username IN (%s)" % quoted)
    sql("DELETE FROM user_info WHERE name IN (%s)" % quoted)


def main():
    ts = int(time.time())
    users = ["webta_%d" % ts, "webtb_%d" % ts]
    print("[orch] 测试用户:", users)

    for p in (PORT, GW_PORT):
        if server_ctl.port_busy("127.0.0.1", p):
            pids = sorted(server_ctl.find_listener_pids(p))
            print("[orch] 端口 %d 已被占用（PID %s），请先手动结束再跑。" % (p, pids or "?"))
            return 3

    # 预清理：同名残留（理论上时间戳唯一，兜底）。
    cleanup_users(users)

    ctl = server_ctl.ServerCtl(EXE, DIST, HOST, PORT)
    gw = None
    rc = 2
    try:
        if not ctl.start():
            print("[orch] 服务端启动失败:", ctl.last_error)
            return 2
        print("[orch] Server.exe 已监听 5000 (PID %s)" % ctl.pid)

        env = dict(os.environ)
        env["GATEWAY_PORT"] = str(GW_PORT)
        env["GATEWAY_TARGET_HOST"] = "127.0.0.1"
        env["GATEWAY_TARGET_PORT"] = str(PORT)
        gw = subprocess.Popen(["node", "server.js"], cwd=GATEWAY_DIR, env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              creationflags=CREATE_NO_WINDOW)
        if not wait_port(GW_PORT):
            print("[orch] 网关未在 %d 监听，放弃。" % GW_PORT)
            return 2
        print("[orch] 网关已监听 %d (PID %s)" % (GW_PORT, gw.pid))

        e2e_env = dict(os.environ)
        e2e_env["GW_URL"] = "ws://127.0.0.1:%d/ws" % GW_PORT
        e2e_env["E2E_USER_A"] = users[0]
        e2e_env["E2E_USER_B"] = users[1]
        print("[orch] 运行 chat_e2e.mjs ...\n" + "-" * 60)
        r = subprocess.run(["node", E2E], cwd=REPO, env=e2e_env,
                           capture_output=True, text=True,
                           creationflags=CREATE_NO_WINDOW)
        sys.stdout.write(r.stdout or "")
        if r.stderr:
            sys.stdout.write("[e2e stderr]\n" + r.stderr)
        rc = r.returncode
        print("-" * 60 + "\n[orch] e2e 退出码:", rc)
    finally:
        if gw:
            kill_pid(gw.pid)
        ctl.stop()
        cleanup_users(users)
        left_u = sql("SELECT COUNT(*) FROM user_info WHERE name IN (%s)"
                     % ",".join("'%s'" % u for u in users))
        left_m = sql("SELECT COUNT(*) FROM message_info WHERE seed_name IN (%s) OR rev_name IN (%s)"
                     % (",".join("'%s'" % u for u in users), ",".join("'%s'" % u for u in users)))
        print("[orch] 清理复核: user_info 残留=%s message_info 残留=%s" % (left_u, left_m))

    return rc


if __name__ == "__main__":
    sys.exit(main())
