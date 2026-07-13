#include "server.h"
#include <QFile>
#include <QDebug>
#include <QMessageBox>
#include <QTcpServer>
#include "mytcpserver.h"
Server::Server(QWidget *parent)
    : QWidget(parent)
{
    //加载配置文件
    loadconfig();
    //监听对应端口号的信息（这里创建了myTcpServer的唯一实例）
    MyTcpServer::getinstance().listen(QHostAddress(m_strlp),m_usPort);
}

Server::~Server()
{
}

void Server::loadconfig()
{
    QFile file(":/connect.config");
    if(file.open(QIODevice::ReadOnly)){
        QString s=QString(file.readAll());
        QStringList lst=s.split("\r\n");
        //读取配置文件里面的Ip地址和端口号，还有文件路径
        this->m_strlp=lst[0];
        this->m_usPort=lst[1].toUShort();
        this->filesPath=lst[2];
        qDebug()<<"loadconfig m_strlp"<<m_strlp<<"m_usPort"<<m_usPort<<"filesPath"<<filesPath<<endl;
        file.close();
    }
    else {
        QMessageBox::critical(this,"提示","打开文件失败");
        qDebug()<<"打开文件失败"<<endl;
    }
}

Server &Server::getinstance()
{
    static Server instance;
    return instance;
}

