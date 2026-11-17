#include "logging.h"
#include "chat.h"
#include "client.h"
#include "protocol.h"
#include "chatbounds.h"
#include "ui_chat.h"

#include <QMessageBox>
#include <QTextCursor>

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
    // REQ-CLI-MSGBND-001：正文上限 2000 字节（UTF-8），与服务端权威校验同值。
    // 超限提示并阻止发送、不改写用户内容（截断只用于接收端的防御性钳制），输入框保持原样便于修改。
    const QByteArray bodyBytes = msg.toUtf8();
    if ((uint)bodyBytes.size() > CHAT_MAX_BODY_BYTES) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("消息过长：%1 字节，上限 %2 字节，请缩减内容后重发。")
                .arg(bodyBytes.size()).arg(int(CHAT_MAX_BODY_BYTES)));
        return;
    }
    //清空消息发送框
    ui->output_LE->clear();
    // 总长 = Msg 定长头 + 正文（含结尾 '\0'），用作 mkPDU 的柔性数组长度
    uint Msglen=sizeof(Msg)+bodyBytes.size()+1;
    PDU* pdu=mkPDU(Msglen);//构建一个Msg大小还有一个消息的柔性数组大小
    pdu->uiType=ENUM_MSG_TYPE_CHAT_REQUEST;
    //现将发送人和接收人的用户名记录下来
    // 两个 32 字节定长槽位，未用满部分由 mkPDU 的零初始化保证
    // （REQ-AUDIT-FULL-001：按实际长度拷贝，防对短源串越界读）
    const QByteArray selfBytes=Client::getinstance().m_LoginName.toUtf8();
    memcpy(pdu->caData,selfBytes.constData(),size_t(qMin(selfBytes.size(),31)));
    const QByteArray peerBytes=m_strChatName.toUtf8();
    memcpy(pdu->caData+32,peerBytes.constData(),size_t(qMin(peerBytes.size(),31)));
    //构造一条消息（消息本体+发送时间）
    Msg* pdu_msg=(Msg*)pdu->caMsg;
    pdu_msg->totalLen=Msglen;//消息总长度（+消息本体长度+1）
    pdu_msg->msgLen=bodyBytes.size()+1;
    qCDebug(lcCli)<<"pdu_msg->totalLen"<<pdu_msg->totalLen<<" pdu_msg->msgLen"<<pdu_msg->msgLen<<" ";
    // Time 为 30 字节定长字段，存放格式化的本地时间字符串
    // REQ-CLI-MSGSTORE-001：时间串只取一次——16 应答要按「对方+时间+正文」匹配待发条目，
    // 这里再取一次会跨秒，导致自发消息永远认不出来、永远不落库
    // REQ-CLI-MSGBND-001：按实际内容拷贝，不足 30 字节的余量由 mkPDU 的清零补齐；
    // 改前固定拷 30 字节会越出短于 30 字节的源缓冲读。
    const QByteArray timeBytes = currtime.toUtf8();
    memcpy(pdu_msg->Time, timeBytes.constData(), size_t(qMin(timeBytes.size(), 30)));
    memcpy(pdu_msg->msg,bodyBytes.constData(),pdu_msg->msgLen);
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

