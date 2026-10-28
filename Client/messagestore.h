#ifndef MESSAGESTORE_H
#define MESSAGESTORE_H

#include <QtGlobal>
#include <QHash>
#include <QList>
#include <QString>

// REQ-CLI-MSGSTORE-001：接收消息的本地存储层（纯逻辑：无界面、无网络、无 QObject）。
//
// 口径：本地只记录「水位及之前」的消息序号。水位 = 已连续落库的最长前缀末尾，
// 与内存里的群读游标同源（Client/groupchatstate.h），落库与水位必须同步推进：
// 越界（水位 + 1 之上）的消息只留在界面，等 91/92 补洞后按序晋升落库。
//
// 群消息与私聊消息分成两个库文件（各库内用 user 列区分账号），明文存储。
// 存储层任何失败都只让本会话降级为「仅内存展示」，不得影响消息收发。

// 会话类型，决定落在哪个库文件
enum class MsgStoreKind
{
    Group = 0,    // 群消息库 group_msg.db
    Chat  = 1     // 私聊消息库 chat_msg.db
};

// 一条已落库（或待渲染）的消息；direction 是客户端本地语义
struct StoredMessage
{
    quint64 seq = 0;           // 群=服务端 group_message.seq；私聊=客户端按接收顺序自增分配
    QString convKey;           // 会话键：群为群 ID，私聊为对方账号名
    QString sender;            // 发送人账号名
    quint8 direction = 0;      // 0=接收 1=发送（本地发出且确认成功）
    qint64 createdAtMs = 0;    // 消息创建时间（毫秒）
    QString content;           // 正文
    QString timeText;          // 私聊用的协议原文时间串；群侧为空（用 createdAtMs）
};

// 越界消息的内存暂存队列（REQ-CLI-MSGSTORE-001 核心规则的可测载体）：
// 界面照常渲染，但只有接得上本地水位的连续段才允许交给存储层落库。
// 洞长期补不上时靠 capTo 丢最旧，不允许无界增长。
class MessageStaging
{
public:
    // 一条暂存项：storable=false 表示这条只占号不落库（系统事件、群文件提示等）
    struct Staged { StoredMessage msg; bool storable = true; };

    // 入队；seq==0（乐观条目还没拿到服务端序号）或同 seq 已存在时返回 false
    bool insert(const StoredMessage& msg, bool storable = true);
    // 为一个「不入库但占着序号」的消息占位：水位必须能跨过它继续前进
    bool reserve(quint64 seq);
    // 取出 (water, water+n] 的连续前缀，按 seq 升序返回并从队列移除
    QList<Staged> takeContiguousAbove(quint64 water);
    int size() const;
    bool isEmpty() const;
    void clear();
    // 只保留最新的 maxEntries 条
    void capTo(int maxEntries);

private:
    QHash<quint64, Staged> m_items;
};

class MessageStore
{
public:
    // 一次落库请求的结果
    enum class AppendResult
    {
        Stored,                 // 已落库，水位同步前进
        Duplicate,              // 序号已被水位跨过（重复投递，或该会话已清空）→ 不落库
        RejectedBeyondWater,    // 越界：与水位之间有洞 → 只在内存，等补洞晋升
        Unavailable,            // 该库不可用（打不开或损坏）→ 静默降级
        Failed                  // 写入报错（含非法序号）
    };

    MessageStore();
    ~MessageStore();

    // 打开当前账号的两库；dirPath 为空时用 defaultDir()。两者都成功才返回 true，
    // 单个库损坏只让该 kind 不可用（available 为假），另一个库照常工作。
    bool open(const QString& user, const QString& dirPath = QString());
    void close();

    // 与传输快照同目录（QStandardPaths::AppLocalDataLocation）
    static QString defaultDir();
    static QString fileName(MsgStoreKind kind);

    bool available(MsgStoreKind kind) const;
    QString filePath(MsgStoreKind kind) const;
    QString user() const { return m_user; }

    // 每会话保留条数，默认 500；小于 1 时按 1 处理
    int keepLimit() const { return m_keepLimit; }
    void setKeepLimit(int n);

    // 该会话的本地水位（无记录时为 0）
    quint64 waterSeq(MsgStoreKind kind, const QString& convKey) const;

    // 本地全新而服务端锚点已靠前时，把水位播种到锚点：历史不回填，锚点之后的
    // 连续消息才落库（否则新消息永远算「越界」，一条也存不下）。锚点不高于现有
    // 水位时不做任何事并返回 false —— 水位只增不减。
    bool seedWater(MsgStoreKind kind, const QString& convKey, quint64 anchorSeq);

    // 单格推进水位而不写消息行：给「占了序号但不入库」的消息用（系统事件等）。
    // 只接受 seq == 水位+1，跳号与倒退一律拒绝。
    bool advanceWater(MsgStoreKind kind, const QString& convKey, quint64 seq);

    // 落库一条；只有 seq == 水位 + 1 会推进水位。
    // dedupKey 为私聊专用：私聊没有服务端序号，同一条消息可能既走 15 实时投递、
    // 又走 18 离线补发，靠序号无法防重，故由调用方给出「时间+内容」组成的去重键；
    // 命中已有键判为 Duplicate，不占号也不推水位。群消息留空，靠 seq 主键幂等。
    AppendResult append(MsgStoreKind kind, const QString& convKey, quint64 seq,
                        const QString& sender, quint8 direction, qint64 createdAtMs,
                        const QString& content, const QString& dedupKey = QString(),
                        const QString& timeText = QString());

    // 按 seq 升序返回该会话最近 limit 条（limit <= 0 时返回空）
    QList<StoredMessage> loadRecent(MsgStoreKind kind, const QString& convKey, int limit) const;

    // 界面「清空聊天记录」：删除该会话的消息行，保留水位——清空后旧序号不会再落库，
    // 服务端后续拉回的同一条消息因此不会复活。
    bool clearConversation(MsgStoreKind kind, const QString& convKey);

    // 账号注销：删除两库中该 user 的消息与水位
    bool removeUser();

    QString lastError() const { return m_lastError; }

private:
    MessageStore(const MessageStore&);
    MessageStore& operator=(const MessageStore&);

    QString connectionName(int kindIndex) const;
    bool ensureSchema(int kindIndex);
    // 游标写入的唯一实现：落库推进水位与锚点播种共用同一条语句，避免两处口径分叉
    bool writeWater(int kindIndex, const QString& convKey, quint64 seq);
    void truncate(MsgStoreKind kind, const QString& convKey) const;

    QString m_user;
    QString m_dir;
    bool m_avail[2];
    QString m_connName[2];
    QString m_connTag;          // 本实例的连接名标识（多实例并存时不互相顶掉）
    int m_keepLimit;
    mutable QString m_lastError;
};

#endif // MESSAGESTORE_H
