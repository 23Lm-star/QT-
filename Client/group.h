#ifndef GROUP_H
#define GROUP_H

#include "protocol.h"
#include "groupchat.h"
#include "groupfile.h"
#include <QComboBox>
#include <QDialog>
#include <QHash>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

class QLabel;

// 群聊主页职责：群列表/邀请的查询与操作入口、群消息与群文件响应的路由中枢。
// 每个群一个独立子窗口（GroupChat/GroupFile），按信封里的 group_id 懒创建并长期缓存；
// 所有请求统一走「20 字节信封 + 业务正文」并经 Client::seedMsg 异步发出。
// 群聊模块：进入页面后按需查询群列表和邀请；REQ-EXT-GROUP-002 起承载群消息
// 窗口打开入口与未读徽标（消息正文在 GroupChat 窗口内处理）。
// 群操作页（列表 + 工具栏 + 按群路由）
class Group : public QWidget
{
    Q_OBJECT
public:
    explicit Group(QWidget *parent = nullptr);
    void handleResponse(PDU *pdu);
    // REQ-EXT-GROUP-002：89/92/94 响应路由到对应群窗口
    void handleMessageResponse(PDU *pdu);
    // REQ-EXT-GROUP-002：90 在线消息事件按 group_id 路由
    void handleMessageEvent(PDU *pdu);
    // REQ-EXT-GROUP-003：100/102/104 群文件响应按 group_id 路由到群文件窗口
    void handleFileResponse(PDU *pdu);
    // REQ-EXT-GROUP-004：会话建立后取回全部群的未读锚点（群水位 + 我的已读游标），
    // 一次性建立会话级群读状态，使状态就绪与窗口是否打开解耦。
    void initializeSession();

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    // 弹窗输入群名并发起建群
    void createGroup();
    // 拉取群列表与未读摘要
    void refreshGroups();
    // 拉取当前用户的待处理邀请
    void refreshInvitations();
    void showInvitationDialog();    // 待处理邀请弹窗（列表选中条目 → 接受/拒绝）
    // 邀请「目标用户名」加入选中群
    void inviteMember();
    // 接受邀请弹窗中选中的邀请
    void acceptInvitation();
    // 拒绝邀请弹窗中选中的邀请
    void rejectInvitation();
    // 弹窗展示选中群的成员及角色
    void listMembers();
    // 退出选中群（群主不可退出）
    void leaveGroup();
    // 将「目标用户名」移出选中群（仅普通成员）
    void removeMember();
    // 按下拉框把「目标用户名」设为 member/admin
    void setRole();
    // 把群主转给「目标用户名」
    void transferOwner();
    // 解散选中群（仅群主，不可恢复）
    void dissolveGroup();
    // 双击群条目：懒创建并显示该群聊天窗口
    void openSelectedChat();
    // 懒创建并显示选中群的群文件窗口
    void openSelectedFiles();

private:
    // 统一出口：构造信封并发送指定类型的群请求
    void sendRequest(uint type, quint64 groupId = 0, const QByteArray &body = QByteArray());
    // 组装 20B 信封（版本/长度/自增请求号/群号）+ 正文
    QByteArray envelope(quint64 groupId, const QByteArray &body);
    // 当前群列表选中项的 group_id（无选中返回 0）
    quint64 selectedGroupId() const;
    // 邀请弹窗中选中项的 invitation_id（无选中返回 0）
    quint64 selectedInvitationId() const;
    // 请求 96 未读摘要，用于刷新徽标与会话级读状态
    void requestUnreadSummary();
    // 在群条目文案后追加“· 未读 N”，0 则保持基础文案
    void refreshUnreadBadge(quint64 groupId, quint64 unread);
    void ensureInvitationDialog();          // 惰性创建邀请弹窗
    QString groupErrorText(quint32 code) const;   // 服务端错误码 → 可读中文
    // 信封首 20B 提取 group_id（不满足最小长度返回 0）
    static quint64 envelopeGroupId(PDU *pdu);
    // 建群
    QPushButton *m_createButton;
    // 重新拉取群列表 + 未读摘要
    QPushButton *m_refreshButton;
    QPushButton *m_fileButton;      // REQ-EXT-GROUP-003：打开选中群的群文件窗口
    // 打开待处理邀请弹窗
    QPushButton *m_invitationButton;
    // 群列表；条目 UserRole 存 group_id
    QListWidget *m_groupList;
    QListWidget *m_invitationList;  // 挂在邀请弹窗内（不再占主布局）
    // 邀请/移除/设角色/转让的目标用户名
    QLineEdit *m_targetEdit;
    // 设角色时选择 member/admin
    QComboBox *m_roleBox;
    // 操作结果与错误提示
    QLabel *m_statusLabel;
    QDialog *m_invitationDialog;    // 待处理邀请弹窗（惰性创建）
    // 信封请求号自增源
    quint64 m_requestId;
    // 首次显示后只自动刷新一次
    bool m_loaded;
    QHash<quint64, GroupChat*> m_chatWindows;   // group_id → 窗口（应用生命周期内保留）
    QHash<quint64, GroupFile*> m_fileWindows;   // group_id → 群文件窗口
    QHash<quint64, quint64> m_unreadByGroup;    // group_id → 未读数（96 回带）
};

#endif // GROUP_H
