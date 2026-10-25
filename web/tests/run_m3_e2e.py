# -*- coding: utf-8 -*-
"""REQ-WEB-001 M3 端到端编排：启动真实 Server.exe + 网关，跑云盘/传输 e2e，按 PID 收尾。

夹具清理（webtd_* 测试用户）：
  - 协议产生的 user_file/upload_session/login_record/user_info 行；
  - 内容对象：先收集该用户 user_file 引用的 object_id，删行后对"无剩余引用"的对象
    删 file_object 行与物理文件（dist/filesys/objects/...，storage_path 来自 DB）；
  - dist/filesys/<user> 整棵目录树（含 .staging 残留）。
运行：仓库根目录  py -3.13 -X utf8 web/tests/run_m3_e2e.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
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
E2E = os.path.join(HERE, "drive_e2e.mjs")
FIXTURE_REPORT = os.path.join(tempfile.gettempdir(), "m3_fixtures.json")

MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0


def sql(q):
    r = subprocess.run([MYSQL] + DB + [q], capture_output=True, text=True,
                       creationflags=CREATE_NO_WINDOW)
    if r.returncode != 0:
        print("[warn] SQL 失败:", q[:120], (r.stderr or "").strip()[:200])
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


def remove_object_if_unreferenced(oid):
    remain = sql("SELECT COUNT(*) FROM user_file WHERE object_id=%s" % oid)
    if remain != "0":
        return
    sp = sql("SELECT storage_path FROM file_object WHERE object_id=%s" % oid)
    if sp:
        p = os.path.join(DIST, "filesys", sp.replace("/", os.sep))
        # 双重护栏：只删 dist/filesys 内的普通文件
        if os.path.isfile(p) and os.path.abspath(p).startswith(os.path.join(DIST, "filesys")):
            try:
                os.remove(p)
            except OSError as e:
                print("[warn] 对象文件删除失败:", p, e)
    sql("DELETE FROM file_object WHERE object_id=%s" % oid)


def cleanup_user(user):
    if not user.startswith("webt"):
        print("[warn] 拒绝清理非测试前缀用户:", user)
        return
    # 1) 收集该用户引用的对象，删 user_file 行，再收敛无引用对象
    oids = sql("SELECT DISTINCT object_id FROM user_file WHERE owner_name='%s' "
               "AND object_id IS NOT NULL" % user)
    sql("DELETE FROM user_file WHERE owner_name='%s'" % user)
    for oid in (oids or "").split():
        if oid.isdigit():
            remove_object_if_unreferenced(oid)
    # 2) 会话/登录/账号行
    sql("DELETE FROM upload_session WHERE owner_name='%s'" % user)
    sql("DELETE FROM login_record WHERE username='%s'" % user)
    sql("DELETE FROM user_info WHERE name='%s'" % user)
    # 3) 用户文件树（含 .staging 残留）
    tree = os.path.join(DIST, "filesys", user)
    if os.path.isdir(tree) and os.path.abspath(tree).startswith(os.path.join(DIST, "filesys")):
        shutil.rmtree(tree, ignore_errors=True)


def residue_counts(user):
    u = sql("SELECT COUNT(*) FROM user_info WHERE name='%s'" % user) or "?"
    uf = sql("SELECT COUNT(*) FROM user_file WHERE owner_name='%s'" % user) or "?"
    us = sql("SELECT COUNT(*) FROM upload_session WHERE owner_name='%s'" % user) or "?"
    lr = sql("SELECT COUNT(*) FROM login_record WHERE username='%s'" % user) or "?"
    tree = os.path.isdir(os.path.join(DIST, "filesys", user))
    return "user_info=%s user_file=%s upload_session=%s login_record=%s filesys目录=%s" % (u, uf, us, lr, tree)


def main():
    user = "webtd_%d" % int(time.time())
    print("[orch] 测试用户:", user)

    for p in (PORT, GW_PORT):
        if server_ctl.port_busy("127.0.0.1", p):
            pids = sorted(server_ctl.find_listener_pids(p))
            print("[orch] 端口 %d 已被占用（PID %s），请先手动结束再跑。" % (p, pids or "?"))
            return 3

    if os.path.exists(FIXTURE_REPORT):
        os.remove(FIXTURE_REPORT)
    cleanup_user(user)  # 时间戳唯一，兜底

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
        print("[orch] 运行 drive_e2e.mjs ...\n" + "-" * 60)
        r = subprocess.run(["node", E2E], cwd=REPO, env=e2e_env,
                           capture_output=True, text=True, timeout=600,
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
        cleanup_user(user)
        if os.path.exists(FIXTURE_REPORT):
            try:
                print("[orch] 夹具报告:", open(FIXTURE_REPORT, encoding="utf-8").read()[:200])
                os.remove(FIXTURE_REPORT)
            except OSError:
                pass
        print("[orch] 清理复核:", residue_counts(user))

    return rc


if __name__ == "__main__":
    sys.exit(main())
