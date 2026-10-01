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

### 1.3 消息类型枚举（基础段 1–55；扩展段见 §8 群 60–96、§9 文件 97–98）

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
| 43 | `ENUM_MSG_TYPE_DOWNLOADFILE_FINISH` | S→C | 下载完成 |
| 44 | `ENUM_MSG_TYPE_CAPTCHA_REQUEST` | C→S | 验证码请求 |
| 45 | `ENUM_MSG_TYPE_CAPTCHA_RESPOND` | S→C | 验证码响应 |
| 46 | `ENUM_MSG_TYPE_GETUSERINFORMATION_REQUEST` | C→S | 获取用户信息请求 |
| 47 | `ENUM_MSG_TYPE_GETUSERINFORMATION_RESPOND` | S→C | 获取用户信息响应 |
| 48 | `ENUM_MSG_TYPE_LOGOFF_REQUEST` | C→S | 注销(删除账号)请求 |
| 49 | `ENUM_MSG_TYPE_LOGOFF_RESPOND` | S→C | 注销(删除账号)响应 |
| 50 | `ENUM_MSG_TYPE_HEARTBEAT_REQUEST` | C→S | 登录后客户端存活探测 PING；空消息体，不访问业务或数据库 |
| 51 | `ENUM_MSG_TYPE_HEARTBEAT_RESPOND` | S→C | PONG；空消息体，仅供 ClientHeartbeat 确认 |
| 52 | `ENUM_MSG_TYPE_CREATE_PUBLINK_REQUEST` | C→S | 创建应用内分享链接；路径仅可作为当前登录用户根目录内普通文件的相对路径 |
| 53 | `ENUM_MSG_TYPE_CREATE_PUBLINK_RESPOND` | S→C | 创建结果；成功时 `caMsg` 返回 `cloud-disk://share/<opaque-token>`，不得日志输出 token |
| 54 | `ENUM_MSG_TYPE_RESOLVE_PUBLINK_REQUEST` | C→S | 解析应用内分享 token；`caMsg` 为完整链接或 token，访问者必须已登录 |
| 55 | `ENUM_MSG_TYPE_RESOLVE_PUBLINK_RESPOND` | S→C | 解析结果和受限下载元数据；成功后只允许复用既有下载流发送，不改变好友分享协议 |
| 99999 | `ENUM_MSG_TYPE_MAX` | — | 最大值标记 |

> **协议兼容基线（2026-07-29）**：下载 `40~43`、验证码 `44~45`、用户信息 `46~47`、注销 `48~49`、心跳 `50~51`、创建链接 `52~53`；本 ADR 规划的解析链接固定为 `54~55`。两端定义和所有分发 case 必须同步，既有 `33~37` 好友分享枚举严禁改动。

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
| 心跳请求/响应 | — | — | — | 空 |
| 创建应用内链接 | 文件相对路径（<=31B；或改用 caMsg 以支持长路径） | — | — | 可选过期秒数；首期为空则服务端使用默认过期时间 |
| 创建应用内链接响应 | 是否成功（4B） | — | — | 成功时返回完整不透明链接；客户端只显示/复制，不写日志 |
| 解析应用内链接 | — | — | — | 完整链接或 token |
| 解析应用内链接响应 | 是否成功（4B） | 文件名（32B） | 文件大小（qint64） | 仅允许展示所需元数据；随后走原下载流 |
| 注销/删除账号 | `ENUM_MSG_TYPE_LOGOFF_REQUEST` | 用户名（32B）+ 密码（32B） | — | — |
| | `ENUM_MSG_TYPE_LOGOFF_RESPOND` | 是否成功（4B） | — | — |

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

---

## 8. 群接口（REQ-EXT-GROUP-001 / 002）

### 8.1 群消息 PDU（88–96，REQ-EXT-GROUP-002）

