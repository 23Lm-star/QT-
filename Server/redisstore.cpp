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

// REQ-AUTH-CONNECTION-001 阶段二修复：原 `m_enabled(parseUrl())` 在初始化列表里调用
// parseUrl，但 m_host/m_password 声明在 m_enabled 之后、尚未构造，向其赋值是未定义行为
// （URL 有效时实测必崩）。改为构造函数体内执行，此时成员默认初始化已完成。
RedisStore::RedisStore() { m_enabled = parseUrl(); }

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
        if (!ok || length < -1) return false;
        // $-1 nil：Redis 回复恰为 "$-1\r\n"，头部之后没有正文和结尾 CRLF，
        // 不能套用正文边界检查（原实现把缺失键误判为命令失败，
        // 导致 GET 缺失键=读错误，旧回退判断从未按预期触发）。
        if (length < 0) { payload.clear(); return true; }
        if (offset + length + 2 > data.size()) return false;
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

// ===== REQ-AUTH-CONNECTION-001 阶段二：验证码与会话原子操作 =====

// 执行返回整数的命令（Lua/EVAL、TTL 等以 ':' 回复）；非整数回复视为失败。
bool RedisStore::commandInt(const QList<QByteArray>& args, qint64& value)
{
    QByteArray payload; char type = 0;
    if (!command(args, payload, type) || type != ':') return false;
    bool ok = false;
    value = payload.toLongLong(&ok);
    return ok;
}

bool RedisStore::evalCaptchaConsume(const QString& keyPrefix, const QByteArray& input, int& outcome)
{
    // KEYS: lock code fail —— 锁定优先拒绝；比对成功即一次性作废 code 并清计数。
    static const QByteArray script =
        "if redis.call('EXISTS',KEYS[1])==1 then return 3 end "
        "local c=redis.call('GET',KEYS[2]) "
        "if not c then return 2 end "
        "if c~=ARGV[1] then return 1 end "
        "redis.call('DEL',KEYS[2]) redis.call('DEL',KEYS[3]) "
        "return 0";
    qint64 v = -1;
    if (!commandInt({"EVAL", script, "3",
                     (keyPrefix + "lock").toUtf8(), (keyPrefix + "code").toUtf8(),
                     (keyPrefix + "fail").toUtf8(), input}, v)) return false;
    if (v < 0 || v > 3) return false;   // 协议外返回值按失败处理
    outcome = int(v);
    return true;
}

bool RedisStore::evalCaptchaFailure(const QString& keyPrefix, int maxFails, int lockSeconds,
                                    int failTtl, bool& locked)
{
    // KEYS: fail lock code —— INCR 续期；达阈值写 lock 并作废当前码。
    static const QByteArray script =
        "local n=redis.call('INCR',KEYS[1]) "
        "redis.call('EXPIRE',KEYS[1],ARGV[3]) "
        "if n>=tonumber(ARGV[1]) then "
        "redis.call('SETEX',KEYS[2],ARGV[2],ARGV[2]) redis.call('DEL',KEYS[3]) return 1 end "
        "return 0";
    qint64 v = -1;
    if (!commandInt({"EVAL", script, "3",
                     (keyPrefix + "fail").toUtf8(), (keyPrefix + "lock").toUtf8(),
                     (keyPrefix + "code").toUtf8(),
                     QByteArray::number(maxFails), QByteArray::number(lockSeconds),
                     QByteArray::number(failTtl)}, v)) return false;
    if (v != 0 && v != 1) return false;
    locked = (v == 1);
    return true;
}

bool RedisStore::setSession(const QString& clientId, const QString& user,
                            const QByteArray& json, int ttlSeconds)
{
    static const QByteArray script =
        "redis.call('SETEX',KEYS[1],ARGV[1],ARGV[2]) "
        "redis.call('SADD',KEYS[2],ARGV[3]) "
        "return 1";
    qint64 v = -1;
    return commandInt({"EVAL", script, "2",
                       QStringLiteral("session:%1").arg(clientId).toUtf8(),
                       QStringLiteral("sessions:%1").arg(user).toUtf8(),
                       QByteArray::number(ttlSeconds), json, clientId.toUtf8()}, v)
            && v == 1;
}

bool RedisStore::getSession(const QString& clientId, QByteArray& json, bool& exists)
{
    QByteArray payload; char type = 0;
    const QString key = QStringLiteral("session:%1").arg(clientId);
    if (!command({"GET", key.toUtf8()}, payload, type)) return false;
    if (type != '$' && type != '+') return false;
    // 会话值恒为非空 JSON；$-1（键不存在）回复为空 → exists=false 与 Redis 故障区分开
    exists = !payload.isEmpty();
    json = exists ? payload : QByteArray();
    return true;
}

