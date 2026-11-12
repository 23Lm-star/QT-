-- migrate_add_password_salt.sql
-- REQ-AUTH-002：密码哈希加盐迁移
-- 历史：salt = '' 曾表示迁移前老数据（裸 MD5 比对，登录成功后自动补盐）；
-- REQ-MIG-001 起空盐兼容分支已删除，空盐账号无法登录。本脚本仅作历史迁移记录。

ALTER TABLE user_info ADD COLUMN salt VARCHAR(32) NOT NULL DEFAULT '';

-- 校验：老数据 salt 应为空串
-- SELECT name, pwd, salt FROM user_info;
