# -*- coding: utf-8 -*-
"""M3 人工验收常驻编排：启动 Server.exe + 网关（托管 dist），sentinel/超时控制，按 PID 收尾。
运行（后台）：py -3.13 -X utf8 web/tests/serve_manual.py
停止：创建 web/tests/.stop_serve 或等待超时（默认 4 小时）。
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
SENTINEL = os.path.join(HERE, ".stop_serve")
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0
MAX_SECONDS = int(os.environ.get("SERVE_MAX_SECONDS", "14400"))


def kill_pid(pid):
    if pid:
        subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True,
                       creationflags=CREATE_NO_WINDOW)


def main():
    if os.path.exists(SENTINEL):
        os.remove(SENTINEL)
    for p in (PORT, GW_PORT):
        if server_ctl.port_busy("127.0.0.1", p):
            print("[serve] 端口 %d 被占，放弃。" % p)
            return 3
    ctl = server_ctl.ServerCtl(EXE, DIST, HOST, PORT)
    gw = None
    try:
        if not ctl.start():
            print("[serve] 服务端启动失败:", ctl.last_error)
            return 2
        env = dict(os.environ)
        env["GATEWAY_PORT"] = str(GW_PORT)
        env["GATEWAY_TARGET_HOST"] = "127.0.0.1"
        env["GATEWAY_TARGET_PORT"] = str(PORT)
        gw = subprocess.Popen(["node", "server.js"], cwd=GATEWAY_DIR, env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              creationflags=CREATE_NO_WINDOW)
        dl = time.time() + 12
        while time.time() < dl and not server_ctl.port_busy("127.0.0.1", GW_PORT):
            time.sleep(0.3)
        if not server_ctl.port_busy("127.0.0.1", GW_PORT):
            print("[serve] 网关未监听 %d。" % GW_PORT)
            return 2
        print("[serve] READY http://127.0.0.1:%d/ Server PID=%d GW PID=%d" % (GW_PORT, ctl.pid, gw.pid))
        sys.stdout.flush()
        end = time.time() + MAX_SECONDS
        while time.time() < end and not os.path.exists(SENTINEL):
            time.sleep(0.5)
        print("[serve] 停止信号或超时，收尾。")
    finally:
        if gw:
            kill_pid(gw.pid)
        ctl.stop()
        print("[serve] 已结束服务端与网关。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
