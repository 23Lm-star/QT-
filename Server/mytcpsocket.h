#ifndef MYTCPSOCKET_H
#define MYTCPSOCKET_H
#include <QTcpSocket>
#include <QObject>
#include <protocol.h>
#include "msghandler.h"
//进行一个客户端连接的类
class MyTcpSocket : public QTcpSocket
{
    Q_OBJECT
public:
    MyTcpSocket();
    ~MyTcpSocket();
public:
    //用来处理客户端PDU请求 的对象 指针
    MsgHandler* m_MsgHandler;
    //登录的用户名
    QString m_strLoginName;
    //存储传输的pdu字节流
    int a;
    QByteArray m_dpulist;
    //接收用户请求的三个过程： (循环)接收--处理--响应
    PDU* handlMsg(PDU* pdu);
    void seedMsg(PDU* respdu);
public slots:
    void recvmsg();//接收信息引起的槽函数
    void clientOffLine();//客户端登录下线断开连接的槽函数

};

#endif // MYTCPSOCKET_H
