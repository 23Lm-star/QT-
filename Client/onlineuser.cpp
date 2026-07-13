#include "onlineuser.h"
#include "ui_onlineuser.h"

OnlineUser::OnlineUser(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OnlineUser)
{
    ui->setupUi(this);
}

OnlineUser::~OnlineUser()
{
    delete ui;
}

void OnlineUser::updateOnlineUser( const QStringList &userNames){
    //清理列表
    ui->listWidget->clear();
    //重新添加所有的在线用户
    ui->listWidget->addItems(userNames);
}
//在线用户 双击添加好友
void OnlineUser::on_listWidget_itemDoubleClicked(QListWidgetItem *item)
{
    QString seedName= item->text();
    int cnt=0;
    for(int i=0;i<seedName.size();i++){
        if(seedName[i]=='.'){
            cnt=i;break;
        }
    }
    QString myName=Client::getinstance().m_LoginName;
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_ADDFRIEND_REQUEST;
    memcpy(pdu->caData,myName.toStdString().c_str(),32);
    memcpy(pdu->caData+32,seedName.toStdString().c_str()+cnt+1,32);
    //显示发送数据包内容
    qDebug()<<"添加好友请求 已发送:"<<endl
            <<"pdu->uiTotalLen"<<pdu->uiTotalLen<<endl
            <<"pdu->uiMsgLen"<<pdu->uiMsgLen<<endl
            <<"pdu->uiType"<<pdu->uiType<<endl
            <<"pdu->caData"<<pdu->caData<<endl
            <<"pdu->caData+32"<<pdu->caData+32<<endl
            <<"pdu->caMsg"<<pdu->caMsg<<endl;
    Client::getinstance().sorket.write((char*)pdu,pdu->uiTotalLen);
    free(pdu);
    pdu=nullptr;
}
