-- REQ-CLI-MSGBND-001 迁移回滚：message_info.msg 缩回 varchar(100)
-- 有损操作。本脚本自带阻塞检查：库内存在 >100 字符的私聊行时直接报错中止，不会静默截断内容。
-- 机制：先把会话语态强制加上 STRICT_TRANS_TABLES，再执行收窄的 ALTER ——
--       严格模式下已有长行放不下会让 ALTER 以 1265「Data truncated for column 'msg'」失败
--       （脚本中止，表结构保持 varchar(2000)）；不强制严格模式时 MySQL 会静默截断，那是不可接受的默认行为。
-- 已实测：本机隔离测试库存在一条 1077 字符的真实私聊行时执行本脚本 → 第 25 行报 1265 中止，
--         列宽复核仍为 varchar(2000)，该行长内容未受影响。
-- 中止后的处理由人工决定：清理这些行后重跑，或接受该批消息不可回退并另行授权。
-- 幂等：已经是 varchar(100) 时不做任何事。
-- 回滚后需重新执行 migrate_msgbnd_001_widen_msg.sql 才能恢复 2000 字节上限；
-- 未恢复前不要只升级客户端/网页端（正文上限与列宽不匹配）。

-- 1) 先报告残留长行数量（不为 0 时下面的 ALTER 会失败）
SELECT COUNT(*) AS rows_longer_than_100 FROM message_info WHERE CHAR_LENGTH(msg) > 100;

-- 2) 强制严格模式：宁可失败也不静默截断
SET SESSION sql_mode = CONCAT(@@SESSION.sql_mode, ',STRICT_TRANS_TABLES');

-- 3) 条件收窄
SET @need_narrow := (SELECT COUNT(*) FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'message_info'
    AND COLUMN_NAME = 'msg' AND CHARACTER_MAXIMUM_LENGTH > 100);
SET @ddl := IF(@need_narrow > 0,
  'ALTER TABLE message_info MODIFY COLUMN msg VARCHAR(100) NULL',
  'SELECT 1');
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