bool RedisStore::keyTtl(const QString& key, qint64& ttlSeconds)
{
    return commandInt({"TTL", key.toUtf8()}, ttlSeconds);
}

bool RedisStore::expireKey(const QString& key, int seconds, bool& refreshed)
{
    qint64 v = -1;
    if (!commandInt({"EXPIRE", key.toUtf8(), QByteArray::number(seconds)}, v)) return false;
    if (v != 0 && v != 1) return false;
    refreshed = (v == 1);
    return true;
}

bool RedisStore::evalSessionRevokeIfMatch(const QString& clientId, const QString& user,
                                          const QByteArray& versionNeedle, const QByteArray& hashNeedle,
                                          bool& revoked)
{
    // 会话 JSON 由 SessionStore 紧凑生成；版本与 tokenHash 双指针同时命中才撤销。
    static const QByteArray script =
        "local v=redis.call('GET',KEYS[1]) "
        "if not v then return 0 end "
        "if string.find(v,ARGV[2],1,true) and string.find(v,ARGV[3],1,true) then "
        "redis.call('DEL',KEYS[1]) redis.call('SREM',KEYS[2],ARGV[1]) return 1 end "
        "return 0";
    qint64 v = -1;
    if (!commandInt({"EVAL", script, "2",
                     QStringLiteral("session:%1").arg(clientId).toUtf8(),
                     QStringLiteral("sessions:%1").arg(user).toUtf8(),
                     clientId.toUtf8(), versionNeedle, hashNeedle}, v)) return false;
    if (v != 0 && v != 1) return false;
    revoked = (v == 1);
    return true;
}

bool RedisStore::evalSessionRevokeIfVersion(const QString& clientId, const QString& user,
                                            const QByteArray& versionNeedle, bool& revoked)
{
    // 119 退出不带 token：只按 clientId+版本条件撤销，同 clientId 已被新登录覆盖时不误删。
    static const QByteArray script =
        "local v=redis.call('GET',KEYS[1]) "
        "if not v then return 0 end "
        "if string.find(v,ARGV[2],1,true) then "
        "redis.call('DEL',KEYS[1]) redis.call('SREM',KEYS[2],ARGV[1]) return 1 end "
        "return 0";
    qint64 v = -1;
    if (!commandInt({"EVAL", script, "2",
                     QStringLiteral("session:%1").arg(clientId).toUtf8(),
                     QStringLiteral("sessions:%1").arg(user).toUtf8(),
                     clientId.toUtf8(), versionNeedle}, v)) return false;
    if (v != 0 && v != 1) return false;
    revoked = (v == 1);
    return true;
}

bool RedisStore::revokeUserSessions(const QString& user, int& removed)
{
    // 枚举即核对：成员缺键也移出索引，避免脏成员滞留。
    static const QByteArray script =
        "local ms=redis.call('SMEMBERS',KEYS[1]) "
        "local n=0 "
        "for i=1,#ms do "
        "redis.call('DEL','session:'..ms[i]) "
        "n=n+1 "
        "end "
        "redis.call('DEL',KEYS[1]) "
        "return n";
    qint64 v = -1;
    if (!commandInt({"EVAL", script, "1",
                     QStringLiteral("sessions:%1").arg(user).toUtf8()}, v)) return false;
    if (v < 0) return false;
    removed = int(v);
    return true;
}

bool RedisStore::evalOnlineDeleteIfVersion(const QString& user, const QByteArray& version, bool& deleted)
{
    qint64 v = -1;
    if (!commandInt({"EVAL",
                     "if redis.call('GET',KEYS[1])==ARGV[1] then return redis.call('DEL',KEYS[1]) else return 0 end",
                     "1", QStringLiteral("online:%1").arg(user).toUtf8(), version}, v)) return false;
    if (v != 0 && v != 1) return false;
    deleted = (v == 1);
    return true;
}

bool RedisStore::sismember(const QString& key, const QString& member, bool& isMember)
{
    qint64 v = -1;
    if (!commandInt({"SISMEMBER", key.toUtf8(), member.toUtf8()}, v)) return false;
    if (v != 0 && v != 1) return false;
    isMember = (v == 1);
    return true;
}

bool RedisStore::scard(const QString& key, qint64& count)
{
    return commandInt({"SCARD", key.toUtf8()}, count);
}
