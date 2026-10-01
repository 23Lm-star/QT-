#include "groupchat.h"

#include "client.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QUuid>
#include <QVBoxLayout>
#include <cstring>

GroupChat::GroupChat(quint64 groupId, const QString &groupName, QWidget *parent)
    : QWidget(parent, Qt::Window),
      m_groupId(groupId),
      m_groupName(groupName),
      m_messageList(new QListWidget(this)),
      m_input(new QLineEdit(this)),
      m_sendButton(new QPushButton(QStringLiteral("发送"), this)),
      m_historyButton(new QPushButton(QStringLiteral("加载更早"), this)),
      m_statusLabel(new QLabel(this))
{
    setWindowTitle(QStringLiteral("群聊 - %1").arg(groupName));
    resize(560, 480);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_messageList);
    auto *row = new QHBoxLayout;
    m_input->setPlaceholderText(QStringLiteral("输入消息（<=2000 字节），回车发送"));
    row->addWidget(m_input);
    row->addWidget(m_sendButton);
    row->addWidget(m_historyButton);
    layout->addLayout(row);
    layout->addWidget(m_statusLabel);
    connect(m_sendButton, &QPushButton::clicked, this, &GroupChat::sendCurrent);
    connect(m_input, &QLineEdit::returnPressed, this, &GroupChat::sendCurrent);
    connect(m_historyButton, &QPushButton::clicked, this, &GroupChat::loadOlder);
    connect(m_messageList, &QListWidget::itemDoubleClicked, this, &GroupChat::onItemDoubleClicked);
    updateStatus();
}

void GroupChat::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_summaryRequested) {
        m_summaryRequested = true;
        // 95 无业务字段；96 返回后由 Group 转发本群游标 → 定位补齐起点
        sendRequest(ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_REQUEST);
    } else {
        confirmRead();
    }
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
    QByteArray body;
    body.append(char(mode));
    groupAppendU64(body, cursor);
    groupAppendU16(body, limit);
    sendRequest(ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_REQUEST, body);
}

void GroupChat::confirmRead()
{
    if (m_lastSeq == 0 || m_lastSeq <= m_confirmedSeq || !isVisible()) return;
    QByteArray body;
    groupAppendU64(body, m_lastSeq);
    sendRequest(ENUM_MSG_TYPE_GROUP_READ_CONFIRM_REQUEST, body);
}

void GroupChat::markRendered(quint64 seq)
{
    if (seq == 0) return;
    m_renderedSeqs.insert(seq, true);
    if (m_firstSeq == 0 || seq < m_firstSeq) m_firstSeq = seq;
    if (seq > m_lastSeq) m_lastSeq = seq;
}

QListWidgetItem *GroupChat::insertMessage(quint64 seq, const QString &sender, quint8 msgType,
                                          quint64 createdAtMs, const QString &content)
{
    if (seq != 0 && m_renderedSeqs.contains(seq)) return nullptr;
    const QString time = QDateTime::fromMSecsSinceEpoch(qint64(createdAtMs)).toString(QStringLiteral("HH:mm:ss"));
    QListWidgetItem *item = new QListWidgetItem;
    if (msgType == 1) {
        item->setText(QStringLiteral("—— %1 ——").arg(content));
        item->setForeground(QColor(120, 120, 120));
        item->setTextAlignment(Qt::AlignCenter);
        item->setData(ROLE_STATE, 3);
    } else {
        const QString who = sender == Client::getinstance().m_LoginName
                ? QStringLiteral("我") : sender;
        item->setText(QStringLiteral("[%1] %2: %3").arg(time).arg(who).arg(content));
        item->setData(ROLE_STATE, 1);
    }
    item->setData(ROLE_SEQ, QVariant::fromValue<qulonglong>(seq));
    m_messageList->addItem(item);
    markRendered(seq);
    return item;
}

