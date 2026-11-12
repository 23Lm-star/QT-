# REQ-MIG-001 迁移态盘点审计
# 盘点三类迁移态基线：空盐账号 / 迁移映射异常 / 非canonical逻辑键 / 未迁移旧物理文件与对象覆盖。
# DB 连接参数从 Server/connect.config 读取（不硬编码密码）。用法：
#   py -3.13 -X utf8 audit_migration.py [--fsroot E:/qt/xiangmu/Server/filesys]
import argparse
import os
import subprocess
import sys

CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0
CFG = os.path.join(os.path.dirname(__file__), "..", "..", "..", "Server", "connect.config")


def load_config():
    # newline="" 禁用换行归一，保持 CRLF 原样（与 Server::loadconfig 切分一致）
    with open(CFG, "r", encoding="utf-8", errors="replace", newline="") as f:
        lines = f.read().split("\r\n")
    host, port, dbname, user, pwd = lines[3], lines[4], lines[5], lines[6], lines[7]
    return host, port, dbname, user, pwd


class Db:
    def __init__(self):
        self.host, self.port, self.dbname, self.user, self.pwd = load_config()
        self.base = [r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe",
                     "-h", self.host, "-P", self.port, "-u", self.user,
                     "-p" + self.pwd, self.dbname, "-N", "-B",
                     "--default-character-set=utf8mb4", "-e"]

    def sql(self, q):
        r = subprocess.run(self.base + [q], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", creationflags=CREATE_NO_WINDOW)
        if r.returncode != 0:
            print("[错误] SQL 失败:", (r.stderr or "").strip()[:300])
            sys.exit(2)
        return r.stdout


def rows(q):
    out = Db().sql(q).strip("\n")
    return [l.split("\t") for l in out.split("\n")] if out else []


# ===== normalizeParent 的 Python 复刻（fileobjectstore.cpp:67-170，用于库外独立复核）=====
def clean_path(p):
    p = p.replace("\\", "/")
    while "//" in p:
        p = p.replace("//", "/")
    leading = p.startswith("/")
    segs = [s for s in p.split("/") if s]
    out = []
    for s in segs:
        if s == ".":
            continue
        if s == ".." and out:
            out.pop()
            continue
        if s == "..":
            continue  # 顶层 .. 丢弃（与判定无关，保守处理）
        out.append(s)
    return ("/" if leading else "") + "/".join(out)


def is_logic_key_form(x, fs_base):
    if not x.startswith("/") or not x.endswith("/") or "\\" in x:
        return False
    if x == "/":
        return True
    if "//" in x or ":" in x:
        return False
    first = x[1:].split("/", 1)[0]
    if first and first.lower() == fs_base.lower():
        return False
    return not any(s in (".", "..") for s in x.split("/") if s)


def normalize_parent(owner, dir_path, fs_root):
    """返回 canonical 键；空串=无法映射（与 C++ 返回 QString() 一致）。"""
    if not owner or not dir_path:
        return ""
    x = dir_path.replace("\\", "/")
    if is_logic_key_form(x, os.path.basename(fs_root)):
        return x
    x = clean_path(x)
    fs_base = os.path.basename(fs_root)
    root_abs = fs_root.replace("\\", "/").rstrip("/") + "/" + owner
    hit, rest = False, ""
    if x == root_abs:
        hit = True
    elif x.startswith(root_abs + "/"):
        rest = x[len(root_abs) + 1:]
        hit = True
    else:
        segs = [s for s in x.split("/") if s]
        for i in range(len(segs) - 1):
            if segs[i].lower() == fs_base.lower() and segs[i + 1].lower() == owner.lower():
                rest = "/".join(segs[i + 2:])
                hit = True
                break
    if not hit:
        return ""
    rest = clean_path(rest)
    if rest in ("", "."):
        return "/"
    if rest == ".." or rest.startswith("../") or "/../" in rest:
        return ""
    return "/" + rest + "/"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fsroot", default=r"E:/qt/xiangmu/Server/filesys")
    args = ap.parse_args()
    fs_root = os.path.abspath(args.fsroot).replace("\\", "/").rstrip("/")
    fs_base = os.path.basename(fs_root)
    report = []

    def emit(s):
        print(s)
        report.append(s)

    # ---- 1. 空盐账号 ----
    legacy_salt = rows("SELECT name FROM user_info WHERE salt='' ORDER BY name")
    emit(f"[1] 空盐账号: {len(legacy_salt)} 个")
    emit("    " + ", ".join(r[0] for r in legacy_salt) if legacy_salt else "    （无）")

    # ---- 2. 迁移映射异常 ----
    bad = rows("SELECT old_path,status,file_id,object_id FROM file_migration_map "
               "WHERE status<>'mapped' ORDER BY status,old_path")
    emit(f"[2] file_migration_map 非 mapped 行: {len(bad)} 条"
         + ("（其中 failed=%d）" % sum(1 for r in bad if r[1] == "failed") if bad else ""))
    for r in bad[:50]:
        emit(f"    {r[1]}\t{r[0]}\tfile_id={r[2]}\tobject_id={r[3]}")

    # 旧机器绝对路径行（normalizeLogicKeys 未收敛的遗留格式）
    all_map = rows("SELECT old_path FROM file_migration_map")
    abs_rows = [r[0] for r in all_map if not r[0].startswith(fs_base + "/")]
    emit(f"    旧机器绝对路径行（键归一遗留）: {len(abs_rows)} 条")

    # ---- 3. 非 canonical 逻辑键 ----
    nc = []
    for r in rows("SELECT file_id,owner_name,parent_path FROM user_file"):
        key = normalize_parent(r[1], r[2], fs_root)
        if key != r[2]:
            nc.append(("user_file", r[0], r[1], r[2], key))
    for r in rows("SELECT session_id,owner_name,parent_path FROM upload_session"):
        key = normalize_parent(r[1], r[2], fs_root)
        if key != r[2]:
            nc.append(("upload_session", r[0], r[1], r[2], key))
    emit(f"[3] 非 canonical 逻辑键: {len(nc)} 行（user_file/upload_session）")
    for t, i, o, p, k in nc[:50]:
        emit(f"    {t}#{i} owner={o} now={p!r} -> {k!r}")

    # ---- 4. 未迁移旧物理文件与对象覆盖 ----
    mapped = set(r[0] for r in all_map)
    unmigrated = []
    for dirpath, dirnames, filenames in os.walk(fs_root):
        rel = os.path.relpath(dirpath, fs_root).replace("\\", "/")
        top = rel.split("/")[0] if rel != "." else ""
        if top in ("objects", ".staging"):
            dirnames[:] = []
            continue
        for fn in filenames:
            key = fs_base + "/" + (rel + "/" if rel != "." else "") + fn
            if key not in mapped:
                unmigrated.append(key)
    emit(f"[4] 未映射旧物理文件（filesys/<user>/ 下不在 file_migration_map）: {len(unmigrated)} 个")
    for k in unmigrated[:50]:
        emit(f"    {k}")

    # 对象覆盖：active 条目缺 object_id（回退条目）+ file_object 指向的物理对象是否在盘
    no_obj = rows("SELECT COUNT(*) FROM user_file WHERE status='active' AND object_id IS NULL")[0][0]
    emit(f"[5] user_file active 且无对象（回退条目）: {no_obj} 行")
    miss = 0
    for r in rows("SELECT object_id,storage_path FROM file_object"):
        if not os.path.isfile(fs_root + "/" + r[1]):
            miss += 1
            if miss <= 20:
                emit(f"    缺失对象: {r[0]} {r[1]}")
    emit(f"    file_object 物理对象盘上缺失: {miss} 个")

    out = os.path.join(os.path.dirname(__file__), "audit_baseline.txt")
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
    print(f"\n基线已写入 {out}")


if __name__ == "__main__":
    main()
