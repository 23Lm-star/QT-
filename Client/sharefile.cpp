// 好友分享弹窗实现：接收人列表按「名字.状态」格式，展示时只保留名字。
#include "sharefile.h"
#include "ui_sharefile.h"
#include "index.h"
ShareFile::ShareFile(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ShareFile)
{
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    ui->setupUi(this);
}

ShareFile::~ShareFile()
{
    delete ui;
}

// 好友项文本形如 "用户名.在线状态"，这里截取第一个点之后的用户名部分
void ShareFile::flusfriend()
{
    ui->friendlist_LW->clear();
    QListWidget* listWidget =Index::getinstance().getFriend()->getfile_LW();
    for(int i=0;i<listWidget->count();i++){
        QString userName=listWidget->item(i)->text();
        int indexi=userName.indexOf('.');
        userName=userName.right(userName.size()-indexi-1);
        QListWidgetItem* listWidgeti=new QListWidgetItem(userName);
        ui->friendlist_LW->addItem(listWidgeti);
    }
}

// 全选：逐项 setSelected，不改变当前项
void ShareFile::on_allchoose_PB_clicked()
{
    for(int i=0;i<ui->friendlist_LW->count();i++){
        ui->friendlist_LW->item(i)->setSelected(true);
    }
}

void ShareFile::on_nochoose_PB_clicked()
{
    for(int i=0;i<ui->friendlist_LW->count();i++){
        ui->friendlist_LW->item(i)->setSelected(false);
    }
}

// 请求负载：caData = 发送人(32B) + 接收人数(4B)；caMsg = 每个接收人占 32B，末尾接文件完整路径
void ShareFile::on_output_PB_clicked()
{
    QString shareFileName=Index::getinstance().getFile()->m_strShareFileName;
    //获取文件完整路径
    QString tarPath=QString("%1/%2").arg(Index::getinstance().getFile()->m_strCurPath).arg(shareFileName);
    //发送人
    QString seedName=Client::getinstance().m_LoginName;
    QList<QListWidgetItem*> listitems= ui->friendlist_LW->selectedItems();
    int tarUserLen=listitems.size();
    PDU* pdu=mkPDU(tarUserLen*32+tarPath.toStdString().size()+1);
    pdu->uiType=ENUM_MSG_TYPE_SHAREFILE_REQUEST;
    //发送人
    memcpy(pdu->caData,seedName.toStdString().c_str(),seedName.toStdString().size());
    //接收人个数
    memcpy(pdu->caData+32,&tarUserLen,sizeof (tarUserLen));
    //接收人
    for(int i=0;i<tarUserLen;i++){
        QString revName=listitems[i]->text();
        memcpy(pdu->caMsg+32*i,revName.toStdString().c_str(),revName.toStdString().size());
    }
    //分享文件路径
    memcpy(pdu->caMsg+32*tarUserLen,tarPath.toStdString().c_str(),tarPath.toStdString().size());
    Client::getinstance().seedMsg(pdu);
}