| 枚举值 | 常量 | 方向 | 说明 |
|--------|------|------|------|
| 88 | `ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_REQUEST` | C→S | 发送群消息；载荷 `client_msg_id` + `content`，无 sender 字段 |
| 89 | `ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_RESPOND` | S→C | 已持久化确认：`code` + `client_msg_id` + `seq` + `server_msg_id` + `created_at_ms` |
| 90 | `ENUM_MSG_TYPE_GROUP_MESSAGE_EVENT` | S→C | 在线成员推送（仅服务端生成）：`group_id` + `seq` + `server_msg_id` + `sender` + `msg_type` + `created_at_ms` + `content` |
| 91 | `ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_REQUEST` | C→S | 按游标拉取：`mode`(0 前向补齐/1 后向翻页) + `cursor_seq` + `limit`(1–100) |
| 92 | `ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_RESPOND` | S→C | `code` + `mode` + `has_more` + `next_cursor` + `group_last_seq` + `unread_count` + `count` + N×消息记录 |
| 93 | `ENUM_MSG_TYPE_GROUP_READ_CONFIRM_REQUEST` | C→S | 已读确认：`read_seq` |
| 94 | `ENUM_MSG_TYPE_GROUP_READ_CONFIRM_RESPOND` | S→C | `code` + `effective_seq`（服务端实际生效的单调游标） |
| 95 | `ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_REQUEST` | C→S | 未读摘要，无业务字段；按 actor 汇总其全部 active 群 |
| 96 | `ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_RESPOND` | S→C | `code` + `count` + N×{`group_id` + `last_seq` + `last_read_seq` + `unread`} |

- 88/90/91/93/95 必须进 SEC-001 actor 门（未加白名单）；90 由服务端生成，客户端发入的同名类型一律丢弃并记审计日志。
- 生命周期 PDU（GROUP-001）为 60–87：建群/邀请/接受/拒绝/退出/移除/任免/转让/解散/群列表/成员列表/邀请查询请求-响应，成员事件 84、邀请事件 85。编号只追加、不重排。

### 8.2 群信封与载荷编码

```cpp
// 文件：protocol.h（Client / Server 同值同名）
static const quint16 GROUP_PROTOCOL_VERSION = 1;
static const quint16 GROUP_ENVELOPE_LENGTH  = 20;
// 信封 = version(u16) + envelope_length(u16) + request_id(u64) + group_id(u64)，全部小端手写编解码
inline void groupAppendU16/U32/U64(...);   // groupAppendString = u32 length + UTF-8 bytes
inline bool groupReadU16/U32/U64/String(...);  // 越界返回 false
```

- 响应在信封后附 `code`(u32，0 成功 / 负数为业务错误)，再附业务字段；业务字段为定宽整数或 `u32 length + UTF-8 bytes` 字符串。
- 服务端先验 `uiMsgLen`、信封长度、版本与每个字段边界，`offset != payload.size()` 即判越界载荷并拒绝；未知类型不进群入口。
- 校验：`content` 非空且 ≤2000B UTF-8；`client_msg_id` 非空且 ≤64B；`limit` ∈ [1,100]；`mode` 只能 0/1。
- 消息记录编码（92 内复用）：`seq`(u64) + `server_msg_id`(u64) + `sender`(string) + `msg_type`(u8: 0 文本/1 系统) + `created_at_ms`(u64) + `content`(string)。

### 8.3 `group_*` 表契约

| 表 | 来源 | 关键列与键 | 语义 |
|----|------|-----------|------|
| `group_info` | GROUP-001 | `group_id` 自增主键；`group_name`、`creator_name`、`status('active'/'dissolved')`、`version`；`last_seq`（GROUP-002 追加） | 群基本信息。`version` 随成员/角色/资料变化递增；`last_seq` 为群内序号水位，事务内原子递增；解散用状态更新不物理删除 |
| `group_member` | GROUP-001 | 主键 `(group_id, user_name)`；`role('owner'/'admin'/'member')`、`status('active'/'left'/'removed')`、`last_read_seq`、`joined_at` | 成员关系唯一事实源，也是未读游标载体。重入只改回 `active` 不插第二行；有效成员由 `status='active'` 判定 |
| `group_invitation` | GROUP-001 | `invitation_id` 主键；`status('pending'/'accepted'/'rejected'/'expired')`；唯一键 `(group_id, request_key)` | 群邀请及其幂等键；接受前重新比较 `expires_at` |
| `group_message` | GROUP-002 | `msg_id` 自增主键；唯一键 `uk_group_seq(group_id, seq)`、`uk_group_client_msg(group_id, client_msg_id)`；索引 `idx_group_created(group_id, created_at)`；`sender_name`、`msg_type('text'/'system')`、`content TEXT`、`created_at DATETIME(3)` | 群消息持久化。`sender_name` 由服务端 actor 推导；`client_msg_id` 为幂等键（系统消息为 NULL，NULL 互不相等故可多行）；排序游标一律以 `seq` 为准 |

