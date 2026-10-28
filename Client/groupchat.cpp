#include "groupchat.h"

#include "client.h"

#include <QDateTime>
#include <QMessageBox>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <cstring>

// 群聊窗口实现：发送为乐观渲染 + 89/90 确认；接收为 90 实时事件，
// 断线/丢事件留下的 seq 洞通过 91/92 前向补拉修复；历史翻页走 mode=1。
// 已读游标由 GroupChatState 统一维护，本文件只在窗口可见时推进并节流上报。
namespace {
// REQ-UX-001 A3：消息统一排版为「身份/时间行 + 正文行」，长文本由 QListWidget 换行显示；
// 发送状态只出现在身份行，不再混进正文。
QString messageHeader(const QString &who, const QString &time, const QString &state)
{
    if (state.isEmpty()) {
        return QStringLiteral("%1  %2").arg(who, time);
    }
    return QStringLiteral("%1  %2  ·  %3").arg(who, time, state);
}
}

GroupChat::GroupChat(quint64 groupId, const QString &groupName, QWidget *parent)
    : QWidget(parent, Qt::Window),
      m_groupId(groupId),
      m_groupName(groupName),
      m_messageList(new QListWidget(this)),
      m_input(new QLineEdit(this)),
      m_sendButton(new QPushButton(QStringLiteral("发送"), this)),
      m_historyButton(new QPushButton(QStringLiteral("加载更早"), this)),
      m_clearButton(new QPushButton(QStringLiteral("清空本地记录"), this)),
      m_statusLabel(new QLabel(this))
{
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    // 独立顶层窗口（Qt::Window），由 Group 按 group_id 长期持有
    setWindowTitle(QStringLiteral("群聊 - %1").arg(groupName));
    resize(1120, 960);
    // REQ-UX-001 A3：长消息必须换行、不横向溢出、不省略
    m_messageList->setWordWrap(true);
    m_messageList->setTextElideMode(Qt::ElideNone);
    m_messageList->setSpacing(6);
    m_messageList->setUniformItemSizes(false);
    // 垂直布局：消息列表在上，输入行 + 状态栏在下
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_messageList);
    auto *row = new QHBoxLayout;
    m_input->setPlaceholderText(QStringLiteral("输入消息，回车发送"));
    row->addWidget(m_input);
    row->addWidget(m_sendButton);
    row->addStretch(1);
    layout->addLayout(row);
    // 「加载更早」与「清空本地记录」单独一行：四个控件挤在输入行里，
    // 高 DPI 缩放（QSS 字号 21px + padding 24px）会把按钮压到看不见。
    auto *toolRow = new QHBoxLayout;
    toolRow->addWidget(m_historyButton);
    toolRow->addWidget(m_clearButton);
    toolRow->addStretch(1);
    layout->addLayout(toolRow);
    layout->addWidget(m_statusLabel);
    connect(m_sendButton, &QPushButton::clicked, this, &GroupChat::sendCurrent);
    connect(m_input, &QLineEdit::returnPressed, this, &GroupChat::sendCurrent);
    connect(m_historyButton, &QPushButton::clicked, this, &GroupChat::loadOlder);
    connect(m_clearButton, &QPushButton::clicked, this, &GroupChat::clearLocalRecords);
    connect(m_messageList, &QListWidget::itemDoubleClicked, this, &GroupChat::onItemDoubleClicked);
    // 已读上报节流器：单次触发，窗口内的连续推进合并为一次网络请求
    m_readReportTimer = new QTimer(this);
    m_readReportTimer->setSingleShot(true);
    connect(m_readReportTimer, &QTimer::timeout, this, [this]() { flushReadReport(); });
    updateStatus();
}

void GroupChat::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 每次显示都是一次新的"观看会话"：补齐批次额度重置，按当前锚点补齐隐藏期间落下的消息
    m_catchupBatches = 0;
    // REQ-CLI-MSGSTORE-001：先灌本地历史，再按服务端锚点播种水位，最后才让补齐跑起来
    loadLocalHistory();
    ensureWaterSeeded();
    updateStatus();
    maybeCatchUp();
}

