// REQ-CLI-MSGSTORE-001：接收消息本地存储层实现。
//
// 表结构（群库与私聊库各一份，同构）：
//   message(user, conv_key, seq, sender, direction, created_at, content)
//     主键 (user, conv_key, seq) —— 同序号重复投递靠它幂等，不靠应用层判重
//   conv_cursor(user, conv_key, last_seq)  本地水位，与已落库消息同步推进
#include "messagestore.h"

#include <QDir>
#include <algorithm>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

namespace {

// 连接名序号：同一进程内多个 MessageStore 实例（产品一个，测试若干）互不顶掉连接
int g_storeSeq = 0;

const char *kTableMessage =
    "create table if not exists message ("
    "user text not null, "
    "conv_key text not null, "
    "seq integer not null, "
    "sender text, "
    "direction integer, "
    "created_at integer, "
    "content text, "
    "dedup_key text, "
    "time_text text, "
    "primary key(user, conv_key, seq))";

// 私聊去重靠它：同一 (账号, 会话, 去重键) 只允许落一条
const char *kIndexDedup =
    "create index if not exists idx_dedup on message(user, conv_key, dedup_key)";

const char *kTableCursor =
    "create table if not exists conv_cursor ("
    "user text not null, "
    "conv_key text not null, "
    "last_seq integer not null, "
    "primary key(user, conv_key))";

QSqlDatabase dbOf(const QString &connName)
{
    return QSqlDatabase::database(connName, false);
}

}   // namespace


// ===================== MessageStaging =====================

bool MessageStaging::insert(const StoredMessage &msg, bool storable)
{
    if (msg.seq == 0 || m_items.contains(msg.seq)) return false;
    Staged item;
    item.msg = msg;
    item.storable = storable;
    m_items.insert(msg.seq, item);
    return true;
}

bool MessageStaging::reserve(quint64 seq)
{
    if (seq == 0 || m_items.contains(seq)) return false;
    Staged item;
    item.msg.seq = seq;
    item.storable = false;
    m_items.insert(seq, item);
    return true;
}

QList<MessageStaging::Staged> MessageStaging::takeContiguousAbove(quint64 water)
{
    QList<Staged> ready;
    // 从水位的下一号起逐格取，断在哪里就停在哪里；洞后的条目继续留着等补
    for (quint64 seq = water + 1; m_items.contains(seq); ++seq)
        ready.append(m_items.take(seq));
    return ready;
}

int MessageStaging::size() const
{
    return m_items.size();
}

bool MessageStaging::isEmpty() const
{
    return m_items.isEmpty();
}

void MessageStaging::clear()
{
    m_items.clear();
}

void MessageStaging::capTo(int maxEntries)
{
    if (maxEntries < 1 || m_items.size() <= maxEntries) return;
    QList<quint64> keys = m_items.keys();
    std::sort(keys.begin(), keys.end());
    const int drop = keys.size() - maxEntries;
    for (int i = 0; i < drop; ++i) m_items.remove(keys.at(i));
}

MessageStore::MessageStore()
    : m_avail()
    , m_connTag(QStringLiteral("msgstore%1").arg(++g_storeSeq))
    , m_keepLimit(500)
{
    m_avail[0] = m_avail[1] = false;
}

MessageStore::~MessageStore()
{
    close();
}

QString MessageStore::defaultDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

QString MessageStore::fileName(MsgStoreKind kind)
{
    return kind == MsgStoreKind::Group ? QStringLiteral("group_msg.db")
                                       : QStringLiteral("chat_msg.db");
}

QString MessageStore::connectionName(int kindIndex) const
{
    return m_connTag + (kindIndex == 0 ? QStringLiteral("_group") : QStringLiteral("_chat"));
}

bool MessageStore::open(const QString &user, const QString &dirPath)
{
    close();
    m_lastError.clear();

    if (user.isEmpty()) {
        m_lastError = QStringLiteral("未提供账号名，本地消息库不打开");
        return false;
    }
    const QString dir = dirPath.isEmpty() ? defaultDir() : dirPath;
    if (!QDir().mkpath(dir)) {
        m_lastError = QStringLiteral("本地消息目录不可用: %1").arg(dir);
        return false;
    }
    m_user = user;
    m_dir = dir;

    bool all = true;
    for (int i = 0; i < 2; ++i) {
        const MsgStoreKind kind = i == 0 ? MsgStoreKind::Group : MsgStoreKind::Chat;
        m_connName[i] = connectionName(i);
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connName[i]);
        db.setDatabaseName(QDir(m_dir).filePath(fileName(kind)));
        m_avail[i] = db.open() && ensureSchema(i);
        if (!m_avail[i]) {
            m_lastError = db.lastError().text().isEmpty()
                              ? QStringLiteral("%1 打开失败").arg(fileName(kind))
                              : QStringLiteral("%1: %2").arg(fileName(kind), db.lastError().text());
            if (db.isOpen()) db.close();
        }
        all = all && m_avail[i];
    }
    return all;
}

