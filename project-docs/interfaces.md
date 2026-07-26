# 接口与数据结构约定

> 生成依据：`protocol.h`（Client 和 Server 共用）、各模块头文件
> 状态：初版

---

## 1. PDU 协议（核心接口）

### 1.1 数据结构

```cpp
// 文件：protocol.h
typedef unsigned int uint;

struct PDU {
    uint uiTotalLen;    // 报文总长度：sizeof(PDU) + uiMsgLen
    uint uiMsgLen;      // 柔性数组实际长度（消息体大小）
    uint uiType;        // 消息类型枚举值
    char caData[64];    // 固定参数区（64 字节，根据消息类型复用）
    char caMsg[];       // 柔性数组，按需分配
};
// sizeof(PDU) = 12 + 64 = 76 字节（uint=4B 平台）
```

### 1.2 辅助数据结构

```cpp
struct FileInfo {
    char caName[32];
    uint uiType;
};

struct Msg {
    uint totalLen;
    uint msgLen;
    char Time[30];
    char msg[];       // 柔性数组
};
```

### 1.3 消息类型枚举（共 43 种）

| 枚举值 | 常量 | 方向 | 说明 |
|--------|------|------|------|
| 0 | `ENUM_MSG_TYPE_MIN` | — | 最小值标记 |
| 1 | `ENUM_MSG_TYPE_REGIST_REQUEST` | C→S | 注册请求 |
| 2 | `ENUM_MSG_TYPE_REGIST_RESPOND` | S→C | 注册响应 |
| 3 | `ENUM_MSG_TYPE_LOGIN_REQUEST` | C→S | 登录请求 |
| 4 | `ENUM_MSG_TYPE_LOGIN_RESPOND` | S→C | 登录响应 |
| 5 | `ENUM_MSG_TYPE_findUser_REQUEST` | C→S | 查找用户请求 |
| 6 | `ENUM_MSG_TYPE_findUser_RESPOND` | S→C | 查找用户响应 |
| 7 | `ENUM_MSG_TYPE_ONLINEUSER_REQUEST` | C→S | 在线用户请求 |
| 8 | `ENUM_MSG_TYPE_ONLINEUSER_RESPOND` | S→C | 在线用户响应 |
| 9 | `ENUM_MSG_TYPE_ADDFRIEND_REQUEST` | C→S | 添加好友请求 |
| 10 | `ENUM_MSG_TYPE_ADDFRIEND_RESPOND` | S→C / C→C | 添加好友响应/转发 |
| 11 | `ENUM_MSG_TYPE_FLUSHFRIEND_REQUEST` | C→S | 刷新好友请求 |
| 12 | `ENUM_MSG_TYPE_FLUSHFRIEND_RESPOND` | S→C | 刷新好友响应 |
| 13 | `ENUM_MSG_TYPE_DELETEFRIEND_REQUEST` | C→S | 删除好友请求 |
| 14 | `ENUM_MSG_TYPE_DELETEFRIEND_RESPOND` | S→C | 删除好友响应 |
| 15 | `ENUM_MSG_TYPE_CHAT_REQUEST` | C→S | 聊天消息（转发） |
| 16 | `ENUM_MSG_TYPE_CHAT_RESPOND` | S→C | 聊天响应 |
| 17 | `ENUM_MSG_TYPE_GETCHATMSG_REQUEST` | C→S | 获取离线消息请求 |
| 18 | `ENUM_MSG_TYPE_GETCHATMSG_RESPOND` | S→C | 获取离线消息响应 |
| 19 | `ENUM_MSG_TYPE_MKDIR_REQUEST` | C→S | 创建目录请求 |
| 20 | `ENUM_MSG_TYPE_MKDIR_RESPOND` | S→C | 创建目录响应 |
| 21 | `ENUM_MSG_TYPE_CHANGEDIR_REQUEST` | C→S | 切换目录请求 |
| 22 | `ENUM_MSG_TYPE_CHANGEDIR_RESPOND` | S→C | 切换目录响应 |
| 23 | `ENUM_MSG_TYPE_FLUSHFILE_REQUEST` | C→S | 刷新文件列表请求 |
| 24 | `ENUM_MSG_TYPE_FLUSHFILE_RESPOND` | S→C | 刷新文件列表响应 |
| 25 | `ENUM_MSG_TYPE_DELFILE_REQUEST` | C→S | 删除文件请求 |
| 26 | `ENUM_MSG_TYPE_DELFILE_RESPOND` | S→C | 删除文件响应 |
| 27 | `ENUM_MSG_TYPE_RENAMEFILE_REQUEST` | C→S | 重命名文件请求 |
| 28 | `ENUM_MSG_TYPE_RENAMEFILE_RESPOND` | S→C | 重命名文件响应 |
| 29 | `ENUM_MSG_TYPE_UPLOADFILE_INIT_REQUEST` | C→S | 上传文件初始化请求 |
| 30 | `ENUM_MSG_TYPE_UPLOADFILE_INIT_RESPOND` | S→C | 上传文件初始化响应 |
| 31 | `ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST` | C→S | 上传文件数据块请求 |
| 32 | `ENUM_MSG_TYPE_UPLOADFILE_DATA_RESPOND` | S→C | 上传文件数据块响应 |
| 33 | `ENUM_MSG_TYPE_SHAREFILE_REQUEST` | C→S | 分享文件请求 |
| 34 | `ENUM_MSG_TYPE_SHAREFILE_RESPOND` | S→C | 分享文件回应 |
| 35 | `ENUM_MSG_TYPE_SHAREFILE_AGREE_REQUEST` | C→S | 分享文件同意请求 |
| 36 | `ENUM_MSG_TYPE_SHAREFILE_AGREE_RESPOND` | S→C | 分享文件同意回应 |
| 37 | `ENUM_MSG_TYPE_SHAREFILE_FINISH_RESPOND` | S→C | 分享文件完成 |
| 38 | `ENUM_MSG_TYPE_MOVEFILE_REQUEST` | C→S | 移动文件请求 |
| 39 | `ENUM_MSG_TYPE_MOVEFILE_RESPOND` | S→C | 移动文件回应 |
| 40 | `ENUM_MSG_TYPE_DOWNLOADFILE_REQUEST` | C→S | 下载文件请求 |
| 41 | `ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND` | S→C | 下载文件响应 |
| 42 | `ENUM_MSG_TYPE_CAPTCHA_REQUEST` | C→S | 验证码请求 |
| 43 | `ENUM_MSG_TYPE_CAPTCHA_RESPOND` | S→C | 验证码响应 |
| 99999 | `ENUM_MSG_TYPE_MAX` | — | 最大值标记 |

