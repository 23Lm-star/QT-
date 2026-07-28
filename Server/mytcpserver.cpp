#include "mytcpserver.h"
#include "mytcpsocket.h"
#include "server.h"
#include "protocol.h"
#include "memalloc.h"
MyTcpServer::MyTcpServer(){
}
//连接新客户端，并且创建新的tcpsocket用来连接新客户端
void MyTcpServer::incomingConnection(qintptr handle)
{
    qDebug()<<"新客户端连接"<<endl;
    MyTcpSocket * m_tcpSocket=new MyTcpSocket;
    m_tcpSocket->setSocketDescriptor(handle);
    {
        QMutexLocker locker(&m_tcpsocketlistMutex);
        tcpsocketlist.append(m_tcpSocket);
        for(int i=0;i<tcpsocketlist.size();i++){
            qDebug()<<tcpsocketlist[i]<<endl;
        }
    }
    qDebug()<<"新客户端连接完成"<<endl;
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
