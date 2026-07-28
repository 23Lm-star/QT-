#ifndef OPERATEDB_H
#define OPERATEDB_H

#include <QObject>
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
private:
    explicit OperateDB(QObject *parent = nullptr);
    OperateDB(const OperateDB&)=delete;
    OperateDB& operator=(const OperateDB&)=delete;
signals:

};

#endif // OPERATEDB_H
