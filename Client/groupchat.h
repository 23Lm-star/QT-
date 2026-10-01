#ifndef GROUPCHAT_H
#define GROUPCHAT_H

#include "protocol.h"
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

// REQ-EXT-GROUP-002：单群聊天窗口（可靠文本消息）。
// 发送 88 → 89 已持久化确认；接收 90 在线事件（与拉取按 seq 去重）；
// 打开窗口经 95/96 定位未读游标后按 91/92 前向补齐，上翻历史走后向翻页；
// 已读经 93/94 单调推进。Group-001 的群事件（84/85）处理不变。
class GroupChat : public QWidget
{
    Q_OBJECT
public:
    GroupChat(quint64 groupId, const QString &groupName, QWidget *parent = nullptr);
    quint64 groupId() const { return m_groupId; }
    // 89/92/94 响应路由（按信封 group_id 匹配）
    void handleResponse(PDU *pdu);
    // 90 在线消息事件
    void handleMessageEvent(PDU *pdu);
    // 96 未读摘要里本群的游标（打开窗口定位补齐起点/刷新徽标）
    void applyUnreadSummary(quint64 lastSeq, quint64 lastReadSeq, quint64 unread);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void sendCurrent();
    void loadOlder();
    void onItemDoubleClicked(QListWidgetItem *item);

private:
    void sendRequest(uint type, const QByteArray &body = QByteArray());
    QByteArray envelope(const QByteArray &body);
    void requestPull(quint8 mode, quint64 cursor, quint16 limit);
    void confirmRead();
    void updateStatus();
    QListWidgetItem *insertMessage(quint64 seq, const QString &sender, quint8 msgType,
                                   quint64 createdAtMs, const QString &content);
    void markRendered(quint64 seq);

    quint64 m_groupId;
    QString m_groupName;
    QListWidget *m_messageList;
    QLineEdit *m_input;
    QPushButton *m_sendButton;
    QPushButton *m_historyButton;
    QLabel *m_statusLabel;
    quint64 m_requestId = 0;
    bool m_initialized = false;
    bool m_summaryRequested = false;
    // 渲染游标：seq 全局唯一，90 与 92 同源去重
    QHash<quint64, bool> m_renderedSeqs;
    quint64 m_firstSeq = 0;        // 已渲染最小 seq（上翻游标）
    quint64 m_lastSeq = 0;         // 已渲染最大 seq
    quint64 m_groupLastSeq = 0;    // 群当前水位（92 回带）
    quint64 m_unread = 0;          // 服务端计算的未读数（92/96 回带）
    quint64 m_confirmedSeq = 0;    // 服务端确认的已读游标（94 回带）
    int m_catchupBatches = 0;      // 打开窗口前向补齐批次计数（上限防长历史全量拉）
    static const int MAX_CATCHUP_BATCHES = 20;
    struct PendingSend { QString clientMsgId; QString content; QListWidgetItem *item; };
    QList<PendingSend> m_pending;
    // 列表项数据角色
    static const int ROLE_SEQ = Qt::UserRole;          // quint64 seq
    static const int ROLE_STATE = Qt::UserRole + 1;    // 0 发送中 / 1 已发送 / 2 发送失败 / 3 系统
    static const int ROLE_CLIENT_KEY = Qt::UserRole + 2;
};

#endif // GROUPCHAT_H
