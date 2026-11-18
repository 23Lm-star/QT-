from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def test_directory_compensation_schema_and_api_are_declared():
    migration = ROOT / "Server" / "migrate_dir_comp_001.sql"
    header = ROOT / "Server" / "operatedb.h"
    assert migration.exists(), "目录补偿迁移脚本缺失"
    sql = migration.read_text(encoding="utf-8")
    assert "directory_operation_compensation" in sql
    assert "claim_until" in sql
    assert "idx_dir_comp_pending" in sql\n    assert "2026111801" in sql
    text = header.read_text(encoding="utf-8")
    assert "enqueueDirectoryCompensation" in text
    assert "claimDirectoryCompensations" in text
    assert "retryDirectoryCompensation" in text


if __name__ == "__main__":
    test_directory_compensation_schema_and_api_are_declared()
    print("directory compensation schema/api: PASS")

