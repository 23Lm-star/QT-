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
    }
}