// 窗口不可见后游标不再推进，把节流窗口内最后一段推进立即落地（不进网络层排队的部分不丢）
void GroupChat::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_readReportTimer->stop();
    flushReadReport();
}

void GroupChat::onAnchorUpdated()
{
    ensureWaterSeeded();        // 锚点比首显晚到时同样要播种
    maybeCatchUp();
}

QByteArray GroupChat::envelope(const QByteArray &body)
{
    QByteArray payload;
    groupAppendU16(payload, GROUP_PROTOCOL_VERSION);
    groupAppendU16(payload, GROUP_ENVELOPE_LENGTH);
    groupAppendU64(payload, ++m_requestId);
    groupAppendU64(payload, m_groupId);
    payload.append(body);
    return payload;
}

void GroupChat::sendRequest(uint type, const QByteArray &body)
{
    const QByteArray payload = envelope(body);
    PDU *pdu = mkPDU(uint(payload.size()));
    pdu->uiType = type;
    memcpy(pdu->caMsg, payload.constData(), size_t(payload.size()));
    Client::getinstance().seedMsg(pdu);
}

void GroupChat::requestPull(quint8 mode, quint64 cursor, quint16 limit)
{
    // 正文：mode(1B) + cursor(8B) + limit(2B)
    QByteArray body;
    body.append(char(mode));
    groupAppendU64(body, cursor);
    groupAppendU16(body, limit);
    sendRequest(ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_REQUEST, body);
}

void GroupChat::markRendered(quint64 seq)
{
    if (seq == 0) return;
    m_renderedSeqs.insert(seq, true);
    if (m_firstSeq == 0 || seq < m_firstSeq) m_firstSeq = seq;
}

GroupChatState &GroupChat::groupState()
{
    return Client::getinstance().groupState(m_groupId);
}

// REQ-EXT-GROUP-004：唯一推进入口——90 实时事件、89/90 自己消息、92 拉取记录全走这里。
// 窗口不可见时只记录水位：消息照渲染，但我没看到，游标绝不动。
void GroupChat::onMessageArrived(quint64 seq)
{
    if (seq == 0) return;
    if (!isVisible()) {
        groupState().noteWater(seq);
        return;
    }
    switch (groupState().onArrived(seq)) {
    case ReadCursorVerdict::Buffered:
        // 有洞：上面那条已渲染给用户，但游标必须停在洞前；立刻前向拉取补洞
        maybeCatchUp();
        break;
    case ReadCursorVerdict::Advanced:
        scheduleReadReport(false);
        break;
    case ReadCursorVerdict::Ignored:
        break;
    }
    updateStatus();
}

// 需要且允许时发起前向补洞拉取：锚点未就绪先取摘要；洞已在拉或批次用尽则不重复拉
void GroupChat::maybeCatchUp()
{
    if (!isVisible()) return;
    if (!groupState().anchored()) {
        if (!m_summaryRequested) {
            m_summaryRequested = true;
            sendRequest(ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_REQUEST);
        }
        return;
    }
    if (m_catchupInFlight) return;
    if (groupState().readCursor() >= groupState().waterSeq()) {
        scheduleReadReport(false);
        return;
    }
    if (m_catchupBatches >= MAX_CATCHUP_BATCHES) return;
    ++m_catchupBatches;
    m_catchupInFlight = true;
    requestPull(0, groupState().readCursor(), 100);
}

// 上报节流：固定 400ms 速率的合并器——窗口内的连续推进只发一次，且发的是最新游标，
// 合并后仍是"前缀末尾"，语义不变。immediate 用于补齐批次结束与关闭前落地。
void GroupChat::scheduleReadReport(bool immediate)
{
    if (!isVisible()) return;
    if (!groupState().needsReport()) return;
    if (immediate) {
        m_readReportTimer->stop();
        flushReadReport();
        return;
    }
    if (m_readReportTimer->isActive()) return;
    m_readReportTimer->start(READ_REPORT_THROTTLE_MS);
}

void GroupChat::flushReadReport()
{
    if (!groupState().needsReport()) return;
    QByteArray body;
    groupAppendU64(body, groupState().readCursor());
    sendRequest(ENUM_MSG_TYPE_GROUP_READ_CONFIRM_REQUEST, body);
}

