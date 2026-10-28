#ifndef CHAT_H
#define CHAT_H

#include <QSet>
#include <QWidget>

// 单聊窗口：展示与某个好友的会话消息，并负责发送聊天请求。
// 窗口自身不解析响应：Client/MsgHandler 收到 15/16/18 后调用 receivePeerMessage /
// handleSendAck 推入，本文件不读网络。
// m_strChatName 由 Friend 打开窗口时写入，既是窗口标题也是本条会话的对方用户名。
namespace Ui {
class Chat;
}

// 聊天页（纯展示 + 发送入口）
class Chat : public QWidget
{
    Q_OBJECT

public:
    QString m_strChatName;//聊天对象
    void updateShow_TE(QString Msg);//更新聊天框 - 一条信息
    // 构造：建立 Designer 控件，初始不加载任何会话
    explicit Chat(QWidget *parent = nullptr);
    ~Chat();

    // ===== REQ-CLI-MSGSTORE-001：私聊消息本地存储 =====
    // 收到的消息落库并渲染：15 实时投递与 18 离线补发共用，按「原文时间+正文」去重，
    // 同一条消息既被实时收到又被离线补发时只显示一次、只存一次。
    void receivePeerMessage(const QString &peer, const QString &content, const QString &timeText);
    // 打开会话时把本机已存历史灌进窗口（首次打开只灌一次）
    void loadLocalHistory(const QString &peer);
    // 发送方拿到服务端 16 应答：仅入库成功才把自发消息落库（不新增界面提示）
    void handleSendAck(const QString &peer, const QString &content, const QString &timeText, bool stored);
    // 发出 17 时记下本次请求归属的会话：18 的响应体里没有对方名，
    // 用户在响应回来前切了好友的话，落库就会记到别人名下（改前只影响显示，现在会持久化错数据）
    void beginHistoryRequest(const QString &peer);
    QString historyPeer() const { return m_historyPeer; }

private slots:
    void on_output_PB_clicked();//点击聊天消息发送按钮->发送消息
    // REQ-CLI-MSGSTORE-001：清空当前会话在本机的存储
    void on_clear_PB_clicked();

private:
    // 自发消息登记：等待服务端 16 回带入库结果
    struct PendingSelf { QString peer; QString content; QString time; };
    // 本地显示与落库的去重键：方向 + 原文时间 + 正文
    static QString dedupKey(const QString &timeText, const QString &content, quint8 direction);
    // 落库一条私聊消息（direction: 0 接收 / 1 发送）
    void persist(const QString &peer, const QString &content, const QString &timeText,
                 quint8 direction);
    // Qt Designer 生成的控件集合
    Ui::Chat *ui;
    // 本会话已显示过的消息键（原文时间+正文），用于本地历史与服务端补发去重
    QSet<QString> m_shownKeys;
    // 未确认的自发消息（收到 16 后按结果处置，超出上限丢最旧）
    QList<PendingSelf> m_pendingSelf;
    // 最近一次 17 请求归属的会话（响应按它落库）
    QString m_historyPeer;
};

#endif // CHAT_H
