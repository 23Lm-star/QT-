import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DIST = REPO / "dist"
EXE = DIST / "Server.exe"
FS = DIST / "filesys"
ROOT = FS / "dircompuser"
CFG = DIST / "reclaim.cfg"
MYSQL = os.environ.get(
    "DIR_COMP_MYSQL",
    r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe",
)
DB_ARGS = [MYSQL, "-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
PREFIX = "dircomp-e2e-"

sys.path.insert(0, str(REPO / "project-docs" / "evidence" / "_harness"))
from server_ctl import ServerCtl

PASS = FAIL = 0


def sql(statement):
    result = subprocess.run(DB_ARGS + [statement], capture_output=True, text=True,
                            errors="replace")
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or "mysql failed")
    return [line.split("\t") for line in result.stdout.splitlines() if line]


def scalar(statement):
    rows = sql(statement)
    return rows[0][0] if rows and rows[0] else None


def check(name, actual, expected):
    global PASS, FAIL
    if actual == expected:
        print(f"  PASS  {name} = {actual}")
        PASS += 1
    else:
        print(f"  FAIL  {name} = {actual} (expected {expected})")
        FAIL += 1


def task(dedupe, operation, source="", target="", status="pending", claim=""):
    source_sql = "NULL" if not source else "'{}'".format(source)
    target_sql = "NULL" if not target else "'{}'".format(target)
    claim_sql = "NULL" if not claim else "'{}'".format(claim)
    sql(
        "INSERT INTO directory_operation_compensation "
        "(dedupe_key,operation_type,account,source_path,target_path,status,next_retry_at,claim_owner,claim_until) "
        f"VALUES ('{dedupe}','{operation}','dircompuser',{source_sql},{target_sql},"
        f"'{status}',NOW(),{claim_sql},DATE_SUB(NOW(),INTERVAL 1 MINUTE))"
    )


def write_cfg():
    CFG.write_text(
        "enabled=1\n"
        "dry_run=0\n"
        "tick_ms=500\n"
        "interval_minutes=60\n"
        "per_tick=20\n"
        "per_round_limit=100\n"
        "staging_ttl_hours=6\n"
        "grace_hours=6\n"
        "directory_retry_minutes=1\n"
        "directory_lease_seconds=30\n",
        encoding="utf-8",
        newline="\n",
    )


def run_server(seconds=3):
    ctl = ServerCtl(str(EXE), str(DIST), "127.0.0.1", 5000)
    if not ctl.start():
        raise RuntimeError(ctl.last_error)
    try:
        time.sleep(seconds)
    finally:
        ctl.stop()


def clean():
    sql(f"DELETE FROM directory_operation_compensation WHERE dedupe_key LIKE '{PREFIX}%'")
    shutil.rmtree(ROOT, ignore_errors=True)


def main():
    backup = CFG.read_bytes() if CFG.exists() else None
    try:
        clean()
        ROOT.mkdir(parents=True, exist_ok=True)
        (ROOT / "todelete").mkdir()
        (ROOT / "movesrc").mkdir()
        (ROOT / "movesrc" / "payload.txt").write_text("payload", encoding="utf-8")
        (ROOT / "fixed").mkdir()

        task(PREFIX + "create", "create_directory", target="dircompuser/newdir")
        task(PREFIX + "delete", "delete_directory", source="dircompuser/todelete")
        task(PREFIX + "move", "move_directory", source="dircompuser/movesrc",
             target="dircompuser/movedst")
        task(PREFIX + "retry", "move_directory", source="dircompuser/missing",
             target="dircompuser/fixed/moved")
        task(PREFIX + "invalid", "delete_directory", source="__INVALID_PATH__")
        task(PREFIX + "lease", "create_directory", status="claimed", claim="old-owner",
             target="dircompuser/lease-recovered")
        write_cfg()
        run_server()

        for key in ("create", "delete", "move", "lease"):
            check(f"{key} task completed", scalar(
                f"SELECT COUNT(*) FROM directory_operation_compensation WHERE dedupe_key='{PREFIX}{key}'"), "0")
        check("create directory", str((ROOT / "newdir").is_dir()), "True")
        check("delete directory", str((ROOT / "todelete").exists()), "False")
        check("move source", str((ROOT / "movesrc").exists()), "False")
        check("move target", str((ROOT / "movedst").is_dir()), "True")
        check("expired lease task", str((ROOT / "lease-recovered").is_dir()), "True")

        retry_status = sql(
            f"SELECT status,retry_count FROM directory_operation_compensation "
            f"WHERE dedupe_key='{PREFIX}retry'"
        )
        check("failed task retained", retry_status[0][0] if retry_status else None, "pending")
        check("failed task retried", retry_status[0][1] if retry_status else None, "1")
        invalid_status = sql(
            f"SELECT status FROM directory_operation_compensation WHERE dedupe_key='{PREFIX}invalid'"
        )
        check("invalid path retained", invalid_status[0][0] if invalid_status else None, "pending")

        (ROOT / "missing").mkdir()
        sql(f"UPDATE directory_operation_compensation SET next_retry_at=NOW() "
            f"WHERE dedupe_key='{PREFIX}retry'")
        run_server()
        check("retried task completed", scalar(
            f"SELECT COUNT(*) FROM directory_operation_compensation WHERE dedupe_key='{PREFIX}retry'"), "0")
        check("retried move target", str((ROOT / "fixed" / "moved").is_dir()), "True")
        return 0 if FAIL == 0 else 1
    finally:
        try:
            clean()
        finally:
            if backup is not None:
                CFG.write_bytes(backup)
            else:
                CFG.unlink(missing_ok=True)
        print(f"PASS={PASS} FAIL={FAIL}")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FATAL {exc}")
        raise
