#include "group.h"

#include "client.h"

#include <QInputDialog>
#include <QDateTime>
#include <QLabel>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QShowEvent>
#include <QVariant>
#include <QVBoxLayout>
#include <cstring>

Group::Group(QWidget *parent)
    : QWidget(parent),
      m_createButton(new QPushButton(QStringLiteral("建群"), this)),
      m_refreshButton(new QPushButton(QStringLiteral("刷新群聊"), this)),
      m_invitationButton(new QPushButton(QStringLiteral("待处理邀请"), this)),
      m_groupList(new QListWidget(this)),
      m_invitationList(new QListWidget(this)),
      m_targetEdit(new QLineEdit(this)),
      m_roleBox(new QComboBox(this)),
      m_statusLabel(new QLabel(this)),
      m_requestId(0),
      m_loaded(false)
{
    auto *layout = new QVBoxLayout(this);
    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(m_createButton);
    toolbar->addWidget(m_refreshButton);
    toolbar->addWidget(m_invitationButton);
    layout->addLayout(toolbar);
    auto *actionBar = new QHBoxLayout;
    const QList<QPair<QString, void (Group::*)()>> actions = {
        {QStringLiteral("邀请"), &Group::inviteMember},
        {QStringLiteral("接受邀请"), &Group::acceptInvitation},
        {QStringLiteral("拒绝邀请"), &Group::rejectInvitation},
        {QStringLiteral("成员列表"), &Group::listMembers},
        {QStringLiteral("退出"), &Group::leaveGroup},
        {QStringLiteral("移除"), &Group::removeMember},
        {QStringLiteral("设角色"), &Group::setRole},
        {QStringLiteral("转让群主"), &Group::transferOwner},
        {QStringLiteral("解散"), &Group::dissolveGroup}
    };
    for (const auto &action : actions) {
        auto *button = new QPushButton(action.first, this);
        connect(button, &QPushButton::clicked, this, action.second);
        actionBar->addWidget(button);
    }
    m_targetEdit->setPlaceholderText(QStringLiteral("目标用户"));
    m_roleBox->addItem(QStringLiteral("member"));
    m_roleBox->addItem(QStringLiteral("admin"));
    actionBar->addWidget(m_targetEdit);
    actionBar->addWidget(m_roleBox);
    layout->addLayout(actionBar);
    layout->addWidget(m_statusLabel);
    layout->addWidget(m_groupList);
    layout->addWidget(m_invitationList);
    connect(m_createButton, &QPushButton::clicked, this, &Group::createGroup);
    connect(m_refreshButton, &QPushButton::clicked, this, &Group::refreshGroups);
    connect(m_invitationButton, &QPushButton::clicked, this, &Group::refreshInvitations);
    // REQ-EXT-GROUP-002：双击群条目打开群聊窗口
    connect(m_groupList, &QListWidget::itemDoubleClicked, this, &Group::openSelectedChat);
    m_statusLabel->setText(QStringLiteral("群聊按需加载"));
    m_invitationList->setMaximumHeight(100);
}

quint64 Group::selectedGroupId() const
{
    const QListWidgetItem *item = m_groupList->currentItem();
    return item == nullptr ? 0 : item->data(Qt::UserRole).toULongLong();
}

quint64 Group::selectedInvitationId() const
{
    const QListWidgetItem *item = m_invitationList->currentItem();
    return item == nullptr ? 0 : item->data(Qt::UserRole).toULongLong();
}

void Group::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_loaded) {
        m_loaded = true;
        refreshGroups();
        refreshInvitations();
    }
}

QByteArray Group::envelope(quint64 groupId, const QByteArray &body)
{
    QByteArray payload;
    groupAppendU16(payload, GROUP_PROTOCOL_VERSION);
    groupAppendU16(payload, GROUP_ENVELOPE_LENGTH);
    groupAppendU64(payload, ++m_requestId);
    groupAppendU64(payload, groupId);
    payload.append(body);
    return payload;
}

