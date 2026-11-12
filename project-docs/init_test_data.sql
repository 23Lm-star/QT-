-- 数据库初始化脚本 (回归原始双名字段结构)
SET FOREIGN_KEY_CHECKS = 0;
DROP TABLE IF EXISTS friend;
DROP TABLE IF EXISTS login_record;
DROP TABLE IF EXISTS user_info;
SET FOREIGN_KEY_CHECKS = 1;

CREATE TABLE user_info (
    id INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(32) UNIQUE NOT NULL,
    pwd VARCHAR(32) NOT NULL,
    online INT DEFAULT 0,
    salt VARCHAR(32) NOT NULL,
    login_version BIGINT NOT NULL DEFAULT 0
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- REQ-EXT-SESSION-001 登录档案（每次成功登录一行；种子不插登录行）
CREATE TABLE login_record (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(32) NOT NULL,
    client_id VARCHAR(64) NULL DEFAULT NULL,
    login_version BIGINT NOT NULL,
    login_time DATETIME NOT NULL,
    KEY idx_user_ver (username, login_version)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE friend (
    id INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(32) NOT NULL,
    friendName VARCHAR(32) NOT NULL,
    UNIQUE KEY (name, friendName)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 插入测试用户（注意：pwd 字段存储的是服务端加盐后的 32 字符 hex 串，不是明文！）
-- REQ-MIG-001 起无空盐分支：pwd = md5(客户端MD5 + salt)；缺 salt 的账号无法登录
-- 123456 的 MD5 = e10adc3949ba59abbe56e057f20f883e；种子盐固定 0123456789abcdef0123456789abcdef
-- 加盐后 pwd = 8cd647f4cec7bdfe190aee5399a6a638
INSERT INTO user_info (name, pwd, salt, online) VALUES
 ('tester',  '8cd647f4cec7bdfe190aee5399a6a638', '0123456789abcdef0123456789abcdef', 1),
 ('friend1', '8cd647f4cec7bdfe190aee5399a6a638', '0123456789abcdef0123456789abcdef', 1),
 ('friend2', '8cd647f4cec7bdfe190aee5399a6a638', '0123456789abcdef0123456789abcdef', 0);

-- 建立好友关系 (name-friendName)
INSERT INTO friend (name, friendName) VALUES ('tester', 'friend1');
