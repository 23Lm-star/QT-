from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def test_directory_compensation_schema_and_api_are_declared():
    migration = ROOT / "Server" / "migrate_dir_comp_001.sql"
    rollback = ROOT / "Server" / "migrate_dir_comp_001_rollback.sql"
    header = ROOT / "Server" / "operatedb.h"
    assert migration.exists(), "目录补偿迁移脚本缺失"
    sql = migration.read_text(encoding="utf-8")
    assert "directory_operation_compensation" in sql
    assert "claim_until" in sql
    assert "idx_dir_comp_pending" in sql
    assert "2026111801" in sql
    rollback_text = rollback.read_text(encoding="utf-8")
    assert "DROP TABLE IF EXISTS directory_operation_compensation" in rollback_text
    text = header.read_text(encoding="utf-8")
    assert "enqueueDirectoryCompensation" in text
    assert "claimDirectoryCompensations" in text
    assert "peekDirectoryCompensations" in text
    assert "retryDirectoryCompensation" in text
    implementation = (ROOT / "Server" / "operatedb.cpp").read_text(encoding="utf-8")
    assert "sourceName + QChar" in implementation
    store = (ROOT / "Server" / "fileobjectstore.cpp").read_text(encoding="utf-8")
    assert "QFileInfo::exists(createdFinalAbs) && !QFile::remove(createdFinalAbs)" in store
    scheduler = (ROOT / "Server" / "reclaimscheduler.cpp").read_text(encoding="utf-8")
    e2e = ROOT / "project-docs" / "evidence" / "REQ-DIR-COMP-001" / "dircomp_e2e.py"
    assert e2e.exists(), "目录补偿专项场景脚本缺失"
    e2e_text = e2e.read_text(encoding="utf-8")
    assert "failed task retained" in e2e_text
    assert "expired lease task" in e2e_text
    for operation_type in (
        "create_storage_root", "create_user_root", "delete_user_root",
        "quarantine_user_root", "create_directory", "delete_directory",
        "rename_directory", "move_directory", "rename_file", "move_file",
        "commit_staged_file", "rollback_staged_file", "delete_staging_file",
        "delete_object_file", "cleanup_orphan_directory",
        "repair_directory_entries",
    ):
        assert operation_type in scheduler, f"调度器缺少操作类型 {operation_type}"


if __name__ == "__main__":
    test_directory_compensation_schema_and_api_are_declared()
    print("directory compensation schema/api: PASS")
