#ifndef MYTCPSERVER_H
#define MYTCPSERVER_H

#include <QObject>
#include <QTcpServer>
#include <QMutex>
#include "mytcpsocket.h"
//管理所有客户端连接的一个类
class MyTcpServer : public QTcpServer
{
    Q_OBJECT
public:
    //单例模式，一个实例管理所有的客户端连接
    static MyTcpServer& getinstance();
    MyTcpServer(const MyTcpServer&)=delete;
    MyTcpServer& operator=(const MyTcpServer&)=delete;
public:
    //进行信号监听
    void incomingConnection(qintptr handle) override;
    //移除已经和客户端断开连接的socket对象
    void removeSocket(MyTcpSocket* m);
    //添加好友
    void seedAddFriend(char* tarname,PDU*pdu);
    //单次拥有型转发（供 addfriend() 在线路径使用，写后 memfree）
    void sendAddFriendAndRelease(char* tarname, PDU* pdu);
    //信息转发
    void reseed(char*tarname,PDU*pdu);
public slots:
    void handleUploadWriteFinished(quint64 connectionId, qint64 offset,
                                   qint64 length, bool success);
private:
    MyTcpServer();
    //记录所有已经建立连接的 socket对象 链表
    QList<MyTcpSocket*>tcpsocketlist;
    QMutex m_tcpsocketlistMutex;
    quint64 m_nextConnectionId = 0;

    //僵尸连接检测：每隔一段时间扫描，静默超过阈值的连接直接 abort 清理
    qint64 m_zombieTimeoutMs;   // 阈值(ms)，由 zombie_timeout.cfg 读取秒数，默认 90000 (90s)
    QTimer* m_sweepTimer;
    void onSweepTimeout();
};

#endif // MYTCPSERVER_H
