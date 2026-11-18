-- REQ-DIR-COMP-001 目录操作失败补偿队列
-- 目标版本 2026111801。执行前确认目标数据库和已应用迁移。

CREATE TABLE IF NOT EXISTS directory_operation_compensation (
    task_id BIGINT NOT NULL AUTO_INCREMENT,
    dedupe_key CHAR(64) NOT NULL,
    operation_type VARCHAR(64) NOT NULL,
    account VARCHAR(128) NOT NULL,
    source_path VARCHAR(1024) NULL,
    target_path VARCHAR(1024) NULL,
    entry_id BIGINT NULL,
    source_name VARCHAR(255) NULL,
    target_name VARCHAR(255) NULL,
    operation_version BIGINT NOT NULL DEFAULT 0,
    status ENUM('pending','claimed') NOT NULL DEFAULT 'pending',
    retry_count INT NOT NULL DEFAULT 0,
    next_retry_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_error VARCHAR(1024) NULL,
    claim_owner VARCHAR(128) NULL,
    claim_until DATETIME NULL,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (task_id),
    UNIQUE KEY uk_dir_comp_dedupe (dedupe_key),
    KEY idx_dir_comp_pending (status, next_retry_at, created_at),
    KEY idx_dir_comp_claim (status, claim_until)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO schema_version (version) VALUES (2026111801);
