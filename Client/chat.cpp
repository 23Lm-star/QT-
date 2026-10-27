#include "logging.h"
#include "chat.h"
#include "client.h"
#include "protocol.h"
#include "ui_chat.h"

// 单聊发送端：收集输入框文本 + 当前时间，封成 Msg 柔性数组后交 Client 发送。
// 协议布局：PDU.caData[0,32) 发送人、[32,64) 接收人；caMsg 起为 Msg 头 + 变长正文。
// 历史消息与实时消息的展示均由外部调用 updateShow_TE(s) 注入，本文件不读网络。
//构造函数
Chat::Chat(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Chat){
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    ui->setupUi(this);
}
Chat::~Chat(){
    delete ui;
}
//往消息框中添加消息—————————————————————————————————————————————————————————————
//向消息框里添加一条消息
void Chat::updateShow_TE(QString Msg){
    // 单条原样追加；调用方（MsgHandler）已完成“发送人:正文 时间”的拼装
    ui->show_TE->append(Msg);
}
//向消息框里添加一系列消息
void Chat::updateShow_TEs(QStringList Msgs){
    // 约定为成对序列：[正文0, 时间0, 正文1, 时间1, ...]
    for(int i=0;i<Msgs.size();i+=2){
        ui->show_TE->append(m_strChatName+":"+Msgs[i]+"  发送时间:"+Msgs[i+1]);
    }
}
//发送聊天消息——————————————————————————————————————————————————————————————————
//发送聊天消息（一条）
void Chat::on_output_PB_clicked(){
    //获取发送消息，还有当前时间
    // 经 UTF-8 往返清洗，保证后续 memcpy 的字节序与协议一致
    QString msg=ui->output_LE->text().toUtf8().toStdString().c_str();
    QString currtime=(QDateTime::currentDateTime()).toString();
    //判断消息是否为空，为空 不发送
    if(msg.isEmpty()){
        return;
    }
    //清空消息发送框
    ui->output_LE->clear();
    // 总长 = Msg 定长头 + 正文（含结尾 '\0'），用作 mkPDU 的柔性数组长度
    uint Msglen=sizeof(Msg)+msg.toUtf8().toStdString().size()+1;
    PDU* pdu=mkPDU(Msglen);//构建一个Msg大小还有一个消息的柔性数组大小
    pdu->uiType=ENUM_MSG_TYPE_CHAT_REQUEST;
    //现将发送人和接收人的用户名记录下来
    // 两个 32 字节定长槽位，未用满部分由 mkPDU 的零初始化保证
    memcpy(pdu->caData,Client::getinstance().m_LoginName.toStdString().c_str(),32);
    memcpy(pdu->caData+32,m_strChatName.toStdString().c_str(),32);
    //构造一条消息（消息本体+发送时间）
    Msg* pdu_msg=(Msg*)pdu->caMsg;
    pdu_msg->totalLen=Msglen;//消息总长度（+消息本体长度+1）
    pdu_msg->msgLen=msg.toUtf8().toStdString().size()+1;
    qCDebug(lcCli)<<"pdu_msg->totalLen"<<pdu_msg->totalLen<<" pdu_msg->msgLen"<<pdu_msg->msgLen<<" ";
    // Time 为 30 字节定长字段，存放格式化的本地时间字符串
    memcpy(pdu_msg->Time,QDateTime::currentDateTime().toString().toStdString().c_str(),30);
    memcpy(pdu_msg->msg,msg.toUtf8().toStdString().c_str(),pdu_msg->msgLen);
    qCDebug(lcCli)<<"发送msg"<<pdu_msg->msg<<" time:"<<pdu_msg->Time<<" ";
    qCDebug(lcCli)<<"报文长度为:"<<pdu->uiTotalLen<<" msgLen:"<<pdu->uiMsgLen<<endl;
    // 交给 Client 的 TCP 通道异步写出；所有权随发送流程移交
    Client::getinstance().seedMsg(pdu);
}
