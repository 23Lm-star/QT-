CREATE TABLE IF NOT EXISTS share_link (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  token_hash CHAR(64) NOT NULL UNIQUE,
  owner_name VARCHAR(255) NOT NULL,
  file_id BIGINT NOT NULL COMMENT '源逻辑条目 user_file.file_id（REQ-SHARE-ENTRYID-001；不设外键，条目删除事务内同步清理）',
  expire_time DATETIME NOT NULL,
  revoked TINYINT NOT NULL DEFAULT 0,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  KEY idx_owner_file (owner_name, file_id)
);