QListWidgetItem *GroupChat::insertMessage(quint64 seq, const QString &sender, quint8 msgType,
                                          quint64 createdAtMs, const QString &content)
{
    if (seq != 0 && m_renderedSeqs.contains(seq)) return nullptr;
    // 协议时间戳为毫秒；msgType=1 表示系统消息（居中灰色），其余为普通文本
    const QString time = QDateTime::fromMSecsSinceEpoch(qint64(createdAtMs)).toString(QStringLiteral("HH:mm:ss"));
    QListWidgetItem *item = new QListWidgetItem;
    if (msgType == 1) {
        item->setText(QStringLiteral("—— %1 ——").arg(content));
        item->setForeground(QColor(150, 170, 190));
        item->setTextAlignment(Qt::AlignCenter);
        item->setData(ROLE_STATE, 3);
    } else {
        const bool mine = (sender == Client::getinstance().m_LoginName);
        const QString who = mine ? QStringLiteral("我") : sender;
        item->setText(QStringLiteral("%1\n%2").arg(messageHeader(who, time, QString()), content));
        item->setTextAlignment(mine ? Qt::AlignRight : Qt::AlignLeft);
        item->setData(ROLE_STATE, 1);
    }
    item->setData(ROLE_SEQ, QVariant::fromValue<qulonglong>(seq));
    m_messageList->addItem(item);
    markRendered(seq);
    // REQ-CLI-MSGSTORE-001：渲染即暂存，接得上水位的当场落库，接不上的留在内存等补洞
    stageForStore(seq, sender, msgType, createdAtMs, content, 0);
    flushStorable();
    return item;
}

void GroupChat::renderOwnMessage(QListWidgetItem *item, const QString &state, const QString &content)
{
    if (item == nullptr) {
        return;
    }
    QString time = item->data(ROLE_TIME).toString();
    if (time.isEmpty()) {
        time = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    }
    item->setText(QStringLiteral("%1\n%2").arg(messageHeader(QStringLiteral("我"), time, state), content));
    item->setTextAlignment(Qt::AlignRight);
}

void GroupChat::sendCurrent()
{
    // 发送前本地校验：空消息不发；协议上限 2000 字节（UTF-8）
    const QString content = m_input->text();
    if (content.isEmpty()) return;
    if (content.toUtf8().size() > 2000) {
        m_statusLabel->setText(QStringLiteral("内容超过 2000 字节，未发送"));
        return;
    }
    // 幂等键：同一 key 重发时服务端可去重；条目先以“发送中”状态乐观入列表
    const QString key = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString time = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    QListWidgetItem *item = new QListWidgetItem(
                QStringLiteral("%1\n%2").arg(messageHeader(QStringLiteral("我"), time, QStringLiteral("发送中…")), content));
    item->setTextAlignment(Qt::AlignRight);
    item->setData(ROLE_TIME, time);
    item->setData(ROLE_STATE, 0);
    item->setData(ROLE_CLIENT_KEY, key);
    m_messageList->addItem(item);
    m_messageList->scrollToBottom();
    m_pending.append(PendingSend{key, content, item});
    QByteArray body;
    groupAppendString(body, key.toUtf8());
    groupAppendString(body, content.toUtf8());
    sendRequest(ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_REQUEST, body);
    m_input->clear();
}

void GroupChat::onItemDoubleClicked(QListWidgetItem *item)
{
    // 只有“发送失败”的条目可双击重试，复用原 clientMsgId
    if (item == nullptr || item->data(ROLE_STATE).toInt() != 2) return;
    const QString key = item->data(ROLE_CLIENT_KEY).toString();
    for (const PendingSend &pending : m_pending) {
        if (pending.clientMsgId != key) continue;
        renderOwnMessage(item, QStringLiteral("发送中…"), pending.content);
        item->setData(ROLE_STATE, 0);
        QByteArray body;
        groupAppendString(body, key.toUtf8());
        groupAppendString(body, pending.content.toUtf8());
        sendRequest(ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_REQUEST, body);
        return;
    }
}

