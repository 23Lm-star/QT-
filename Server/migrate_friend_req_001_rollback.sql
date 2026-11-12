-- REQ-AUDIT-001 R4 回滚：好友邀请表
-- 先确认没有依赖本表的新逻辑在运行（旧版本服务端不使用该表，回滚安全）。
DROP TABLE IF EXISTS friend_request;
DELETE FROM schema_version WHERE version = 2026111201;