- 迁移版本：GROUP-001 `2026093001`、GROUP-002 `2026100101`，写入 `schema_version`；`OperateDB::checkSchemaVersion()` 取文件资产/群/群消息三者 `qMax`，低版本 fail-fast，不运行时静默建表。

### 8.4 OperateDB 群接口

```cpp
// 业务层内部操作码（非网络 PDU 编号）
enum GroupOperation {
    GroupCreate = 1, GroupInvite, GroupAcceptInvitation, GroupRejectInvitation,
    GroupLeave, GroupRemoveMember, GroupSetRole, GroupTransferOwner,
    GroupDissolve, GroupList, GroupListMembers, GroupQueryInvitations, GroupSummary,
    GroupSendMessage, GroupPullMessages, GroupReadConfirm, GroupUnreadSummary
};
struct GroupRequest { /* groupId, targetName, requestKey, role,
                        clientMsgId, content, pullMode, cursorSeq, limit, readSeq */ };
struct GroupMessageRecord { quint64 seq, msgId; QString sender; quint8 msgType;
                            quint64 createdAtMs; QString content; };
struct GroupResult { int code; QByteArray responseBody; QStringList notifyUsers;
                     quint64 groupId, version; int eventType;
                     GroupMessageRecord message; bool broadcastMessage; };

GroupResult handleGroupOperation(const GroupRequest&, const QString &actor);        // 生命周期
GroupResult handleGroupMessageOperation(const GroupRequest&, const QString &actor); // 88–96
```

### 8.5 业务错误码

| 码 | 含义 |
|----|------|
| 0 | 成功 |
| -1001 / -1002 | 未认证主体 / 数据库不可达 |
| -2001 / -2002 | 非法信封（版本或长度不符）/ 越界载荷（字段边界或取值非法） |
| -2101 | 非成员（非成员/已退出/被移除即时失效） |
| -2102 | 群不存在或已解散 |
| -2103 | 内容或幂等键非法（空/超长） |
| -2104 | 幂等冲突异常（同键异发送者等） |
| -2105 | 序号分配或入库失败（含 DB 失败） |

### 8.6 客户端可见操作

- 发送：发 88，收 89 前为「发送中」，成功转「已发送」，失败可对同一 `client_msg_id` 重试（服务端幂等）。
- 接收：`Client::handlMsg` 收 90，按 `group_id` 路由到对应 `GroupChat`；未打开窗口只累加未读并提示；90 与 92 结果按 `seq` 去重。
- 历史：打开窗口经 95/96 定位游标，再以 91/92 `mode=0` 前向补齐，到达水位后可 `mode=1` 上翻；`mode=1` 为 `seq <= cursor` 含边界语义，调用方须传 `next_cursor - 1`。
- 已读：93/94 单调推进 `last_read_seq`（不可回退、不可代推、不超过群水位）。
- 未读：群列表经 95/96 显示徽标，服务端计算未读数，客户端不可自报。

---

## 9. 文件接口（REQ-EXT-FILE-001 / 002）

### 9.1 秒传预检查 PDU（97–98，REQ-EXT-FILE-002）

| 枚举值 | 常量 | 方向 | 说明 |
|--------|------|------|------|
| 97 | `ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_REQUEST` | C→S | 秒传预检查；只追加，旧编号 1–96 不变 |
| 98 | `ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_RESPOND` | S→C | `ret`+`hit`+`version`+`file_id`+`session_id`+`next`+`name_len`；命中时 `caMsg` 为最终展示名 |

- 97/98 必须进 SEC-001 actor 门；未认证请求拒绝，响应类型正确。
- 既有分块上传 29–32 语义**零改动**，预检查未命中时复用既有 `Uploader` 链路。

### 9.2 秒传预检查布局常量