void GroupChat::loadOlder()
{
    // 91 mode=1 语义为 seq<=cursor（含边界）：必须以「已渲染最小 seq-1」请求，
    // 否则最早一条会被重复回传（92 渲染侧亦有 seq 去重兜底）。
    quint64 cursor = groupState().waterSeq();
    if (m_firstSeq > 0) cursor = m_firstSeq - 1;
    if (cursor == 0) return;
    requestPull(1, cursor, 50);   // 上翻历史：seq 全在游标之下，不推进已读、不上报
}

void GroupChat::updateStatus()
{
    // REQ-UX-001 A3：不再向用户暴露"水位/已读游标"等协议术语
    // REQ-EXT-GROUP-004：未读 = 水位 - 连续前缀末尾，与服务器 COUNT 口径等价（序号稠密）
    m_statusLabel->setText(QStringLiteral("本群消息共 %1 条 · 未读 %2 条")
                           .arg(groupState().waterSeq()).arg(groupState().unread()));
}

void GroupChat::handleMessageEvent(PDU *pdu)
{
    // 90 事件体：信封 + code + {group_id, seq, msg_id, sender, msg_type, created_at, content}
    if (pdu == nullptr || pdu->uiMsgLen < GROUP_ENVELOPE_LENGTH + 4) return;
    const QByteArray payload(pdu->caMsg, int(pdu->uiMsgLen));
    int offset = 0;
    quint16 version = 0, length = 0;
    quint64 requestId = 0, groupId = 0;
    quint32 code = 0;
    if (!groupReadU16(payload, offset, version) || !groupReadU16(payload, offset, length)
            || !groupReadU64(payload, offset, requestId) || !groupReadU64(payload, offset, groupId)
            || !groupReadU32(payload, offset, code) || version != GROUP_PROTOCOL_VERSION
            || length != GROUP_ENVELOPE_LENGTH) return;
    Q_UNUSED(requestId);
    if (code != 0 || groupId != m_groupId) return;
    quint64 evtGroupId = 0, seq = 0, msgId = 0, createdAtMs = 0;
    QByteArray sender, content;
    if (!groupReadU64(payload, offset, evtGroupId) || !groupReadU64(payload, offset, seq)
            || !groupReadU64(payload, offset, msgId) || !groupReadString(payload, offset, sender)
            || offset + 1 > payload.size()) return;
    const quint8 msgType = quint8(payload.at(offset++));
    if (!groupReadU64(payload, offset, createdAtMs)
            || !groupReadString(payload, offset, content)) return;
    Q_UNUSED(evtGroupId);
    Q_UNUSED(msgId);
    const QString senderName = QString::fromUtf8(sender);
    const QString text = QString::fromUtf8(content);
    if (senderName == Client::getinstance().m_LoginName) {
        // 自己的消息：与本地乐观条目按内容+发送中状态合并，否则按 seq 去重后正常渲染
        for (int i = 0; i < m_pending.size(); ++i) {
            PendingSend &pending = m_pending[i];
            if (pending.content != text) continue;
            if (pending.item != nullptr && pending.item->data(ROLE_STATE).toInt() == 0) {
                markRendered(seq);
                pending.item->setData(ROLE_SEQ, QVariant::fromValue<qulonglong>(seq));
                renderOwnMessage(pending.item, QStringLiteral("已送达"), text);
                pending.item->setData(ROLE_STATE, 1);
                // REQ-CLI-MSGSTORE-001：90 先于 89 到达时，自己的消息同样按发送方向落库
                stageForStore(seq, senderName, msgType, createdAtMs, text, 1);
                flushStorable();
                onMessageArrived(seq);
                return;
            }
        }
    }
    QListWidgetItem *item = insertMessage(seq, senderName, msgType, createdAtMs, text);
    if (item != nullptr) m_messageList->scrollToBottom();
    // 渲染去重与游标推进是两件事：即使这条已渲染过，也要喂进推进入口
    onMessageArrived(seq);
}