> **✅ 已修复**（2026-07-24）：枚举值冲突已解决，`SHAREFILE_FINISH_RESPOND`=37、`MOVEFILE_REQUEST`=38、`MOVEFILE_RESPOND`=39，拼写也已更正。两端 `handlMsg()` 均已完整处理。

### 1.4 `caData[64]` 字段复用约定

| 消息类型 | `caData[0-31]` | `caData[32-39]` | `caData[40-43]` | `caMsg` |
|----------|----------------|-----------------|------------------|---------|
| 注册请求 | 用户名 | MD5 密码哈希 | — | 验证码文本 |
| 登录请求 | 用户名 | MD5 密码哈希 | — | 验证码文本 |
| 查找用户 | 目标用户名 | — | — | — |
| 在线用户 | 当前用户名（服务器端过滤用） | — | — | — |
| 添加好友 | 操作者用户名 | 目标用户名 | — | — |
| 聊天 | 发送者 | 接收者 | — | `Msg` 结构体 |
| 上传 INIT | 文件名 | 文件总大小(`qint64`) | 魔数 `0x5253554D` | 目标目录路径 |
| 上传 DATA | 块偏移量(`qint64`) | — | 魔数校验 | 文件数据块 |
| 上传 INIT 响应 | — | — | — | 已接收偏移量（字符串） |
| 创建目录 | 目录名 | — | — | — |
| 切换目录 | 新路径 | — | — | — |
| 刷新文件 | — | — | — | 当前目录路径 |
| 删除文件 | 文件名 | — | — | 当前目录路径 |
| 重命名文件 | 旧名 | 新名 | — | 当前目录路径 |
| 下载文件 | 文件名 | — | — | 当前目录路径 |
| 分享文件 | — | 目标用户名 | — | 文件名 |

