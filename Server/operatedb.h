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

    explicit OperateDB(QObject *parent = nullptr);
    OperateDB(const OperateDB&)=delete;
    OperateDB& operator=(const OperateDB&)=delete;
signals:

};

#endif // OPERATEDB_H
