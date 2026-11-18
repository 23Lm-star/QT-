-- REQ-DIR-COMP-001 回滚脚本
-- 仅在确认没有待保留补偿任务且已停止相关服务后执行。
DELETE FROM schema_version WHERE version = 2026111801;
DROP TABLE IF EXISTS directory_operation_compensation;
