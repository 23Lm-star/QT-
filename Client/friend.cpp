#include "friend.h"
#include "protocol.h"
#include "ui_friend.h"
#include "client.h"
#include <QInputDialog>
#include <qdebug.h>
Friend::Friend(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Friend)
{
    ui->setupUi(this);
    m_onlineUser=new OnlineUser;
    flushfriend();
    m_chat=new Chat;
}

Friend::~Friend()
{
    delete ui;
    delete m_onlineUser;
    delete m_chat;
}

void Friend::flushfriend()
{
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_FLUSHFRIEND_REQUEST;
    Client::getinstance().seedMsg(pdu);
}

QListWidget *Friend::getfile_LW()
{
    return  ui->listWidget;
}

void Friend::flushFriend_LW(const QStringList& q)
{
    ui->listWidget->clear();
    ui->listWidget->addItems(q);
}

void Friend::on_findUser_PB_clicked()
{
    QString strname=QInputDialog::getText(this,"查找用户","用户名");
    qDebug()<<strname<<endl;
    if(strname==nullptr){
        return ;
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_findUser_REQUEST;
    memcpy(pdu->caData,strname.toStdString().c_str(),32);
    Client::getinstance().seedMsg(pdu);
}

void Friend::on_onlineUser_PB_clicked()
{
    if(m_onlineUser->isHidden()){
        m_onlineUser->show();
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_ONLINEUSER_REQUEST;
    Client::getinstance().seedMsg(pdu);
}

void Friend::on_flush_PB_clicked()
{
   flushfriend();
}

void Friend::on_del_PB_clicked()
{
    QListWidgetItem* tar =ui->listWidget->currentItem();
    if(!tar){
        return ;
    }
    QString tarname=tar->text();
    int cnt=0;
    for(int i=0;i<tarname.size();i++){
        if(tarname[i]=='.'){
            cnt=i;break;
        }
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_DELETEFRIEND_REQUEST;
    memcpy(pdu->caData,tarname.toStdString().c_str()+cnt+1,32);
    memcpy(pdu->caData+32,Client::getinstance().m_LoginName.toStdString().c_str(),32);
    Client::getinstance().seedMsg(pdu);
}

void Friend::on_chat_PB_clicked()
{
    QListWidgetItem* pitem=ui->listWidget->currentItem();//获取当前聊天的对象
    if(pitem==nullptr){//如果聊天对象为nullptr，那么就不能进行聊天
        return;
    }
    //取出聊天对象的名字（去除前面的序号）
    char pName[32]={'\0'};
    int cnt=0;
    for(int i=0;i<pitem->text().size();i++){
        if(pitem->text()[i]=='.'){
            cnt=i;
            break;
        }
    }
    memcpy(pName,pitem->text().toStdString().c_str()+cnt+1,32);
    m_chat->m_strChatName=pName;//记录聊天对象
    if(m_chat->isHidden()){
        m_chat->show();
        m_chat->setWindowTitle(m_chat->m_strChatName);
        PDU* pdu= mkPDU();
        pdu->uiType=ENUM_MSG_TYPE_GETCHATMSG_REQUEST;
        memcpy(pdu->caData,Client::getinstance().m_LoginName.toStdString().c_str(),32);
        memcpy(pdu->caData+32,m_chat->m_strChatName.toStdString().c_str(),32);
        Client::getinstance().seedMsg(pdu);
    }
}
