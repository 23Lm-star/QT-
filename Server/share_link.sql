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

-- 全新库：建表即达版本门槛；仅当本库 share_link 已是 file_id 新结构才落戳——
-- 旧结构库不落戳，启动门继续拦截，强制走 migrate_share_entryid_001.sql
-- （清空存量 + 改列），防止重跑本脚本把旧库标到新版本后运行期 SQL 报未知列。
INSERT IGNORE INTO schema_version (version)
SELECT 2026112101 FROM DUAL WHERE EXISTS (
  SELECT 1 FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'share_link' AND COLUMN_NAME = 'file_id'
);