void MessageStore::close()
{
    for (int i = 0; i < 2; ++i) {
        if (m_connName[i].isEmpty()) continue;
        {
            QSqlDatabase db = dbOf(m_connName[i]);
            if (db.isOpen()) db.close();
        }
        QSqlDatabase::removeDatabase(m_connName[i]);   // 必须在局部 QSqlDatabase 析构后调用
        m_connName[i].clear();
        m_avail[i] = false;
    }
    m_user.clear();
    m_dir.clear();
}

bool MessageStore::ensureSchema(int kindIndex)
{
    QSqlDatabase db = dbOf(m_connName[kindIndex]);
    if (!db.isOpen()) return false;
    QSqlQuery q(db);
    if (!q.exec(QLatin1String(kTableMessage))) {
        m_lastError = q.lastError().text();
        return false;
    }
    if (!q.exec(QLatin1String(kTableCursor))) {
        m_lastError = q.lastError().text();
        return false;
    }
    if (!q.exec(QLatin1String(kIndexDedup))) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

bool MessageStore::available(MsgStoreKind kind) const
{
    return m_avail[static_cast<int>(kind)];
}

QString MessageStore::filePath(MsgStoreKind kind) const
{
    if (m_dir.isEmpty()) return QString();
    return QDir(m_dir).filePath(fileName(kind));
}

void MessageStore::setKeepLimit(int n)
{
    m_keepLimit = n < 1 ? 1 : n;
}

quint64 MessageStore::waterSeq(MsgStoreKind kind, const QString &convKey) const
{
    if (!available(kind) || convKey.isEmpty()) return 0;
    QSqlDatabase db = dbOf(m_connName[static_cast<int>(kind)]);
    if (!db.isOpen()) return 0;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("select last_seq from conv_cursor where user=? and conv_key=?"));
    q.bindValue(0, m_user);
    q.bindValue(1, convKey);
    if (!q.exec() || !q.next()) return 0;
    return q.value(0).toULongLong();
}

bool MessageStore::writeWater(int kindIndex, const QString &convKey, quint64 seq)
{
    QSqlDatabase db = dbOf(m_connName[kindIndex]);
    if (!db.isOpen()) return false;
    QSqlQuery up(db);
    up.prepare(QStringLiteral("insert into conv_cursor(user, conv_key, last_seq) values(?,?,?)"
                              " on conflict(user, conv_key) do update set last_seq=excluded.last_seq"));
    up.bindValue(0, m_user);
    up.bindValue(1, convKey);
    up.bindValue(2, static_cast<qulonglong>(seq));
    if (!up.exec()) {
        m_lastError = up.lastError().text();
        return false;
    }
    return true;
}

bool MessageStore::seedWater(MsgStoreKind kind, const QString &convKey, quint64 anchorSeq)
{
    if (!available(kind) || convKey.isEmpty() || anchorSeq == 0) return false;
    const quint64 water = waterSeq(kind, convKey);
    if (anchorSeq <= water) return false;       // 水位只增不减
    return writeWater(static_cast<int>(kind), convKey, anchorSeq);
}

bool MessageStore::advanceWater(MsgStoreKind kind, const QString &convKey, quint64 seq)
{
    if (!available(kind) || convKey.isEmpty() || seq == 0) return false;
    // 只允许单格前进：跳号会掩盖丢号，倒退会把已确认的区间重新当成洞
    if (seq != waterSeq(kind, convKey) + 1) return false;
    return writeWater(static_cast<int>(kind), convKey, seq);
}

MessageStore::AppendResult MessageStore::append(MsgStoreKind kind, const QString &convKey,
                                                quint64 seq, const QString &sender,
                                                quint8 direction, qint64 createdAtMs,
                                                const QString &content, const QString &dedupKey,
                                                const QString &timeText)
{
    if (!available(kind)) return AppendResult::Unavailable;
    if (convKey.isEmpty() || seq == 0) {
        m_lastError = QStringLiteral("非法落库请求: 会话键为空或序号为 0");
        return AppendResult::Failed;
    }
    QSqlDatabase db = dbOf(m_connName[static_cast<int>(kind)]);
    if (!db.isOpen()) {
        m_avail[static_cast<int>(kind)] = false;
        return AppendResult::Unavailable;
    }

    const quint64 water = waterSeq(kind, convKey);
    if (seq > water + 1) return AppendResult::RejectedBeyondWater;   // 有洞：只留在内存
    if (seq <= water) return AppendResult::Duplicate;                // 已被水位跨过（重复或已清空）

    // 私聊去重：同一 (账号, 会话, 时间+内容) 已落过就不再占号
    if (!dedupKey.isEmpty()) {
        QSqlQuery seen(db);
        seen.prepare(QStringLiteral("select 1 from message where user=? and conv_key=? and dedup_key=? limit 1"));
        seen.bindValue(0, m_user);
        seen.bindValue(1, convKey);
        seen.bindValue(2, dedupKey);
        if (!seen.exec()) {
            m_lastError = seen.lastError().text();
            return AppendResult::Failed;
        }
        if (seen.next()) return AppendResult::Duplicate;
    }

    QSqlQuery q(db);
    q.prepare(QStringLiteral("insert into message"
                             "(user, conv_key, seq, sender, direction, created_at, content, dedup_key, time_text)"
                             " values(?,?,?,?,?,?,?,?,?)"));
    q.bindValue(0, m_user);
    q.bindValue(1, convKey);
    q.bindValue(2, static_cast<qulonglong>(seq));
    q.bindValue(3, sender);
    q.bindValue(4, int(direction));
    q.bindValue(5, createdAtMs);
    q.bindValue(6, content);
    q.bindValue(7, dedupKey.isEmpty() ? QVariant() : QVariant(dedupKey));
    q.bindValue(8, timeText);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return AppendResult::Failed;
    }

    // 水位与落库同一步推进：写游标失败就把刚插入的行撤掉，两者不允许分叉
    if (!writeWater(static_cast<int>(kind), convKey, seq)) {
        QSqlQuery rollback(db);
        rollback.prepare(QStringLiteral("delete from message where user=? and conv_key=? and seq=?"));
        rollback.bindValue(0, m_user);
        rollback.bindValue(1, convKey);
        rollback.bindValue(2, static_cast<qulonglong>(seq));
        rollback.exec();
        return AppendResult::Failed;
    }

    truncate(kind, convKey);
    return AppendResult::Stored;
}

