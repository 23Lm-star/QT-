#ifndef GROUP_H
#define GROUP_H

#include "protocol.h"
#include <QListWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>

class QLabel;

// 群聊模块：进入页面后按需查询群列表和邀请，不承载 GROUP-002 消息正文。
class Group : public QWidget
{
    Q_OBJECT
public:
    explicit Group(QWidget *parent = nullptr);
    void handleResponse(PDU *pdu);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void createGroup();
    void refreshGroups();
    void refreshInvitations();
    void inviteMember();
    void acceptInvitation();
    void rejectInvitation();
    void listMembers();
    void leaveGroup();
    void removeMember();
    void setRole();
    void transferOwner();
    void dissolveGroup();

private:
    void sendRequest(uint type, quint64 groupId = 0, const QByteArray &body = QByteArray());
    QByteArray envelope(quint64 groupId, const QByteArray &body);
    quint64 selectedGroupId() const;
    quint64 selectedInvitationId() const;
    QPushButton *m_createButton;
    QPushButton *m_refreshButton;
    QPushButton *m_invitationButton;
    QListWidget *m_groupList;
    QListWidget *m_invitationList;
    QLineEdit *m_targetEdit;
    QComboBox *m_roleBox;
    QLabel *m_statusLabel;
    quint64 m_requestId;
    bool m_loaded;
};

#endif // GROUP_H
