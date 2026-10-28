-- REQ-CLI-MSGBND-001 私聊正文上限抬到 2000 字节 迁移脚本（目标版本 2026102801）
-- 执行：mysql -u<user> -p <db> < migrate_msgbnd_001_widen_msg.sql
-- 目的：message_info.msg 原为 varchar(100)（按字符计）。sql_mode 含 STRICT_TRANS_TABLES 时
-- 超长正文直接 1406 报错，seedchatMsg 失败会让服务端既不入库也不转发，2000 字节上限形同虚设。
-- 2000 个字符可容纳任意 2000 UTF-8 字节正文（最坏情况全为 ASCII），故列宽取 varchar(2000)。
-- 附加式（additive）：只扩列宽，不新增表/列、不删数据、不改 NULL 语义与字符集
-- （表排序规则实测已是 utf8mb4_general_ci，4 字节字符本就能入库）。
-- 幂等可重放：MySQL 5.7 无 MODIFY COLUMN IF EXISTS，用 information_schema 判断当前宽度。
-- 行大小：varchar(2000) utf8mb4 最大 8000B，InnoDB(DYNAMIC) 溢出页存放，不与同表其它列冲突
-- （message_info 其余列为 varchar(32)/varchar(32)/tinyint/varchar(30)，无组合长度问题）。
-- 回滚见 migrate_msgbnd_001_rollback.sql —— 缩回 varchar(100) 会截断（STRICT 下报错）已存在的
-- 长消息，属有损回滚，只在确认库里无 >100 字符行时才执行，不作常规回退路径。

SET @need_widen := (SELECT COUNT(*) FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'message_info'
    AND COLUMN_NAME = 'msg' AND CHARACTER_MAXIMUM_LENGTH < 2000);
SET @ddl := IF(@need_widen > 0,
  'ALTER TABLE message_info MODIFY COLUMN msg VARCHAR(2000) NULL',
  'SELECT 1');
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

INSERT IGNORE INTO schema_version (version) VALUES (2026102801);