void GroupChat::handleResponse(PDU *pdu)
{
    if (pdu == nullptr || pdu->uiMsgLen < GROUP_ENVELOPE_LENGTH + 4) return;
    const QByteArray payload(pdu->caMsg, int(pdu->uiMsgLen));
    int offset = 0;
    quint16 version = 0, length = 0;
    quint64 requestId = 0, groupId = 0;
    quint32 code = 0;
    if (!groupReadU16(payload, offset, version) || !groupReadU16(payload, offset, length)
            || !groupReadU64(payload, offset, requestId) || !groupReadU64(payload, offset, groupId)
            || !groupReadU32(payload, offset, code) || version != GROUP_PROTOCOL_VERSION
            || length != GROUP_ENVELOPE_LENGTH || groupId != m_groupId) return;
    Q_UNUSED(requestId);

    if (pdu->uiType == ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_RESPOND) {
        // 89 成功体：client_msg_id + seq + msg_id + created_at；失败时可能只回带 key
        QByteArray key;
        quint64 seq = 0, msgId = 0, createdAtMs = 0;
        const bool hasKey = groupReadString(payload, offset, key);
        if (code != 0) {
            // 失败状态：服务端在可解析到幂等键时回带，供同键重试
            if (hasKey) {
                for (const PendingSend &pending : m_pending) {
                    if (pending.clientMsgId == QString::fromUtf8(key) && pending.item != nullptr) {
                        renderOwnMessage(pending.item, QStringLiteral("发送失败（双击重试）"), pending.content);
                        pending.item->setData(ROLE_STATE, 2);
                        break;
                    }
                }
            }
            m_statusLabel->setText(QStringLiteral("发送失败：%1").arg(qint32(code)));
            return;
        }
        if (!hasKey || !groupReadU64(payload, offset, seq) || !groupReadU64(payload, offset, msgId)
                || !groupReadU64(payload, offset, createdAtMs)) return;
        Q_UNUSED(msgId);
        const QString keyText = QString::fromUtf8(key);
        for (const PendingSend &pending : m_pending) {
            if (pending.clientMsgId != keyText || pending.item == nullptr) continue;
            const int state = pending.item->data(ROLE_STATE).toInt();
            if (state == 0) {
                // 89 先于 90 到达：渲染并入已渲染集合；90 到达时按 seq 去重不再重复
                pending.item->setData(ROLE_SEQ, QVariant::fromValue<qulonglong>(seq));
                renderOwnMessage(pending.item, QStringLiteral("已发送"), pending.content);
                pending.item->setData(ROLE_STATE, 1);
                markRendered(seq);
                // REQ-CLI-MSGSTORE-001：89 即「服务端已持久化」，只有这里确认成功才按发送方向落库
                stageForStore(seq, Client::getinstance().m_LoginName, 0, createdAtMs,
                              pending.content, 1);
                flushStorable();
                onMessageArrived(seq);   // 自己的消息同样占用序号通道，缺的正是它接上前缀
            } else if (state == 1) {
                renderOwnMessage(pending.item, QStringLiteral("已发送"), pending.content);
            }
            break;
        }
        updateStatus();
        return;
    }

    if (pdu->uiType == ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_RESPOND) {
        if (code != 0) {
            // 拉取失败：必须解掉"在途"标志，否则本次会话再也补不了洞
            m_catchupInFlight = false;
            m_statusLabel->setText(QStringLiteral("拉取失败：%1").arg(qint32(code)));
            return;
        }
        // 92 响应体：mode(1B) + has_more(1B) + next_cursor + group_last_seq
        //              + unread + count + count×{seq,msg_id,sender,msg_type,created_at,content}
        if (offset + 3 > payload.size()) return;
        const quint8 mode = quint8(payload.at(offset++));
        const bool hasMore = quint8(payload.at(offset++)) != 0;
        quint64 nextCursor = 0, groupLastSeq = 0, unread = 0;
        quint32 count = 0;
        if (!groupReadU64(payload, offset, nextCursor) || !groupReadU64(payload, offset, groupLastSeq)
                || !groupReadU64(payload, offset, unread) || !groupReadU32(payload, offset, count)) return;
        struct Record { quint64 seq, msgId, createdAtMs; QByteArray sender, content; quint8 msgType; };
        QList<Record> records;
        for (quint32 i = 0; i < count; ++i) {
            Record record;
            if (!groupReadU64(payload, offset, record.seq) || !groupReadU64(payload, offset, record.msgId)
                    || !groupReadString(payload, offset, record.sender)
                    || offset + 1 > payload.size()) return;
            record.msgType = quint8(payload.at(offset++));
            if (!groupReadU64(payload, offset, record.createdAtMs)
                    || !groupReadString(payload, offset, record.content)) return;
            records.append(record);
        }
        // 拉取结果同样是水位证据
        // 水位取 max 收敛。回带的未读数只作交叉校验（测试 S4 钉住它与"水位-游标"等价），
        // 客户端显示一律由会话状态推导，不引入第二个真相源。
        groupState().noteWater(groupLastSeq);
        Q_UNUSED(unread);
        if (mode == 1) {
            // 后向翻页：响应按 seq 升序；逐条前插到列表头部（已渲染 seq 跳过兜底）
            for (int i = records.size() - 1; i >= 0; --i) {
                const Record &record = records.at(i);
                if (m_renderedSeqs.contains(record.seq)) continue;
                const QString time = QDateTime::fromMSecsSinceEpoch(qint64(record.createdAtMs))
                        .toString(QStringLiteral("HH:mm:ss"));
                QListWidgetItem *item = new QListWidgetItem;
                if (record.msgType == 1) {
                    item->setText(QStringLiteral("—— %1 ——").arg(QString::fromUtf8(record.content)));
                    item->setForeground(QColor(150, 170, 190));
                    item->setTextAlignment(Qt::AlignCenter);
                } else {
                    const bool mine = (QString::fromUtf8(record.sender)
                            == Client::getinstance().m_LoginName);
                    const QString who = mine ? QStringLiteral("我") : QString::fromUtf8(record.sender);
                    item->setText(QStringLiteral("%1\n%2")
                                  .arg(messageHeader(who, time, QString()), QString::fromUtf8(record.content)));
                    item->setTextAlignment(mine ? Qt::AlignRight : Qt::AlignLeft);
                }
                item->setData(ROLE_SEQ, QVariant::fromValue<qulonglong>(record.seq));
                item->setData(ROLE_STATE, 1);
                markRendered(record.seq);
                m_messageList->insertItem(0, item);
            }
        } else {
            // 前向补齐按 seq 升序返回，逐条喂回同一个推进入口：洞被补齐时游标自然一路推到边界。
            // 渲染去重（insertMessage 命中已渲染返回 nullptr）与游标推进是两件事——
            // 窗口隐藏期间已渲染过的消息，同样必须喂进推进函数。
            // 遍历期间 m_catchupInFlight 保持 true，本轮不会插入第二次拉取。
            for (const Record &record : records) {
                insertMessage(record.seq, QString::fromUtf8(record.sender), record.msgType,
                              record.createdAtMs, QString::fromUtf8(record.content));
                onMessageArrived(record.seq);
            }
            m_messageList->scrollToBottom();
            if (hasMore && m_catchupBatches < MAX_CATCHUP_BATCHES) {
                ++m_catchupBatches;
                requestPull(0, nextCursor, 100);
            } else {
                m_catchupInFlight = false;
                scheduleReadReport(true);   // 批次结束：立即上报，不参与节流
            }
        }
        updateStatus();
        return;
    }

    if (pdu->uiType == ENUM_MSG_TYPE_GROUP_READ_CONFIRM_RESPOND) {
        // 94 回认服务端钳制后的有效游标（不会回退），用于抑制重复上报
        quint64 effective = 0;
        if (code == 0 && groupReadU64(payload, offset, effective)) {
            groupState().markConfirmed(effective);
        }
        updateStatus();
        return;
    }
}

