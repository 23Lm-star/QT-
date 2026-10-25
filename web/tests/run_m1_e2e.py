# -*- coding: utf-8 -*-
"""REQ-WEB-001 M1 端到端编排：启动真实 Server.exe + 网关，跑 Node 登录 e2e，按 PID 收尾。

复用回归套件既有的 server_ctl（只按 PID 管理自己启动的进程，端口被占不盲杀）。
本脚本会启动/结束两个进程（dist/Server.exe、node 网关），并在 mydb 中创建后删除一个
带时间戳的测试用户（webt_*）；不触碰其他数据。

运行：仓库根目录  py -3.13 -X utf8 web/tests/run_m1_e2e.py
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
E2E = os.path.join(HERE, "login_e2e.mjs")

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
    if not pid:
        return
    subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True,
                   creationflags=CREATE_NO_WINDOW)


def main():
    user = "webt_%d" % int(time.time())
    print("[orch] 测试用户:", user)

    # 端口预检：5000/8080 必须空闲，否则不盲杀、直接报错。
    for p in (PORT, GW_PORT):
        if server_ctl.port_busy("127.0.0.1", p):
            pids = sorted(server_ctl.find_listener_pids(p))
            print("[orch] 端口 %d 已被占用（PID %s），请先手动结束再跑。" % (p, pids or "?"))
            return 3

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
        e2e_env["E2E_USER"] = user
        e2e_env["TCP_PORT"] = str(PORT)
        print("[orch] 运行 login_e2e.mjs ...\n" + "-" * 60)
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
        # 清理本次创建的测试用户（仅 DB 行；新登录用户无文件资产）。
        sql("DELETE FROM login_record WHERE username='%s'" % user)
        sql("DELETE FROM user_info WHERE name='%s'" % user)
        print("[orch] 已收尾进程并清理测试用户:", user)

    return rc


if __name__ == "__main__":
    sys.exit(main())