void Group::sendRequest(uint type, quint64 groupId, const QByteArray &body)
{
    const QByteArray payload = envelope(groupId, body);
    PDU *pdu = mkPDU(uint(payload.size()));
    pdu->uiType = type;
    memcpy(pdu->caMsg, payload.constData(), size_t(payload.size()));
    Client::getinstance().seedMsg(pdu);
}

void Group::createGroup()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("建群"), QStringLiteral("群名称"),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    QByteArray body;
    groupAppendString(body, name.trimmed().toUtf8());
    sendRequest(ENUM_MSG_TYPE_CREATE_GROUP_REQUEST, 0, body);
}

void Group::refreshGroups()
{
    sendRequest(ENUM_MSG_TYPE_LIST_GROUPS_REQUEST);
    requestUnreadSummary();
}

// REQ-EXT-GROUP-002：95 无业务字段，按 actor 汇总全部 active 群；96 回带未读徽标
void Group::requestUnreadSummary()
{
    sendRequest(ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_REQUEST);
}

void Group::refreshInvitations()
{
    sendRequest(ENUM_MSG_TYPE_QUERY_GROUP_INVITATIONS_REQUEST);
}

void Group::inviteMember()
{
    const quint64 groupId = selectedGroupId();
    if (groupId == 0 || m_targetEdit->text().trimmed().isEmpty()) return;
    QByteArray body;
    groupAppendString(body, m_targetEdit->text().trimmed().toUtf8());
    groupAppendString(body, QStringLiteral("client-%1").arg(QDateTime::currentMSecsSinceEpoch()).toUtf8());
    sendRequest(ENUM_MSG_TYPE_INVITE_GROUP_MEMBER_REQUEST, groupId, body);
}

void Group::acceptInvitation()
{
    const quint64 invitationId = selectedInvitationId();
    if (invitationId == 0) return;
    QByteArray body;
    groupAppendU64(body, invitationId);
    sendRequest(ENUM_MSG_TYPE_ACCEPT_GROUP_INVITATION_REQUEST, 0, body);
}

void Group::rejectInvitation()
{
    const quint64 invitationId = selectedInvitationId();
    if (invitationId == 0) return;
    QByteArray body;
    groupAppendU64(body, invitationId);
    sendRequest(ENUM_MSG_TYPE_REJECT_GROUP_INVITATION_REQUEST, 0, body);
}

void Group::listMembers()
{
    const quint64 groupId = selectedGroupId();
    if (groupId != 0) sendRequest(ENUM_MSG_TYPE_LIST_GROUP_MEMBERS_REQUEST, groupId);
}

void Group::leaveGroup()
{
    const quint64 groupId = selectedGroupId();
    if (groupId != 0) sendRequest(ENUM_MSG_TYPE_LEAVE_GROUP_REQUEST, groupId);
}

void Group::removeMember()
{
    const quint64 groupId = selectedGroupId();
    if (groupId == 0 || m_targetEdit->text().trimmed().isEmpty()) return;
    QByteArray body;
    groupAppendString(body, m_targetEdit->text().trimmed().toUtf8());
    sendRequest(ENUM_MSG_TYPE_REMOVE_GROUP_MEMBER_REQUEST, groupId, body);
}

void Group::setRole()
{
    const quint64 groupId = selectedGroupId();
    if (groupId == 0 || m_targetEdit->text().trimmed().isEmpty()) return;
    QByteArray body;
    groupAppendString(body, m_targetEdit->text().trimmed().toUtf8());
    groupAppendString(body, m_roleBox->currentText().toUtf8());
    sendRequest(ENUM_MSG_TYPE_SET_GROUP_ROLE_REQUEST, groupId, body);
}

void Group::transferOwner()
{
    const quint64 groupId = selectedGroupId();
    if (groupId == 0 || m_targetEdit->text().trimmed().isEmpty()) return;
    QByteArray body;
    groupAppendString(body, m_targetEdit->text().trimmed().toUtf8());
    sendRequest(ENUM_MSG_TYPE_TRANSFER_GROUP_OWNER_REQUEST, groupId, body);
}

