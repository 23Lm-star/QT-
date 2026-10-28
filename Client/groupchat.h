#ifndef GROUPCHAT_H
#define GROUPCHAT_H

#include "messagestore.h"
#include "protocol.h"
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

class QHideEvent;
class QTimer;
class GroupChatState;

// REQ-EXT-GROUP-002：单群聊天窗口（可靠文本消息）。
// 发送 88 → 89 已持久化确认；接收 90 在线事件（与拉取按 seq 去重）；
// 打开窗口经 95/96 定位未读游标后按 91/92 前向补齐，上翻历史走后向翻页；
// 已读经 93/94 单调推进。Group-001 的群事件（84/85）处理不变。
//
// REQ-EXT-GROUP-004：本窗口只是"会话级群读状态"的读写者，自己不再持有游标。
// 上报锚点恒为「已连续拿到的最长前缀的末尾」，绝不是已渲染的最大 seq——
// 实时投递会丢（服务端出站队列满即丢弃），报最大会让洞内消息被永久判为已读。
// 单群聊天窗口：消息列表 + 输入框 + 状态栏
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
    // 服务端摘要（96）落地后通知：按新锚点续补/上报，窗口未开或无新锚点时为空操作
    void onAnchorUpdated();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private slots:
    // 发送输入框内容（乐观入列表，失败可重试）
    void sendCurrent();
    // 加载更早消息（91 mode=1）
    void loadOlder();
    // 双击条目：仅对“发送失败”的消息重发
    void onItemDoubleClicked(QListWidgetItem *item);
    // REQ-CLI-MSGSTORE-001：清空本群在本机的消息存储（只删本地行，服务端历史不动）
    void clearLocalRecords();

private:
    // 统一请求出口（自动加 20B 信封）
    void sendRequest(uint type, const QByteArray &body = QByteArray());
    QByteArray envelope(const QByteArray &body);
    // 拉取消息：mode=0 前向补洞（seq>cursor 升序），mode=1 后向翻页（seq<=cursor）
    void requestPull(quint8 mode, quint64 cursor, quint16 limit);
    GroupChatState &groupState();                  // Client 持有的会话级群读状态
    void onMessageArrived(quint64 seq);      // 唯一推进入口（实时/自己消息/拉取共用）
    void maybeCatchUp();                     // 需要且允许时发起前向补洞拉取
    void scheduleReadReport(bool immediate); // 上报（节流合并 / 立即）
    void flushReadReport();
    void updateStatus();
    QListWidgetItem *insertMessage(quint64 seq, const QString &sender, quint8 msgType,
                                   quint64 createdAtMs, const QString &content);
    void markRendered(quint64 seq);
    // ===== REQ-CLI-MSGSTORE-001：本地存储接线 =====
    // 会话键：群 ID 的字符串形式（群库内区分会话用）
    QString storeKey() const;
    // 先按本地水位把已暂存且接得上的消息落库，越界的继续留在 m_storeStaging
    void flushStorable();
    // 渲染即暂存：文本消息（msgType==0）落库；系统事件等不可存类型只占号，
    // 否则水位后面永远缺一格，之后的文本消息会全被判成越界而停写
    void stageForStore(quint64 seq, const QString &sender, quint8 msgType,
                       quint64 createdAtMs, const QString &content, quint8 direction);
    // 首次显示时把本地已存历史灌进窗口（服务端增量再按 seq 去重）
    void loadLocalHistory();
    // 本地从未存过该群时，用服务端已读游标播种水位：历史不回填，锚点之后才落库
    void ensureWaterSeeded();
    // REQ-UX-001 A3：自己的消息统一"身份/时间行 + 正文行"右对齐重绘
    void renderOwnMessage(QListWidgetItem *item, const QString &state, const QString &content);

    // 本窗口所属群（信封里恒用此值）
    quint64 m_groupId;
    // 窗口标题显示用
    QString m_groupName;
    // 消息列表（每条含身份/时间行 + 正文行）
    QListWidget *m_messageList;
    // 输入框；回车等同点发送
    QLineEdit *m_input;
    QPushButton *m_sendButton;
    // “加载更早”：后向翻页
    QPushButton *m_historyButton;
    // REQ-CLI-MSGSTORE-001：清空本群本地存储
    QPushButton *m_clearButton;
    // 总条数/未读提示
    QLabel *m_statusLabel;
    // 信封请求号自增源
    quint64 m_requestId = 0;
    // 本次显示是否已请求过 96 摘要（防重复）
    bool m_summaryRequested = false;
    // 渲染去重：seq 全局唯一，90 与 92 同源
    QHash<quint64, bool> m_renderedSeqs;
    // REQ-CLI-MSGSTORE-001：已渲染但还没接上本地水位的消息（越界只留在内存，补洞后落库）
    MessageStaging m_storeStaging;
    // 本窗口是否已把本地历史灌进列表（只做一次，避免重复显示）
    bool m_localLoaded = false;
    quint64 m_firstSeq = 0;        // 已渲染最小 seq（上翻游标）
    QTimer *m_readReportTimer;     // 已读上报节流（固定 400ms 速率的合并器）
    bool m_catchupInFlight = false; // 补洞拉取在途（防拉风暴）
    int m_catchupBatches = 0;      // 前向补齐批次计数（上限防长历史全量拉）
    static const int MAX_CATCHUP_BATCHES = 20;
    // 本地库不可用时的暂存上限：降级期间也不能让越界消息无限堆在内存里
    static const int STAGING_CAP_WHEN_UNAVAILABLE = 500;
    static const int READ_REPORT_THROTTLE_MS = 400;
    // 乐观发送中的消息：clientMsgId 用于 89/90 回包配对，item 用于就地改状态
    struct PendingSend { QString clientMsgId; QString content; QListWidgetItem *item; };
    // 未确认发送（失败后可双击重试）
    QList<PendingSend> m_pending;
    // 列表项数据角色
    static const int ROLE_SEQ = Qt::UserRole;          // quint64 seq
    static const int ROLE_STATE = Qt::UserRole + 1;    // 0 发送中 / 1 已发送 / 2 发送失败 / 3 系统
    static const int ROLE_CLIENT_KEY = Qt::UserRole + 2;
    static const int ROLE_TIME = Qt::UserRole + 3;     // 本地乐观条目的时间戳显示
};

#endif // GROUPCHAT_H
