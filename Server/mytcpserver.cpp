#include "mytcpserver.h"
#include "mytcpsocket.h"
#include "server.h"
#include "protocol.h"
#include "memalloc.h"
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
MyTcpServer::MyTcpServer(){
    // 读取僵尸连接阈值（zombie_timeout.cfg，单位秒；默认 90s = 3×30s 心跳周期）
    m_zombieTimeoutMs = 90000;
    const QString cfg = QCoreApplication::applicationDirPath() + "/zombie_timeout.cfg";
    QFile cf(cfg);
    if (cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        bool ok = false;
        const qint64 secs = cf.readAll().trimmed().toLongLong(&ok);
        cf.close();
        if (ok && secs > 0) {
            m_zombieTimeoutMs = secs * 1000;
            qDebug() << "[ZOMBIE] 阈值配置:" << secs << "s (" << m_zombieTimeoutMs << "ms)";
        } else {
            qDebug() << "[ZOMBIE] 阈值配置无效，使用默认 90s";
        }
    } else {
        qDebug() << "[ZOMBIE] 未找到 zombie_timeout.cfg，使用默认阈值 90s";
    }

    m_sweepTimer = new QTimer(this);
    connect(m_sweepTimer, &QTimer::timeout, this, &MyTcpServer::onSweepTimeout);
    m_sweepTimer->start(30000);   // 每 30s 扫描一次僵尸连接
}
//连接新客户端，并且创建新的tcpsocket用来连接新客户端
void MyTcpServer::incomingConnection(qintptr handle)
{
    qDebug()<<"新客户端连接"<<endl;
    MyTcpSocket * m_tcpSocket=new MyTcpSocket;
    m_tcpSocket->setSocketDescriptor(handle);
    {
        QMutexLocker locker(&m_tcpsocketlistMutex);
        m_tcpSocket->m_connectionId=++m_nextConnectionId;
        tcpsocketlist.append(m_tcpSocket);
        for(int i=0;i<tcpsocketlist.size();i++){
            qDebug()<<tcpsocketlist[i]<<endl;
        }
    }
    qDebug()<<"新客户端连接完成"<<endl;
}

void MyTcpServer::handleUploadWriteFinished(quint64 connectionId, qint64 offset,
                                            qint64 length, bool success)
{
    QMutexLocker locker(&m_tcpsocketlistMutex);
    for(MyTcpSocket* socket : tcpsocketlist){
        if(socket->m_connectionId == connectionId){
            socket->handleUploadWriteFinished(offset,length,success);
            return;
        }
    }
}

MyTcpServer &MyTcpServer::getinstance()
{
    static MyTcpServer instance;
    return instance;
}
//移除已经与客户端断连的socket对象
void MyTcpServer::removeSocket(MyTcpSocket *m)
{
    {
        QMutexLocker locker(&m_tcpsocketlistMutex);
        this->tcpsocketlist.removeOne(m);
        for(int i=0;i<tcpsocketlist.size();i++){
            qDebug()<<tcpsocketlist[i]<<endl;
        }
    }
    m->deleteLater();
    m=nullptr;
}

//发送指定的客户端发送添加好友的消息
void MyTcpServer::seedAddFriend(char *tarname, PDU *pdu)
{
    if(tarname==nullptr||pdu==nullptr){
        return;
    }
    QMutexLocker locker(&m_tcpsocketlistMutex);
    for(int i=0;i<tcpsocketlist.size();i++){
        if(strcmp(tcpsocketlist[i]->m_strLoginName.toStdString().c_str(),tarname)==0){
            tcpsocketlist[i]->write((char*)pdu,pdu->uiTotalLen);
            break;
        }
    }
}

//单次拥有型转发：写出后无条件 memfree，调用方不得再访问 PDU
void MyTcpServer::sendAddFriendAndRelease(char *tarname, PDU *pdu)
{
    if(pdu==nullptr){
        return;
    }
    {
        QMutexLocker locker(&m_tcpsocketlistMutex);
        for(int i=0;i<tcpsocketlist.size();i++){
            if(tarname!=nullptr && strcmp(tcpsocketlist[i]->m_strLoginName.toStdString().c_str(),tarname)==0){
                tcpsocketlist[i]->write((char*)pdu,pdu->uiTotalLen);
                break;
            }
        }
    }
    memfree(pdu);
}
//给客户端转发pdu消息
void MyTcpServer::reseed(char *tarname, PDU *pdu){
    if(tarname==nullptr||pdu==nullptr){
        return;
    }
    QMutexLocker locker(&m_tcpsocketlistMutex);
    for(int i=0;i<tcpsocketlist.size();i++){
        if(strcmp(tcpsocketlist[i]->m_strLoginName.toStdString().c_str(),tarname)==0){
            tcpsocketlist[i]->write((char*)pdu,pdu->uiTotalLen);
            break;
        }
    }
}

//僵尸连接扫描：静默超过 m_zombieTimeoutMs 的连接直接 abort，触发既有下线清理
void MyTcpServer::onSweepTimeout(){
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<MyTcpSocket*> stale;
    {
        QMutexLocker locker(&m_tcpsocketlistMutex);
        for (MyTcpSocket* s : tcpsocketlist) {
            if (now - s->m_lastActiveMs > m_zombieTimeoutMs) {
                stale.append(s);
            }
        }
    }
    // 退出锁后再 abort：避免 abort()→disconnected→clientOffLine→removeSocket 重入同一把锁导致死锁
    for (MyTcpSocket* s : stale) {
        qDebug() << "僵尸连接清理:" << s->m_strLoginName
                 << "静默" << (now - s->m_lastActiveMs) << "ms";
        s->abort();   // 立即关闭 → 触发 disconnected → clientOffLine() 走既有清理
    }
}