void GroupChat::sendCurrent()
{
    const QString content = m_input->text();
    if (content.isEmpty()) return;
    if (content.toUtf8().size() > 2000) {
        m_statusLabel->setText(QStringLiteral("内容超过 2000 字节，未发送"));
        return;
    }
    const QString key = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QListWidgetItem *item = new QListWidgetItem(
                QStringLiteral("我: %1（发送中…）").arg(content));
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
    if (item == nullptr || item->data(ROLE_STATE).toInt() != 2) return;
    const QString key = item->data(ROLE_CLIENT_KEY).toString();
    for (const PendingSend &pending : m_pending) {
        if (pending.clientMsgId != key) continue;
        item->setText(QStringLiteral("我: %1（发送中…重试）").arg(pending.content));
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
    quint64 cursor = m_groupLastSeq;
    if (m_firstSeq > 0) cursor = m_firstSeq - 1;
    if (cursor == 0) return;
    requestPull(1, cursor, 50);
}

void GroupChat::updateStatus()
{
    m_statusLabel->setText(QStringLiteral("群水位 %1 · 服务端已读 %2 · 本地未读 %3")
                           .arg(m_groupLastSeq).arg(m_confirmedSeq).arg(m_unread));
}

void GroupChat::applyUnreadSummary(quint64 lastSeq, quint64 lastReadSeq, quint64 unread)
{
    m_groupLastSeq = lastSeq;
    m_unread = unread;
    if (!m_initialized) {
        // 打开窗口：从前向补齐未读开始（游标=服务端 last_read_seq），补到水位即可上翻
        m_initialized = true;
        requestPull(0, lastReadSeq, 100);
    }
    updateStatus();
}

void GroupChat::handleMessageEvent(PDU *pdu)
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
                pending.item->setText(QStringLiteral("我: %1（已送达）").arg(text));
                pending.item->setData(ROLE_STATE, 1);
                updateStatus();
                return;
            }
        }
    }
    QListWidgetItem *item = insertMessage(seq, senderName, msgType, createdAtMs, text);
    if (item != nullptr) {
        m_messageList->scrollToBottom();
        updateStatus();
    }
    confirmRead();
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
        QByteArray key;
        quint64 seq = 0, msgId = 0, createdAtMs = 0;
        const bool hasKey = groupReadString(payload, offset, key);
        if (code != 0) {
            // 失败状态：服务端在可解析到幂等键时回带，供同键重试
            if (hasKey) {
                for (const PendingSend &pending : m_pending) {
                    if (pending.clientMsgId == QString::fromUtf8(key) && pending.item != nullptr) {
                        pending.item->setText(QStringLiteral("我: %1（发送失败，双击重试）")
                                              .arg(pending.content));
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
                pending.item->setText(QStringLiteral("我: %1（已发送）").arg(pending.content));
                pending.item->setData(ROLE_STATE, 1);
                markRendered(seq);
            } else if (state == 1) {
                pending.item->setText(QStringLiteral("我: %1（已发送）").arg(pending.content));
            }
            break;
        }
        updateStatus();
        return;
    }

    if (pdu->uiType == ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_RESPOND) {
        if (code != 0) {
            m_statusLabel->setText(QStringLiteral("拉取失败：%1").arg(qint32(code)));
            return;
        }
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
        m_groupLastSeq = groupLastSeq;
        m_unread = unread;
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
                    item->setForeground(QColor(120, 120, 120));
                    item->setTextAlignment(Qt::AlignCenter);
                } else {
                    const QString who = QString::fromUtf8(record.sender)
                            == Client::getinstance().m_LoginName ? QStringLiteral("我")
                                                                 : QString::fromUtf8(record.sender);
                    item->setText(QStringLiteral("[%1] %2: %3").arg(time).arg(who)
                                  .arg(QString::fromUtf8(record.content)));
                }
                item->setData(ROLE_SEQ, QVariant::fromValue<qulonglong>(record.seq));
                item->setData(ROLE_STATE, 1);
                markRendered(record.seq);
                m_messageList->insertItem(0, item);
            }
        } else {
            for (const Record &record : records) {
                insertMessage(record.seq, QString::fromUtf8(record.sender), record.msgType,
                              record.createdAtMs, QString::fromUtf8(record.content));
            }
            m_messageList->scrollToBottom();
            if (hasMore && m_catchupBatches < MAX_CATCHUP_BATCHES) {
                ++m_catchupBatches;
                requestPull(0, nextCursor, 100);
            } else {
                confirmRead();
            }
        }
        updateStatus();
        return;
    }

    if (pdu->uiType == ENUM_MSG_TYPE_GROUP_READ_CONFIRM_RESPOND) {
        quint64 effective = 0;
        if (code == 0 && groupReadU64(payload, offset, effective) && effective > m_confirmedSeq) {
            m_confirmedSeq = effective;
        }
        updateStatus();
        return;
    }
}