### 1.5 `mkPDU` 接口

```cpp
// 声明 (protocol.h)
PDU* mkPDU(uint uiMsgLen = 0);

// 客户端实现 (Client/protocol.cpp): malloc 分配
// 服务端实现 (Server/protocol.cpp): memalloc 分配（走内存池）
```

**约束**：客户端 PDU 必须 `free()` 释放；服务端 PDU 必须 `memfree()` 释放。**禁止跨端混用**。

> **所有权规则**：Client 端 PDU 必须用 `malloc` 分配、`free` 释放；Server 端 PDU 必须用 `memalloc` 分配、`memfree` 释放。同一进程内的分配/释放始终配对。PDU 不跨进程转移所有权。

---

## 2. 数据库接口

### 2.1 数据库配置

| 配置项 | 值 |
|--------|-----|
| 数据库类型 | MySQL |
| 驱动名 | `QMYSQL` |
| 主机 | `localhost` |
| 端口 | `3306` |
| 用户名 | `root` |
| 密码 | `123456` |
| 数据库名 | `mydb` |

> 配置硬编码在 `Server/operatedb.cpp:285` 的 `OperateDB::connect()` 中。

### 2.2 表结构

```sql
-- created implicitly via Server/operatedb.cpp usage
CREATE TABLE user_info (
    id       INT PRIMARY KEY AUTO_INCREMENT,  -- 自增主键
    name     VARCHAR(255),                    -- 用户名
    pwd      VARCHAR(255),                    -- MD5 密码哈希
    online   INT DEFAULT 0                    -- 在线状态 (0/1)
);

CREATE TABLE friend (
    user_id   INT,                            -- 用户 ID
    friend_id INT,                            -- 好友 ID
    -- 双向关系存储两条记录
);

CREATE TABLE message_info (
    seed_name   VARCHAR(255),                 -- 发送者
    rev_name    VARCHAR(255),                 -- 接收者
    msg         TEXT,                         -- 消息内容
    issuccess   INT,                          -- 送达状态 (0=未送达/1=已送达)
    seed_time   VARCHAR(255)                  -- 发送时间
);
```

### 2.3 OperateDB 接口

| 方法 | 功能 | 返回值 |
|------|------|--------|
| `handleRegit(name, pwd)` | 注册用户 | `bool` |
| `handleLogin(name, pwd)` | 登录验证 | `int` (1=成功, 0=失败, -1=用户不存在) |
| `handleOffLine(name)` | 下线 | `void` |
| `handleFindUser(name)` | 查找用户 | `int` (在线状态) |
| `handleOnlineUsers(name)` | 获取可添加的非好友 | `QStringList` |
| `handleAddFriend(usr, tar)` | 检查好友是否在线 | `int` (1=在线, 0=不在线, -1=错误) |
| `handleAddFriendFin(usr, tar)` | 建立双向好友关系 | `bool` |
| `handleFlushFriend(name)` | 获取好友列表 | `QStringList` |
| `handleDeleteFriend(usr, tar)` | 删除双向好友关系 | `bool` |
| `seedchatMsg(seed, rev, msg, time)` | 存储聊天消息 | `bool` (是否在线可达) |
| `getchatMsg(seed, rev)` | 获取离线消息 | `QStringList` |

---

## 3. 数据库连接池接口

```cpp
// DatabaseConnectionPool (单例)
class DatabaseConnectionPool {
    static DatabaseConnectionPool& instance();
    void init(host, port, user, pwd, db, maxConnPerThread=8);
    bool testConnection();
    PooledSqlConnection acquire();  // RAII 获取
    void close();
};

// PooledSqlConnection (RAII 封装)
class PooledSqlConnection {
    bool isValid() const;
    QSqlDatabase database() const;
    QSqlError lastError() const;
    // 析构时自动归还到连接池
};
```

