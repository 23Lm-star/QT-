#include "group.h"

#include "client.h"

#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QShowEvent>
#include <QVBoxLayout>
#include <cstring>

Group::Group(QWidget *parent)
    : QWidget(parent),
      m_createButton(new QPushButton(QStringLiteral("建群"), this)),
      m_refreshButton(new QPushButton(QStringLiteral("刷新群聊"), this)),
      m_invitationButton(new QPushButton(QStringLiteral("待处理邀请"), this)),
      m_groupList(new QListWidget(this)),
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
    layout->addWidget(m_statusLabel);
    layout->addWidget(m_groupList);
    connect(m_createButton, &QPushButton::clicked, this, &Group::createGroup);
    connect(m_refreshButton, &QPushButton::clicked, this, &Group::refreshGroups);
    connect(m_invitationButton, &QPushButton::clicked, this, &Group::refreshInvitations);
    m_statusLabel->setText(QStringLiteral("群聊按需加载"));
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
            m_groupList->addItem(QStringLiteral("%1  [%2] %3 (%4)")
                                 .arg(QString::fromUtf8(name)).arg(QString::fromUtf8(role))
                                 .arg(QString::fromUtf8(status)).arg(id));
        }
        m_statusLabel->setText(QStringLiteral("我的群聊：%1 个").arg(count));
    } else if (pdu->uiType == ENUM_MSG_TYPE_QUERY_GROUP_INVITATIONS_RESPOND) {
        quint32 count = 0;
        if (!groupReadU32(payload, offset, count)) return;
        m_statusLabel->setText(QStringLiteral("待处理邀请：%1 个").arg(count));
    } else if (pdu->uiType == ENUM_MSG_TYPE_CREATE_GROUP_RESPOND) {
        m_statusLabel->setText(QStringLiteral("建群成功"));
        refreshGroups();
    }
}