// ===================== REQ-CLI-MSGSTORE-001：本地存储接线 =====================

QString GroupChat::storeKey() const
{
    return QString::number(m_groupId);
}

void GroupChat::stageForStore(quint64 seq, const QString &sender, quint8 msgType,
                              quint64 createdAtMs, const QString &content, quint8 direction)
{
    if (seq == 0) return;
    if (msgType != 0) {
        // 系统提示、群文件等不入库的事件仍占着序号：占号让水位能跨过它继续前进
        m_storeStaging.reserve(seq);
        return;
    }
    StoredMessage msg;
    msg.seq = seq;
    msg.convKey = storeKey();
    msg.sender = sender;
    msg.direction = direction;
    msg.createdAtMs = qint64(createdAtMs);
    msg.content = content;
    m_storeStaging.insert(msg);
}

void GroupChat::flushStorable()
{
    MessageStore &store = Client::getinstance().msgStore();
    if (m_storeStaging.isEmpty()) return;
    // 先收上限：库不可用时下面不落库，暂存项也不能无界增长
    m_storeStaging.capTo(store.available(MsgStoreKind::Group)
                         ? store.keepLimit() : STAGING_CAP_WHEN_UNAVAILABLE);
    if (!store.available(MsgStoreKind::Group)) return;
    const QString key = storeKey();
    // 只取接得上水位的连续段（洞留在队列里），逐条落库
    const QList<MessageStaging::Staged> ready =
        m_storeStaging.takeContiguousAbove(store.waterSeq(MsgStoreKind::Group, key));
    for (const MessageStaging::Staged &item : ready) {
        bool advanced = false;
        if (!item.storable) {
            // 占号条目：只推水位，不写消息行
            advanced = store.advanceWater(MsgStoreKind::Group, key, item.msg.seq);
        } else {
            const MessageStore::AppendResult r =
                store.append(MsgStoreKind::Group, key, item.msg.seq, item.msg.sender,
                             item.msg.direction, item.msg.createdAtMs, item.msg.content);
            advanced = (r == MessageStore::AppendResult::Stored
                        || r == MessageStore::AppendResult::Duplicate);
        }
        if (!advanced) {
            // 写失败放回去，下次推进再试：静默丢消息会让本地库与界面长期不一致
            if (item.storable) m_storeStaging.insert(item.msg);
            else m_storeStaging.reserve(item.msg.seq);
            break;
        }
    }
}