```cpp
// 文件：protocol.h（Client / Server 同值同名，只追加）
static const uint FASTCHECK_MAGIC      = 0x4643484B;  // "FCHK"
static const uint FASTCHECK_VERSION    = 1;           // 请求版本不等于服务端即拒
static const int  FASTCHECK_MAX_MSG_LEN = 1024;

// 请求 caData：定长字段与长度字段；可变长度（sha/名/目录/reqId）一律进 caMsg
//   0 version(u32) | 4 magic(u32) | 8 file_size(qint64) | 16 sha_len(u32,须=64)
//   20 name_len(u32) | 24 dir_len(u32) | 28 reqid_len(u32)
// 响应 caData：0 ret(bool) | 1 hit(bool) | 2 version(u32) | 8 file_id(qint64)
//   16 session_id(qint64) | 24 next(qint64,未命中续传起点) | 32 name_len(u32)
static const int FASTCHECK_SHA_HEX_LEN   = 64;   // 小写十六进制
static const int FASTCHECK_REQID_HEX_LEN = 32;   // 客户端请求级幂等键
```

- 校验：sha 严格 `[0-9a-f]{64}`；请求总长 == `uiMsgLen` 且不超 `FASTCHECK_MAX_MSG_LEN`；目录经 `logicPathInActorRoot` + `normalizeParent` 双层约束。

### 9.3 文件资产表契约（REQ-EXT-FILE-001）

| 表 | 关键列与键 | 语义 |
|----|-----------|------|
| `schema_version` | `version` 主键 | 迁移版本水位；启动 `checkSchemaVersion()` fail-fast |
| `file_object` | `object_id` 主键；唯一键 `uk_sha256(sha256)`；`size_bytes`、`storage_path`（`objects/<sha前2位>/<object_id>`）、`refcount`、`status('ready'/'pending_reclaim')` | 物理对象层，内容寻址，物理唯一性由 DB 保证 |
| `user_file` | `file_id` 主键；唯一键 `uk_loc(owner_name, parent_path, display_name)`；索引 `idx_owner(owner_name, parent_path, status)`；`is_dir`、`object_id`、`status('active'/'deleted')` | 逻辑条目层（目录树）；`parent_path` 为 owner 相对逻辑键；外键指向 `file_object` |
| `upload_session` | `session_id` 主键；`expected_size`、`client_sha256`（仅预检元数据，不作转正依据）、`staging_path`、`confirmed_offset`、`status('uploading'/'verifying'/'done'/'failed'/'expired')`；FILE-002 追加 `client_request_id VARCHAR(64)`、`file_id BIGINT`、唯一键 `uk_req(owner_name, client_request_id)` | 断点续传与秒传会话；`uk_req` 为请求级幂等键（存量行 NULL 不冲突） |
| `file_migration_map` | `old_path` 主键；`file_id`、`object_id`、`status('mapped'/'skipped_name_conflict'/'failed')` | FILE-001 存量迁移台账，幂等重放 |

- 迁移版本：FILE-001 `2026092901`、FILE-002 `2026100102`；`OperateDB::checkSchemaVersion()` 取文件资产/群/群消息/秒传四者 `qMax`，低版本 fail-fast，不运行时静默建表。

### 9.4 FileObjectStore / OperateDB 文件接口

```cpp
// Server/fileobjectstore.{h,cpp}
bool   hashStaging(...);                                   // 暂存文件流式摘要
int    commitUpload(..., const QString& expectedSha);      // 事务内复算大小+摘要 → 转正/命中安置
int    attachEntry(...);                                   // 条目建/复活 + refcount++
int    attachExistingObject(...);                          // 秒传命中：同事务安置条目，丢弃 staging
bool   findObjectBySha(...);                               // uk_sha256 精确命中查询
int    releaseEntry(...);                                  // 条目释放 + refcount-- ；归零转 pending_reclaim
void   normalizeParent(...);                               // 逻辑父路径规范化（owner 相对、'/' 结尾）
bool   logicPathInActorRoot(...);                          // actor 根内逻辑路径校验

// Server/operatedb.{h,cpp}（文件资产部分）
int  createUploadSession(...);      int updateUploadOffset(...);   int finishUploadSession(...);
int  lookupUploadSessionByRequestId(...);   // 秒传请求级幂等短路
int  restartUploadSession(...);              int insertCompletedUploadSession(...);
bool isLogicKeyForm(...);            void normalizeLogicKeys(...); // 逻辑键判别与一次性转换
bool checkSchemaVersion();           // 四版本 qMax 达标检查
```