void MessageStore::truncate(MsgStoreKind kind, const QString &convKey) const
{
    QSqlDatabase db = dbOf(m_connName[static_cast<int>(kind)]);
    if (!db.isOpen()) return;

    // 找第 keepLimit 新的序号作为保留下界，删掉比它更旧的；不足 keepLimit 条时查不到行
    QSqlQuery q(db);
    q.prepare(QStringLiteral("select seq from message where user=? and conv_key=?"
                             " order by seq desc limit 1 offset ?"));
    q.bindValue(0, m_user);
    q.bindValue(1, convKey);
    q.bindValue(2, m_keepLimit - 1);
    if (!q.exec() || !q.next()) return;

    const quint64 floor = q.value(0).toULongLong();
    QSqlQuery del(db);
    del.prepare(QStringLiteral("delete from message where user=? and conv_key=? and seq<?"));
    del.bindValue(0, m_user);
    del.bindValue(1, convKey);
    del.bindValue(2, static_cast<qulonglong>(floor));
    if (!del.exec()) m_lastError = del.lastError().text();
}

QList<StoredMessage> MessageStore::loadRecent(MsgStoreKind kind, const QString &convKey,
                                              int limit) const
{
    QList<StoredMessage> out;
    if (!available(kind) || convKey.isEmpty() || limit <= 0) return out;
    QSqlDatabase db = dbOf(m_connName[static_cast<int>(kind)]);
    if (!db.isOpen()) return out;

    QSqlQuery q(db);
    q.prepare(QStringLiteral("select seq, conv_key, sender, direction, created_at, content, time_text"
                             " from message where user=? and conv_key=? order by seq desc limit ?"));
    q.bindValue(0, m_user);
    q.bindValue(1, convKey);
    q.bindValue(2, limit);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return out;
    }
    while (q.next()) {
        StoredMessage msg;
        msg.seq = q.value(0).toULongLong();
        msg.convKey = q.value(1).toString();
        msg.sender = q.value(2).toString();
        msg.direction = quint8(q.value(3).toInt());
        msg.createdAtMs = q.value(4).toLongLong();
        msg.content = q.value(5).toString();
        msg.timeText = q.value(6).toString();
        out.prepend(msg);                       // 查的是倒序，还原成 seq 升序
    }
    return out;
}

bool MessageStore::clearConversation(MsgStoreKind kind, const QString &convKey)
{
    if (!available(kind) || convKey.isEmpty()) return false;
    QSqlDatabase db = dbOf(m_connName[static_cast<int>(kind)]);
    if (!db.isOpen()) return false;

    QSqlQuery q(db);
    q.prepare(QStringLiteral("delete from message where user=? and conv_key=?"));
    q.bindValue(0, m_user);
    q.bindValue(1, convKey);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;                                // 水位保留：清空后旧序号不再落库
}

bool MessageStore::removeUser()
{
    if (m_user.isEmpty()) return false;
    bool all = true;
    for (int i = 0; i < 2; ++i) {
        if (!m_avail[i]) continue;              // 本来就用不了的库无需清除，也不算失败
        QSqlDatabase db = dbOf(m_connName[i]);
        if (!db.isOpen()) { all = false; continue; }
        QSqlQuery q(db);
        q.prepare(QStringLiteral("delete from message where user=?"));
        q.bindValue(0, m_user);
        bool ok = q.exec();
        QSqlQuery c(db);
        c.prepare(QStringLiteral("delete from conv_cursor where user=?"));
        c.bindValue(0, m_user);
        ok = ok && c.exec();
        if (!ok) {
            m_lastError = q.lastError().text().isEmpty() ? c.lastError().text() : q.lastError().text();
            all = false;
        }
    }
    return all;
}