void GroupChat::loadLocalHistory()
{
    if (m_localLoaded) return;
    m_localLoaded = true;
    MessageStore &store = Client::getinstance().msgStore();
    if (!store.available(MsgStoreKind::Group)) return;
    const QList<StoredMessage> rows =
        store.loadRecent(MsgStoreKind::Group, storeKey(), store.keepLimit());
    for (const StoredMessage &row : rows)
        insertMessage(row.seq, row.sender, 0, quint64(row.createdAtMs), row.content);
    m_storeStaging.clear();     // 已在库里的行不必再走暂存
    if (!rows.isEmpty()) m_messageList->scrollToBottom();
}

void GroupChat::ensureWaterSeeded()
{
    MessageStore &store = Client::getinstance().msgStore();
    if (!store.available(MsgStoreKind::Group) || !groupState().anchored()) return;
    const QString key = storeKey();
    if (store.waterSeq(MsgStoreKind::Group, key) != 0) return;   // 本机存过就不播种
    const quint64 anchor = groupState().readCursor();
    if (anchor > 0) store.seedWater(MsgStoreKind::Group, key, anchor);
}

void GroupChat::clearLocalRecords()
{
    MessageStore &store = Client::getinstance().msgStore();
    if (!store.available(MsgStoreKind::Group)) {
        m_statusLabel->setText(QStringLiteral("本机没有可清空的群聊记录"));
        return;
    }
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, QStringLiteral("清空本地记录"),
        QStringLiteral("只删除本机保存的该群聊天记录，不影响服务端与其它设备。\n"
                       "清空后点“加载更早”仍会从服务端取回更早的消息。\n确定清空？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    if (!store.clearConversation(MsgStoreKind::Group, storeKey())) {
        m_statusLabel->setText(QStringLiteral("清空失败：%1").arg(store.lastError()));
        return;
    }
    m_messageList->clear();
    m_renderedSeqs.clear();
    m_storeStaging.clear();
    m_firstSeq = 0;
    m_localLoaded = true;       // 清完不再回灌本地历史
    updateStatus();
}
