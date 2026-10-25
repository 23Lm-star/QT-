# -*- coding: utf-8 -*-
"""REQ-WEB-001 M5 端到端编排：启动真实 Server.exe + 网关，跑好友分享（33-37）与外链（52-57）e2e，按 PID 收尾。
M5b 起外链取回走服务端条目优先；夹具清理包含 share_link 行与物理/条目产物。

夹具清理（webts_* 测试用户）：
  - share_link：按 owner_name 删除本轮测试用户创建的链接行；
  - 个人条目：删 user_file 行，收敛无引用内容对象（user_file + group_file 双查）；
  - 外链取文件落地是物理复制（无条目），随 dist/filesys/<user> 目录树一并删除；
  - 账号/会话/登录行与目录树。
运行：仓库根目录  py -3.13 -X utf8 web/tests/run_m5_e2e.py
"""
import os
import shutil
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
E2E = os.path.join(HERE, "share_e2e.mjs")

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


def in_list(users):
    return ",".join("'%s'" % u for u in users)


def remove_object_if_unreferenced(oid):
    """仅当 user_file 与 group_file 都不再引用该对象时，删对象行与物理文件。"""
    uf = sql("SELECT COUNT(*) FROM user_file WHERE object_id=%s" % oid)
    gf = sql("SELECT COUNT(*) FROM group_file WHERE object_id=%s" % oid)
    if (uf or "?") != "0" or (gf or "?") != "0":
        return
    sp = sql("SELECT storage_path FROM file_object WHERE object_id=%s" % oid)
    if sp:
        p = os.path.join(DIST, "filesys", sp.replace("/", os.sep))
        if os.path.isfile(p) and os.path.abspath(p).startswith(os.path.join(DIST, "filesys")):
            try:
                os.remove(p)
            except OSError as e:
                print("[warn] 对象文件删除失败:", p, e)
    sql("DELETE FROM file_object WHERE object_id=%s" % oid)


def cleanup_users(users):
    for u in users:
        if not u.startswith("webt"):
            print("[warn] 拒绝清理非测试前缀用户:", u)
            return
    il = in_list(users)
    # 1) 分享链接行
    sql("DELETE FROM share_link WHERE owner_name IN (%s)" % il)
    # 2) 个人条目对象 + user_file 行
    objs = set()
    for u in users:
        oids = sql("SELECT DISTINCT object_id FROM user_file WHERE owner_name='%s' "
                   "AND object_id IS NOT NULL" % u)
        sql("DELETE FROM user_file WHERE owner_name='%s'" % u)
        for oid in (oids or "").split():
            if oid.isdigit():
                objs.add(oid)
    for oid in sorted(objs):
        remove_object_if_unreferenced(oid)
    # 3) 会话/登录/账号 + 目录树（外链落地为物理复制，无条目）
    for u in users:
        sql("DELETE FROM upload_session WHERE owner_name='%s'" % u)
        sql("DELETE FROM login_record WHERE username='%s'" % u)
        sql("DELETE FROM user_info WHERE name='%s'" % u)
        tree = os.path.join(DIST, "filesys", u)
        if os.path.isdir(tree) and os.path.abspath(tree).startswith(os.path.join(DIST, "filesys")):
            shutil.rmtree(tree, ignore_errors=True)


def residue_counts(users):
    il = in_list(users)
    ui = sql("SELECT COUNT(*) FROM user_info WHERE name IN (%s)" % il) or "?"
    uf = sql("SELECT COUNT(*) FROM user_file WHERE owner_name IN (%s)" % il) or "?"
    sl = sql("SELECT COUNT(*) FROM share_link WHERE owner_name IN (%s)" % il) or "?"
    trees = any(os.path.isdir(os.path.join(DIST, "filesys", u)) for u in users)
    return "user_info=%s user_file=%s share_link=%s filesys目录存在=%s" % (ui, uf, sl, trees)


def main():
    ts = int(time.time())
    users = ["webts_a_%d" % ts, "webts_b_%d" % ts, "webts_c_%d" % ts]
    print("[orch] 测试用户:", users)

    for p in (PORT, GW_PORT):
        if server_ctl.port_busy("127.0.0.1", p):
            pids = sorted(server_ctl.find_listener_pids(p))
            print("[orch] 端口 %d 已被占用（PID %s），请先手动结束再跑。" % (p, pids or "?"))
            return 3

    cleanup_users(users)  # 时间戳唯一，兜底

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
        e2e_env["E2E_USER_A"], e2e_env["E2E_USER_B"], e2e_env["E2E_USER_C"] = users
        print("[orch] 运行 share_e2e.mjs ...\n" + "-" * 60)
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
        cleanup_users(users)
        print("[orch] 清理复核:", residue_counts(users))

    return rc


if __name__ == "__main__":
    sys.exit(main())