void Group::dissolveGroup()
{
    const quint64 groupId = selectedGroupId();
    if (groupId != 0) sendRequest(ENUM_MSG_TYPE_DISSOLVE_GROUP_REQUEST, groupId);
}

void Group::handleResponse(PDU *pdu)
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
    if (code != 0) {
        m_statusLabel->setText(QStringLiteral("群操作失败：%1").arg(qint32(code)));
        return;
    }
    if (pdu->uiType == ENUM_MSG_TYPE_LIST_GROUPS_RESPOND) {
        quint32 count = 0;
        if (!groupReadU32(payload, offset, count)) return;
        m_groupList->clear();
        for (quint32 i = 0; i < count; ++i) {
            quint64 id = 0, groupVersion = 0;
            QByteArray name, status, role;
            if (!groupReadU64(payload, offset, id) || !groupReadString(payload, offset, name)
                    || !groupReadString(payload, offset, status) || !groupReadString(payload, offset, role)
                    || !groupReadU64(payload, offset, groupVersion)) return;
            auto *item = new QListWidgetItem(QStringLiteral("%1  [%2] %3 (%4)")
                                             .arg(QString::fromUtf8(name)).arg(QString::fromUtf8(role))
                                             .arg(QString::fromUtf8(status)).arg(id), m_groupList);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(id));
            item->setData(Qt::UserRole + 1, QString::fromUtf8(name));   // 群名 → 聊天窗口标题
            item->setData(Qt::UserRole + 2, item->text());              // 基础文案（徽标在其上追加）
            refreshUnreadBadge(id, m_unreadByGroup.value(id, 0));
        }
        m_statusLabel->setText(QStringLiteral("我的群聊：%1 个").arg(count));
    } else if (pdu->uiType == ENUM_MSG_TYPE_QUERY_GROUP_INVITATIONS_RESPOND) {
        quint32 count = 0;
        if (!groupReadU32(payload, offset, count)) return;
        m_invitationList->clear();
        for (quint32 i = 0; i < count; ++i) {
            quint64 invitationId = 0, invitationGroupId = 0, expiresAt = 0;
            QByteArray groupName, inviter, status;
            if (!groupReadU64(payload, offset, invitationId) || !groupReadU64(payload, offset, invitationGroupId)
                    || !groupReadString(payload, offset, groupName) || !groupReadString(payload, offset, inviter)
                    || !groupReadString(payload, offset, status) || !groupReadU64(payload, offset, expiresAt)) return;
            Q_UNUSED(invitationGroupId);
            Q_UNUSED(expiresAt);
            auto *item = new QListWidgetItem(QStringLiteral("%1 邀请人：%2 (%3)")
                                             .arg(QString::fromUtf8(groupName)).arg(QString::fromUtf8(inviter))
                                             .arg(QString::fromUtf8(status)), m_invitationList);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(invitationId));
        }
        m_statusLabel->setText(QStringLiteral("待处理邀请：%1 个").arg(count));
    } else if (pdu->uiType == ENUM_MSG_TYPE_LIST_GROUP_MEMBERS_RESPOND) {
        quint32 count = 0;
        if (!groupReadU32(payload, offset, count)) return;
        QStringList members;
        for (quint32 i = 0; i < count; ++i) {
            QByteArray user, role, status;
            if (!groupReadString(payload, offset, user) || !groupReadString(payload, offset, role)
                    || !groupReadString(payload, offset, status)) return;
            members << QStringLiteral("%1 [%2] %3").arg(QString::fromUtf8(user))
                       .arg(QString::fromUtf8(role)).arg(QString::fromUtf8(status));
        }
        QMessageBox::information(this, QStringLiteral("群成员"), members.join(QStringLiteral("\n")));
    } else if (pdu->uiType == ENUM_MSG_TYPE_CREATE_GROUP_RESPOND) {
        m_statusLabel->setText(QStringLiteral("建群成功"));
        refreshGroups();
    } else if (pdu->uiType == ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_RESPOND) {
        // REQ-EXT-GROUP-002：96 = count + N×{group_id,last_seq,last_read_seq,unread}
        quint32 count = 0;
        if (!groupReadU32(payload, offset, count)) return;
        quint64 totalUnread = 0;
        for (quint32 i = 0; i < count; ++i) {
            quint64 summaryGroupId = 0, lastSeq = 0, lastReadSeq = 0, unread = 0;
            if (!groupReadU64(payload, offset, summaryGroupId) || !groupReadU64(payload, offset, lastSeq)
                    || !groupReadU64(payload, offset, lastReadSeq) || !groupReadU64(payload, offset, unread)) return;
            totalUnread += unread;
            m_unreadByGroup.insert(summaryGroupId, unread);
            refreshUnreadBadge(summaryGroupId, unread);
            // 已打开窗口：同步游标与未读数
            if (m_chatWindows.contains(summaryGroupId)) {
                m_chatWindows.value(summaryGroupId)->applyUnreadSummary(lastSeq, lastReadSeq, unread);
            }
        }
        m_statusLabel->setText(QStringLiteral("我的群聊：%1 个 · 未读合计 %2")
                               .arg(m_groupList->count()).arg(totalUnread));
    }
}

