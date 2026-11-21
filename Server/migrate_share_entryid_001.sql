-- REQ-SHARE-ENTRYID-001 分享链接源定位改绑逻辑条目标识
-- 目标版本 2026112101。执行前确认目标数据库和已应用迁移。
-- 存量口径（2026-11-21 用户确认）：直接清空，不做 path→file_id 迁移；旧链接全部失效需重建。

DELETE FROM share_link;

ALTER TABLE share_link DROP COLUMN relative_path;

ALTER TABLE share_link
  ADD COLUMN file_id BIGINT NOT NULL COMMENT '源逻辑条目 user_file.file_id（不设外键，条目删除事务内同步清理）' AFTER owner_name,
  ADD KEY idx_owner_file (owner_name, file_id);

INSERT IGNORE INTO schema_version (version) VALUES (2026112101);
