#ifndef OPERATEDB_H
#define OPERATEDB_H

#include <QObject>
#include <QDateTime>
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
    //启动检查：schema_version 达标返回 true；库不可达/表缺失/版本低返回 false 并打日志
    bool checkSchemaVersion();
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
    int handleLogin(char*strname,char*strpwd);
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
