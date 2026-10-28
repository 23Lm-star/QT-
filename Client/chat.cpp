#include "logging.h"
#include "chat.h"
#include "client.h"
#include "protocol.h"
#include "ui_chat.h"

#include <QMessageBox>

// 单聊发送端：收集输入框文本 + 当前时间，封成 Msg 柔性数组后交 Client 发送。
// 协议布局：PDU.caData[0,32) 发送人、[32,64) 接收人；caMsg 起为 Msg 头 + 变长正文。
// 消息展示由 receivePeerMessage / loadLocalHistory 注入，本文件不读网络。
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
    // REQ-CLI-MSGSTORE-001：时间串只取一次——16 应答要按「对方+时间+正文」匹配待发条目，
    // 这里再取一次会跨秒，导致自发消息永远认不出来、永远不落库
    memcpy(pdu_msg->Time,currtime.toStdString().c_str(),30);
    memcpy(pdu_msg->msg,msg.toUtf8().toStdString().c_str(),pdu_msg->msgLen);
    qCDebug(lcCli)<<"发送msg"<<pdu_msg->msg<<" time:"<<pdu_msg->Time<<" ";
    qCDebug(lcCli)<<"报文长度为:"<<pdu->uiTotalLen<<" msgLen:"<<pdu->uiMsgLen<<endl;
    // REQ-CLI-MSGSTORE-001：改前自发消息只在窗口里根本不显示（要等下次进会话才从
    // 服务端历史看到），本地存储上线后仍然看不到——这里补上乐观渲染。
    // 落库仍要等 16 确认，所以未发送成功的消息重启后不会出现，与"发送成功才存"一致。
    updateShow_TE(QStringLiteral("我:%1  发送时间:%2").arg(msg, currtime));
    // REQ-CLI-MSGSTORE-001：登记待发条目，服务端 16 回带入库成功后才落本地库
    m_pendingSelf.append(PendingSelf{m_strChatName, msg, currtime});
    if (m_pendingSelf.size() > 500) m_pendingSelf.removeFirst();
    // 交给 Client 的 TCP 通道异步写出；所有权随发送流程移交
    Client::getinstance().seedMsg(pdu);
}

// ===== REQ-CLI-MSGSTORE-001：私聊消息本地存储 =====
//
// 私聊没有服务端序号：本机会话内按接收顺序自增分配本地序号（水位=已落库的最长前缀），
// 只落「水位及之前」的消息。17/18 是消费式离线补发（服务端只回未送达给本人的行，
// 取后标记已送达），所以补发回来的方向恒为接收，且与实时投递不会重叠。

QString Chat::dedupKey(const QString &timeText, const QString &content, quint8 direction)
{
    // 方向进键：同一秒里双方各发一条相同文本时，不能被误判成重复
    return QString::number(int(direction)) + QLatin1Char(1) + timeText + QLatin1Char(1) + content;
}

void Chat::persist(const QString &peer, const QString &content, const QString &timeText,
                   quint8 direction)
{
    MessageStore &store = Client::getinstance().msgStore();
    if (!store.available(MsgStoreKind::Chat) || peer.isEmpty()) return;
    const quint64 seq = store.waterSeq(MsgStoreKind::Chat, peer) + 1;
    store.append(MsgStoreKind::Chat, peer, seq,
                 direction == 1 ? Client::getinstance().m_LoginName : peer,
                 direction, 0, content, dedupKey(timeText, content, direction), timeText);
}

void Chat::receivePeerMessage(const QString &peer, const QString &content, const QString &timeText)
{
    const QString key = dedupKey(timeText, content, 0);
    // 只有正显示这条会话时才渲染：离线补发晚到而用户已切好友时，落库照做、界面不串会话
    if (peer == m_strChatName && !m_shownKeys.contains(key)) {
        m_shownKeys.insert(key);
        updateShow_TE(QStringLiteral("%1:%2 发送时间:%3").arg(peer, content, timeText));
        if (isHidden()) show();                       // 沿用改前的"来消息即弹出窗口"行为
        if (windowTitle() != peer) setWindowTitle(peer);
    }
    // 去重由存储层的去重键兜底：认不出来的一条也不会重复入库
    persist(peer, content, timeText, 0);
    if (m_shownKeys.size() > 5000) m_shownKeys.clear();   // 只是显示去重缓存，丢顶多重复显示一行
}

void Chat::loadLocalHistory(const QString &peer)
{
    MessageStore &store = Client::getinstance().msgStore();
    if (!store.available(MsgStoreKind::Chat) || peer.isEmpty()) return;
    const QList<StoredMessage> rows = store.loadRecent(MsgStoreKind::Chat, peer, store.keepLimit());
    for (const StoredMessage &row : rows) {
        const QString key = dedupKey(row.timeText, row.content, row.direction);
        if (m_shownKeys.contains(key)) continue;
        m_shownKeys.insert(key);
        const QString who = row.direction == 1 ? QStringLiteral("我") : peer;
        ui->show_TE->append(QStringLiteral("%1:%2  发送时间:%3").arg(who, row.content, row.timeText));
    }
}

void Chat::beginHistoryRequest(const QString &peer)
{
    m_historyPeer = peer;
}

void Chat::handleSendAck(const QString &peer, const QString &content, const QString &timeText,
                         bool stored)
{
    for (int i = 0; i < m_pendingSelf.size(); ++i) {
        const PendingSelf &item = m_pendingSelf.at(i);
        if (item.peer != peer || item.content != content || item.time != timeText) continue;
        m_pendingSelf.removeAt(i);
        if (stored) persist(peer, content, timeText, 1);   // 入库失败或结果未回：不写本地
        return;
    }
}

void Chat::on_clear_PB_clicked()
{
    MessageStore &store = Client::getinstance().msgStore();
    if (m_strChatName.isEmpty()) return;
    if (!store.available(MsgStoreKind::Chat)) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("本机没有可清空的记录"));
        return;
    }
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, QStringLiteral("清空本地记录"),
        QStringLiteral("只删除本机保存的与 %1 的聊天记录，不影响服务端与其它设备。\n确定清空？").arg(m_strChatName),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    if (!store.clearConversation(MsgStoreKind::Chat, m_strChatName)) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("清空失败：%1").arg(store.lastError()));
        return;
    }
    // 该会话还在等 16 的自发条目一并作废：否则迟到的确认会把刚清空的消息写回来
    for (int i = m_pendingSelf.size() - 1; i >= 0; --i) {
        if (m_pendingSelf.at(i).peer == m_strChatName) m_pendingSelf.removeAt(i);
    }
    ui->show_TE->clear();
    m_shownKeys.clear();
}
