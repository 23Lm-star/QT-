#ifndef MYTCPSERVER_H
#define MYTCPSERVER_H

#include <QObject>
#include <QTcpServer>
#include "mytcpsocket.h"
#include <QThreadPool>
//管理所有客户端连接的一个类
class MyTcpServer : public QTcpServer
{
    Q_OBJECT
public:
    QThreadPool m_threadPool;
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
    //信息转发
    void reseed(char*tarname,PDU*pdu);
private:
    MyTcpServer();
    //记录所有已经建立连接的 socket对象 链表
    QList<MyTcpSocket*>tcpsocketlist;
};

#endif // MYTCPSERVER_H
