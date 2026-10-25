# -*- coding: utf-8 -*-
"""REQ-WEB-001 M4 端到端编排：启动真实 Server.exe + 网关，跑群（生命周期+文本+文件）e2e，按 PID 收尾。

夹具清理（webtg_* 测试用户）：
  - 群表：收集这些用户作为 creator 或 member 关联的全部 group_id，删其
    group_message/group_invitation/group_member/group_file/group_info 行；
  - 个人条目：删 webtg_* 用户的 user_file 行，收敛无引用内容对象；
  - 群文件对象：object_id 可能与 user_file 共享（上传者个人条目），
    "无剩余引用"须同时统计 user_file 与 group_file；
  - 账号/会话/登录行与 dist/filesys/<user> 目录树。
运行：仓库根目录  py -3.13 -X utf8 web/tests/run_m4_e2e.py
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
E2E = os.path.join(HERE, "group_e2e.mjs")

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
    # 1) 收集关联 group_id（creator 或 member）
    gids = set()
    q1 = sql("SELECT group_id FROM group_info WHERE creator_name IN (%s)" % il) or ""
    q2 = sql("SELECT group_id FROM group_member WHERE user_name IN (%s)" % il) or ""
    for tok in (q1 + " " + q2).split():
        if tok.isdigit():
            gids.add(tok)
    gid_il = ",".join(gids) if gids else "0"
    # 2) 群文件引用的对象（删 group_file 前先收集）
    objs = set()
    gf_objs = sql("SELECT object_id FROM group_file WHERE group_id IN (%s)" % gid_il) or ""
    for tok in gf_objs.split():
        if tok.isdigit():
            objs.add(tok)
    # 3) 删群相关行（含群消息/邀请/成员/文件/群信息）
    for tbl in ("group_message", "group_invitation", "group_member", "group_file"):
        sql("DELETE FROM %s WHERE group_id IN (%s)" % (tbl, gid_il))
    sql("DELETE FROM group_info WHERE group_id IN (%s)" % gid_il)
    # 4) 个人条目对象 + user_file 行
    for u in users:
        oids = sql("SELECT DISTINCT object_id FROM user_file WHERE owner_name='%s' "
                   "AND object_id IS NOT NULL" % u)
        sql("DELETE FROM user_file WHERE owner_name='%s'" % u)
        for oid in (oids or "").split():
            if oid.isdigit():
                objs.add(oid)
    # 5) 收敛无引用对象（user_file 与 group_file 均已删，剩余引用为 0 才删）
    for oid in sorted(objs):
        remove_object_if_unreferenced(oid)
    # 6) 会话/登录/账号 + 目录树
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
    ginfo = sql("SELECT COUNT(*) FROM group_info WHERE creator_name IN (%s)" % il) or "?"
    gmem = sql("SELECT COUNT(*) FROM group_member WHERE user_name IN (%s)" % il) or "?"
    gmsg = sql("SELECT COUNT(*) FROM group_message WHERE sender_name IN (%s)" % il) or "?"
    uf = sql("SELECT COUNT(*) FROM user_file WHERE owner_name IN (%s)" % il) or "?"
    gf = sql("SELECT COUNT(*) FROM group_file WHERE uploader_name IN (%s)" % il) or "?"
    ginv = sql("SELECT COUNT(*) FROM group_invitation WHERE target_name IN (%s) "
               "OR inviter_name IN (%s)" % (il, il)) or "?"
    trees = any(os.path.isdir(os.path.join(DIST, "filesys", u)) for u in users)
    return ("user_info=%s group_info=%s group_member=%s group_message=%s user_file=%s "
            "group_file=%s group_invitation=%s filesys目录存在=%s"
            % (ui, ginfo, gmem, gmsg, uf, gf, ginv, trees))


def main():
    ts = int(time.time())
    users = ["webtg_a_%d" % ts, "webtg_b_%d" % ts, "webtg_c_%d" % ts]
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
        print("[orch] 运行 group_e2e.mjs ...\n" + "-" * 60)
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