---

## 4. 断点续传接口

```cpp
struct BeginResult {
    bool ok;
    qint64 offset;
    bool complete;
};

class ResumableUploadSession {
    BeginResult begin(dirPath, fileName, totalSize, resumeEnabled);
    bool append(data, len);
    bool isComplete() const;
    qint64 receivedSize() const;
    qint64 totalSize() const;
    void reset();
};
```

---

## 5. 服务端关键接口

### 5.1 MyTcpServer

```cpp
class MyTcpServer : public QTcpServer {
    static MyTcpServer& getinstance();
    void incomingConnection(qintptr handle) override;  // 新连接回调
    void removeSocket(MyTcpSocket* m);                // 移除断开连接
    void seedAddFriend(char* tarname, PDU* pdu);       // 转发添加好友
    void reseed(char* tarname, PDU* pdu);              // 转发消息
    QList<MyTcpSocket*> tcpsocketlist;                 // 连接列表（⚠️ 无锁）
};
```

### 5.2 MyTcpSocket

```cpp
class MyTcpSocket : public QTcpSocket {
    MsgHandler* m_MsgHandler;       // 请求处理器
    QString m_strLoginName;         // 登录用户名
    QByteArray m_dpulist;           // 接收缓冲区（粘包处理）
    PDU* handlMsg(PDU* pdu);        // 处理请求，返回响应 PDU
    void seedMsg(PDU* respdu);       // 发送响应，发送后 memfree
    void recvmsg();                  // readyRead 槽：粘包/半包处理
    void clientOffLine();            // disconnected 槽：清理状态
};
```

---

## 6. 客户端关键接口

### 6.1 Client

```cpp
class Client : public QWidget {
    static Client& getinstance();
    QTcpSocket sorket;               // TCP 连接
    QString m_LoginName;             // 当前登录用户名
    MsgHandler* m_mhptr;             // 响应分发器
    QByteArray m_pdulist;            // 接收缓冲区
    void loadconfig();               // 加载 connect.config
    void seedMsg(PDU* pdu);          // 发送 PDU，发送后 free
    void recvmsg();                  // readyRead 槽：粘包处理 → handlMsg
    void handlMsg(PDU* &pdu);        // 按 uiType 分发到 MsgHandler
};
```

### 6.2 MsgHandler (Client)

```cpp
// 响应分发函数（按服务端响应类型）
void handlrigist(), handllogin(), handlfinduser(), handlonlineuser();
void handlAddFriend(), handlAddFriendAgree(), handlFlushFriend(), handlDeleteFriend();
void chat(), getcharmsg();
void mkdir(), deldir(), changedir(), renamedir();
void flushfile(), sharefileagree(), sharefilefinsh();
void uploadfileinit(), uploadfiledata(), downloadfile();
void handleCaptcha();
```

### 6.3 MsgHandler (Server)

```cpp
// 请求处理函数（按客户端请求类型）
PDU* rigist(), *login(QString&), *finduser(), *onlineuser(const QString&);
PDU* addfriend(), *addfriendFin(), *flushfriend(const QString&), *deletefriend();
PDU* chat(), *chatmsg();
PDU* MKDIR(), *changedir(), *deldir(), *flushfile(), *renamefile();
PDU* sharefile(), *sharefilemove();
PDU* uploadfileinit(), *uploadfiledata(), *downloadfile();
PDU* captcha();
```

---

## 7. 内存分配接口

```cpp
// 服务端内存池包装层 (memalloc.h)
void* memalloc(size_t size);     // 优先走 ConCurrentAlloc，失败降级 malloc
void  memfree(void* ptr);        // 根据 MemHeader 选择 ConCurrentFree 或 free

// 直接分配入口 (ConCurrentAlloc.h)
void* ConCurrentAlloc(size_t size);
void  ConCurrentFree(void* ptr);
```

