#ifndef OPERATEDB_H
#define OPERATEDB_H

#include <QObject>
#include <QByteArray>
#include <QDateTime>
#include <QStringList>
//数据库操作类
class OperateDB : public QObject
{
    Q_OBJECT
public:
    ~OperateDB();
    //管理/维护一个数据库的连接
    void connect();
    static OperateDB& instance();//单例模式
    //REQ-EXT-FILE-001 T1: 文件资产 schema 版本（D1 启动 fail-fast，不静默建表）
    static const int kFileAssetSchemaVersion = 2026092901;
    // REQ-EXT-GROUP-001：群生命周期迁移版本；启动时要求数据库至少达到此版本。
    static const int kGroupSchemaVersion = 2026093001;
    // REQ-EXT-GROUP-002：群消息迁移版本（group_info.last_seq + group_message 表）。
    static const int kGroupMessageSchemaVersion = 2026100101;
    //启动检查：schema_version 达标返回 true；库不可达/表缺失/版本低返回 false 并打日志
    bool checkSchemaVersion();

    // 群请求操作编号（业务层内部枚举，不等同于网络 PDU 编号）。
    enum GroupOperation {
        GroupCreate = 1, GroupInvite, GroupAcceptInvitation, GroupRejectInvitation,
        GroupLeave, GroupRemoveMember, GroupSetRole, GroupTransferOwner,
        GroupDissolve, GroupList, GroupListMembers, GroupQueryInvitations,
        GroupSummary,
        // REQ-EXT-GROUP-002：群消息（发送/拉取/已读/未读）
        GroupSendMessage, GroupPullMessages, GroupReadConfirm, GroupUnreadSummary
    };
    struct GroupRequest {
        GroupOperation operation;
        quint64 groupId;             // 群标识
        quint64 invitationId;        // 邀请标识
        QString groupName;           // 群名称
        QString targetName;          // 目标用户
        QString requestKey;          // 幂等键
        QString role;                // 角色 owner/admin/member
        // REQ-EXT-GROUP-002 字段
        QString clientMsgId;         // 发送幂等键（非空，<=64B）
        QString content;             // 消息正文（<=2000B UTF-8，非空）
        quint8 pullMode = 0;         // 0 前向补齐 / 1 后向翻页
        quint64 cursorSeq = 0;       // 拉取游标 seq
        quint16 limit = 50;          // 拉取条数（已钳制 1-100）
        quint64 readSeq = 0;         // 已读确认目标 seq（服务端单调钳制）
    };
    // REQ-EXT-GROUP-002：需要在线广播（PDU 90）的群消息记录
    struct GroupMessageRecord {
        quint64 seq = 0;
        quint64 msgId = 0;
        QString sender;
        quint8 msgType = 0;          // 0 文本 / 1 系统
        quint64 createdAtMs = 0;
        QString content;
    };
    struct GroupResult {
        int code;                    // 0 成功，负数为业务错误
        QByteArray responseBody;     // 已编码的响应业务字段
        QStringList notifyUsers;     // 需要接收成员事件的账号
        quint64 groupId;             // 事件所属群
        quint64 version;             // 提交后的群版本
        int eventType;               // 0 无事件，1 成员变化，2 邀请变化
        GroupMessageRecord message;  // REQ-EXT-GROUP-002：广播用消息记录
        bool broadcastMessage = false; // true=提交成功后广播（幂等重放=false）
    };
    // 在单个数据库事务中执行群状态变更或隔离查询；actor 来自连接认证上下文。
    GroupResult handleGroupOperation(const GroupRequest &request, const QString &actor);
    // REQ-EXT-GROUP-002：群消息操作（发送/拉取/已读确认/未读摘要）；actor 来自连接认证上下文。
    GroupResult handleGroupMessageOperation(const GroupRequest &request, const QString &actor);
    //REQ-EXT-FILE-001 T6：存量文件启动自动迁移（幂等：file_migration_map 已有记录即跳过；
    //失败记 failed 继续其余，绝不删除旧文件——验收5）。main 在版本检查通过后调用一次。
    void migrateLegacyFiles();
    //REQ-EXT-FILE-001 F-3/D5 修复：存量"绝对机器路径"键 → owner 相对逻辑键的一次性确定性转换
    //（幂等可重放；uk_loc 冲突按 file_id 最早保留，后到 active 行软删+减引用）。
    //main 在 checkSchemaVersion 后、migrateLegacyFiles 前调用。
    void normalizeLogicKeys();
    //执行注册用户操作函数
    bool handleRegit(char* strname,char*strpwd);
    //登录用户
    // REQ-EXT-SESSION-001：clientId=登录携带的客户端唯一标识（可空）；成功(ret==1)时
    // outVersion=本次登录在事务内原子分配的用户域版本号
    int handleLogin(char*strname,char*strpwd,const QString& clientId,quint64& outVersion);
    //下线
    void handleOffLine(const char* strname);
    //查找用户
    int handleFindUser(const char* userName);
    //获取所有用户
    QStringList handleOnlineUsers(const QString& strname);
    //添加好友操作，查找
    int handleAddFriend(char* usrname,char* tarname);
    //添加好友操作，添加
    int handleAddFriendFin(char* usrname,char* tarname);
    //刷新好友操作，
    QStringList handleFlushFriend(const QString&strname);
    //删除好友操作
    bool handleDeleteFriend(char*userName,char*tarName);
    //记录聊天消息信息
    bool seedchatMsg(char*seedName,char *revName,QString msg,QString seedTime);
    //获取聊天信息
    QStringList getchatMsg(char*seedName,char *revName);
    //获取用户信息
    bool handleGetUserInfo(const char* userName, QString& outName, int& outOnline);
    //注销/删除用户账户
    bool handleLogoff(const char* name, const char* pwd);
    //分享链接操作
    bool createPublishLink(const QString &code, const QString &path, const QString &creator);
    QString getFilePathByCode(const QString &code);
    
    // 新增：安全外链分享操作（BUG-ARC-PUBLINK-001）
    bool createShareLink(const QString &hash, const QString &owner, const QString &path, const QDateTime &expire);
    struct ShareLinkInfo {
        QString ownerName;
        QString relativePath;
        QDateTime expireTime;
        bool revoked;
    };
    bool getShareLinkByHash(const QString &hash, ShareLinkInfo &outInfo);
    QString getShareLinkByPath(const QString &owner, const QString &path);

    // 跨功能联动维护（文件删除/重命名/移动 → 分享链接记录一致性）
    bool deleteShareLinkByPath(const QString &owner, const QString &path);
    bool updateShareLinkPath(const QString &owner, const QString &oldPath, const QString &newPath);
    // 删除好友时清理双方私聊记录
    bool deleteFriendMessages(const QString &userA, const QString &userB);

    // ===== REQ-EXT-FILE-001 T4：上传会话持久化（upload_session 表） =====
    // 登记暂存会话；返回 session_id（<0 失败）。ttlSec<=0 时默认 3600。
    qint64 createUploadSession(const QString &owner, const QString &parentPath,
                               const QString &displayName, qint64 expectedSize,
                               const QString &clientSha256, const QString &stagingPath,
                               qint64 confirmedOffset, int ttlSec);
    bool updateUploadOffset(qint64 sessionId, qint64 confirmedOffset);
    // 完成/失败：写 status（done|failed|expired|verifying）
    bool finishUploadSession(qint64 sessionId, const QString &status);

    explicit OperateDB(QObject *parent = nullptr);
    OperateDB(const OperateDB&)=delete;
    OperateDB& operator=(const OperateDB&)=delete;
signals:

};

#endif // OPERATEDB_H