// ===== REQ-PAGE-FETCH-001：服务端历史分页 =====
// 发 117：caData[0,32)=自己、[32,64)=对方；caMsg 头部=游标(qint64)+limit(u32)
void Chat::requestServerHistory(qint64 cursorId)
{
    const QString peer = m_historyPeer.isEmpty() ? m_strChatName : m_historyPeer;
    if (peer.isEmpty()) return;
    if (cursorId == 0) {                       // 最新一页：重置分页状态
        m_serverOldestId = 0;
        m_serverHasMore = false;
        ui->loadEarlier_PB->setHidden(true);
    }
    m_lastHistoryCursor = cursorId;
    PDU* pdu = mkPDU(uint(CHAT_HIST_REQ_LEN));
    pdu->uiType = ENUM_MSG_TYPE_CHAT_HISTORY_REQUEST;
    const QByteArray self = Client::getinstance().m_LoginName.toUtf8();
    const QByteArray peerB = peer.toUtf8();
    memcpy(pdu->caData, self.constData(), size_t(qMin(self.size(), 31)));
    memcpy(pdu->caData + 32, peerB.constData(), size_t(qMin(peerB.size(), 31)));
    memcpy(pdu->caMsg + CHAT_HIST_REQ_CURSOR_OFFSET, &cursorId, sizeof(cursorId));
    const uint limit = 50;
    memcpy(pdu->caMsg + CHAT_HIST_REQ_LIMIT_OFFSET, &limit, sizeof(limit));
    Client::getinstance().seedMsg(pdu);
}

// 收 118：本页去重后渲染/落库；最新页追加显示，更早页整块插顶
void Chat::applyServerHistory(const QString &peer, qint64 oldestId, bool hasMore, uint total,
                              const QVector<ServerHistRow> &rows)
{
    if (peer.isEmpty()) return;
    // 118 不回显双方名，归属全靠本地 bookkeeping：请求发出后用户已切到别的会话时，
    // 迟到的旧页直接丢弃，防止渲染进当前会话窗口（串会话）
    if (peer != m_strChatName) return;
    // rows 为 id 降序（新→旧）；组装成时间正序的可见行
    QStringList lines;
    const bool latestPage = (m_lastHistoryCursor == 0);
    if (latestPage) {
        // 最新页整页替换：以服务端页为准重置显示去重缓存；沿用旧键会把本地库已知的行
        // 从替换后的视图里剔除，权威页显示不全（wcy 清屏/lilei 只剩 1 条，2026-11-14 实测）。
        m_shownKeys.clear();
    }
    for (int i = rows.size() - 1; i >= 0; --i) {
        const ServerHistRow &r = rows.at(i);
        const quint8 dir = r.fromSelf ? 1 : 0;
        const QString key = dedupKey(r.timeText, r.content, dir);
        if (!latestPage && m_shownKeys.contains(key)) continue;   // 更早页插顶仍按旧键去重防重复行
        m_shownKeys.insert(key);
        const QString who = r.fromSelf ? QStringLiteral("我") : peer;
        lines.append(QStringLiteral("%1:%2  发送时间:%3").arg(who, r.content, r.timeText));
        persist(peer, r.content, r.timeText, dir);
    }
    if (latestPage) {
        // 最新页以服务端为准确整页替换：先渲染本地库再收 118 时，旧键会把服务端整页
        // 全部去重吞掉，窗口只剩本地库内容（wcy 空白/lilei 乱序，2026-11-14 GUI 实测）。
        // 服务端是权威数据源，最新页必然覆盖本地缓存显示（含空页清屏）。
        ui->show_TE->setPlainText(lines.join(QLatin1Char('\n')));
    } else if (!lines.isEmpty()) {
        prependHistoryBlock(lines.join(QLatin1Char('\n')));
    }
    m_serverOldestId = oldestId;
    m_serverHasMore = hasMore;
    ui->loadEarlier_PB->setHidden(!hasMore);
    if (!hasMore && m_lastHistoryCursor != 0) {
        ui->loadEarlier_PB->setToolTip(QStringLiteral("已到最早（共 %1 条）").arg(total));
    }
}

// 更早历史整块插到显示区顶部
void Chat::prependHistoryBlock(const QString &block)
{
    QTextCursor cur = ui->show_TE->textCursor();
    cur.movePosition(QTextCursor::Start);
    cur.insertText(block + QStringLiteral("\n"));
}

// 加载更早消息：按本页最旧 ID 继续向前翻页
void Chat::on_loadEarlier_PB_clicked()
{
    if (!m_serverHasMore || m_serverOldestId <= 0) return;
    requestServerHistory(m_serverOldestId);
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
