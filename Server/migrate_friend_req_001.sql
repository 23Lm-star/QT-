-- REQ-AUDIT-001 R4：好友邀请持久化（接受必须存在待处理邀请，且由服务端保存的请求方决定方向）
-- 目标 schema_version = 2026111201；服务端启动检查不达标则拒绝监听。
-- 回滚见 migrate_friend_req_001_rollback.sql。

CREATE TABLE IF NOT EXISTS friend_request (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    requester_name VARCHAR(32) NOT NULL,
    target_name VARCHAR(32) NOT NULL,
    status VARCHAR(16) NOT NULL DEFAULT 'pending',
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    handled_at DATETIME NULL DEFAULT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uk_friend_req_pending (requester_name, target_name, status),
    KEY idx_friend_req_target (target_name, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO schema_version (version) VALUES (2026111201);
