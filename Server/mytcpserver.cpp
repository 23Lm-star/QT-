#include "mytcpserver.h"
#include "mytcpsocket.h"
#include "server.h"
#include "protocol.h"
#include "clienttask.h"
MyTcpServer::MyTcpServer(){
    m_threadPool.setMaxThreadCount(8);
}
//连接新客户端，并且创建新的tcpsocket用来连接新客户端
void MyTcpServer::incomingConnection(qintptr handle)
{
    qDebug()<<"新客户端连接"<<endl;
    MyTcpSocket * m_tcpSocket=new MyTcpSocket;
    m_tcpSocket->setSocketDescriptor(handle);
    tcpsocketlist.append(m_tcpSocket);
    for(int i=0;i<tcpsocketlist.size();i++){
        qDebug()<<tcpsocketlist[i]<<endl;
    }
    //新建一个客户端连接类对象并且让新建一个线程托管运行
    ClientTask*task=new ClientTask(m_tcpSocket);
    m_threadPool.start(task);
}

MyTcpServer &MyTcpServer::getinstance()
{
    static MyTcpServer instance;
    return instance;
}
//移除已经与客户端断连的socket对象
void MyTcpServer::removeSocket(MyTcpSocket *m)
{
    this->tcpsocketlist.removeOne(m);
    m->deleteLater();
    m=nullptr;
    for(int i=0;i<tcpsocketlist.size();i++){
        qDebug()<<tcpsocketlist[i]<<endl;
    }
}

//发送指定的客户端发送添加好友的消息
void MyTcpServer::seedAddFriend(char *tarname, PDU *pdu)
{
    if(tarname==nullptr||pdu==nullptr){
        return;
    }
    for(int i=0;i<tcpsocketlist.size();i++){
        if(strcmp(tcpsocketlist[i]->m_strLoginName.toStdString().c_str(),tarname)==0){
            tcpsocketlist[i]->write((char*)pdu,pdu->uiTotalLen);
            break;
        }
    }
}
//给客户端转发pdu消息
void MyTcpServer::reseed(char *tarname, PDU *pdu){
    if(tarname==nullptr||pdu==nullptr){
        return;
    }
    for(int i=0;i<tcpsocketlist.size();i++){
        if(strcmp(tcpsocketlist[i]->m_strLoginName.toStdString().c_str(),tarname)==0){
            tcpsocketlist[i]->write((char*)pdu,pdu->uiTotalLen);
            break;
        }
    }
}