// REQ-EXT-GROUP-002：群条目文案追加未读徽标（0 不显示）
void Group::refreshUnreadBadge(quint64 groupId, quint64 unread)
{
    for (int i = 0; i < m_groupList->count(); ++i) {
        QListWidgetItem *item = m_groupList->item(i);
        if (item->data(Qt::UserRole).toULongLong() != groupId) continue;
        QString text = item->data(Qt::UserRole + 2).toString();
        if (text.isEmpty()) text = item->text();
        if (unread > 0) text += QStringLiteral(" · 未读 %1").arg(unread);
        item->setText(text);
        return;
    }
}

void Group::openSelectedChat()
{
    const QListWidgetItem *item = m_groupList->currentItem();
    if (item == nullptr) return;
    const quint64 groupId = item->data(Qt::UserRole).toULongLong();
    if (groupId == 0) return;
    const QString groupName = item->data(Qt::UserRole + 1).toString();
    GroupChat *chat = m_chatWindows.value(groupId, nullptr);
    if (chat == nullptr) {
        chat = new GroupChat(groupId, groupName, nullptr);
        m_chatWindows.insert(groupId, chat);
    }
    chat->show();
    chat->raise();
    chat->activateWindow();
}

// 信封首 20B：版本(2)+长度(2)+request_id(8)+group_id(8)
quint64 Group::envelopeGroupId(PDU *pdu)
{
    if (pdu == nullptr || pdu->uiMsgLen < GROUP_ENVELOPE_LENGTH) return 0;
    const QByteArray payload(pdu->caMsg, GROUP_ENVELOPE_LENGTH);
    int offset = 0;
    quint16 version = 0, length = 0;
    quint64 requestId = 0, groupId = 0;
    if (!groupReadU16(payload, offset, version) || !groupReadU16(payload, offset, length)
            || !groupReadU64(payload, offset, requestId) || !groupReadU64(payload, offset, groupId)) return 0;
    Q_UNUSED(version);
    Q_UNUSED(length);
    Q_UNUSED(requestId);
    return groupId;
}

void Group::handleMessageResponse(PDU *pdu)
{
    const quint64 groupId = envelopeGroupId(pdu);
    if (groupId == 0) return;
    if (m_chatWindows.contains(groupId)) {
        m_chatWindows.value(groupId)->handleResponse(pdu);
        return;
    }
    m_statusLabel->setText(QStringLiteral("群 %1 的消息响应到达（窗口未打开）").arg(groupId));
}

void Group::handleMessageEvent(PDU *pdu)
{
    const quint64 groupId = envelopeGroupId(pdu);
    if (groupId == 0) return;
    GroupChat *chat = m_chatWindows.value(groupId, nullptr);
    if (chat != nullptr) {
        chat->handleMessageEvent(pdu);
        return;
    }
    // 未打开窗口：不渲染，只刷新未读徽标并提示（离线补偿拉取兜底）
    requestUnreadSummary();
    m_statusLabel->setText(QStringLiteral("群 %1 有新消息").arg(groupId));
}
