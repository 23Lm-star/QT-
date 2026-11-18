#include "redisstore.h"

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QUrl>
#include <QTcpSocket>

Q_DECLARE_LOGGING_CATEGORY(lcNet)

namespace {
const int REDIS_TIMEOUT_MS = 80;
QByteArray bulk(const QByteArray& value)
{
    return QByteArray("$") + QByteArray::number(value.size()) + "\r\n" + value + "\r\n";
}
}

RedisStore& RedisStore::instance()
{
    static RedisStore store;
    return store;
}

RedisStore::RedisStore() : m_enabled(parseUrl()) {}

bool RedisStore::parseUrl()
{
    const QByteArray raw = qgetenv("CLOUDDISK_REDIS_URL");
    if (raw.isEmpty()) return false;
    const QUrl url(QString::fromUtf8(raw));
    if (!url.isValid() || url.scheme() != QStringLiteral("redis") || url.host().isEmpty()) {
        qCWarning(lcNet) << "Redis 配置无效，保持现有存储路径";
        return false;
    }
    m_host = url.host();
    m_port = url.port(6379);
    m_password = url.password().toUtf8();
    return true;
}

bool RedisStore::enabled() const { return m_enabled; }

bool RedisStore::parseResponse(const QByteArray& data, int& offset, QByteArray& payload, char& type)
{
    if (offset >= data.size()) return false;
    type = data.at(offset++);
    const int lineEnd = data.indexOf("\r\n", offset);
    if (lineEnd < 0) return false;
    const QByteArray line = data.mid(offset, lineEnd - offset);
    offset = lineEnd + 2;
    if (type == '+' || type == '-' || type == ':') { payload = line; return true; }
    if (type == '$') {
        bool ok = false;
        const int length = line.toInt(&ok);
        if (!ok || length < -1 || offset + length + 2 > data.size()) return false;
        if (length < 0) { payload.clear(); return true; }
        payload = data.mid(offset, length);
        offset += length + 2;
        return true;
    }
    if (type == '*') {
        const int count = line.toInt();
        QByteArray joined;
        for (int i = 0; i < count; ++i) {
            QByteArray item; char itemType = 0;
            if (!parseResponse(data, offset, item, itemType)) return false;
            joined += item;
            joined += '\0';
        }
        payload = joined;
        return true;
    }
    return false;
}

bool RedisStore::command(const QList<QByteArray>& args, QByteArray& payload, char& type)
{
    if (!m_enabled) return false;
    QByteArray request = "*" + QByteArray::number(args.size()) + "\r\n";
    for (const QByteArray& arg : args) request += bulk(arg);
    QTcpSocket socket;
    socket.connectToHost(m_host, m_port);
    if (!socket.waitForConnected(REDIS_TIMEOUT_MS)) return false;
    if (!m_password.isEmpty()) {
        const QByteArray auth = "*2\r\n" + bulk("AUTH") + bulk(m_password);
        if (socket.write(auth) != auth.size() || !socket.waitForBytesWritten(REDIS_TIMEOUT_MS) || !socket.waitForReadyRead(REDIS_TIMEOUT_MS)) return false;
        const QByteArray response = socket.readAll(); int offset = 0; QByteArray ignored; char authType = 0;
        if (!parseResponse(response, offset, ignored, authType) || authType == '-') return false;
    }
    if (socket.write(request) != request.size() || !socket.waitForBytesWritten(REDIS_TIMEOUT_MS)) return false;
    QByteArray response;
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < REDIS_TIMEOUT_MS) {
        if (socket.waitForReadyRead(qMax(1, REDIS_TIMEOUT_MS - int(timer.elapsed())))) response += socket.readAll();
        int offset = 0;
        if (parseResponse(response, offset, payload, type)) return type != '-';
    }
    return false;
}

bool RedisStore::setEx(const QString& key, int seconds, const QByteArray& value)
{ QByteArray payload; char type = 0; return command({"SETEX", key.toUtf8(), QByteArray::number(seconds), value}, payload, type); }
bool RedisStore::get(const QString& key, QByteArray& value)
{ char type = 0; return command({"GET", key.toUtf8()}, value, type) && (type == '$' || type == '+'); }
bool RedisStore::del(const QString& key)
{ QByteArray payload; char type = 0; return command({"DEL", key.toUtf8()}, payload, type); }
bool RedisStore::incr(const QString& key, qint64& value)
{ QByteArray payload; char type = 0; if (!command({"INCR", key.toUtf8()}, payload, type) || type != ':') return false; bool ok = false; value = payload.toLongLong(&ok); return ok; }
bool RedisStore::evalCompareDelete(const QString& key, const QByteArray& version)
{ QByteArray payload; char type = 0; const QByteArray script = "if redis.call('get',KEYS[1]) == ARGV[1] then return redis.call('del',KEYS[1]) else return 0 end"; return command({"EVAL", script, "1", key.toUtf8(), version}, payload, type); }
bool RedisStore::mget(const QStringList& keys, QList<QByteArray>& values)
{ Q_UNUSED(keys); Q_UNUSED(values); return false; }
