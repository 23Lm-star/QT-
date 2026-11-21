#include "sessionstore.h"

#include "protocol.h"
#include "redisstore.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>

namespace {
// 恢复凭据字节数：32B 随机 → 64 位小写十六进制
const int RESUME_TOKEN_BYTES = 32;
}

SessionStore& SessionStore::instance()
{
    static SessionStore store;
    return store;
}

QString SessionStore::generateResumeToken()
{
    QByteArray raw(RESUME_TOKEN_BYTES, '\0');
    QRandomGenerator *rng = QRandomGenerator::system();
    for (int i = 0; i < RESUME_TOKEN_BYTES; i += 4) {
        const quint32 v = rng->generate();
        for (int j = 0; j < 4 && i + j < RESUME_TOKEN_BYTES; ++j)
            raw[i + j] = char((v >> (8 * j)) & 0xff);
    }
    return QString::fromLatin1(raw.toHex());
}

QString SessionStore::tokenHashHex(const QString& tokenPlain)
{
    const QByteArray digest = QCryptographicHash::hash(
        tokenPlain.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(digest.toHex());
}

QByteArray SessionStore::buildSessionJson(const QString& user, const QString& clientId,
                                          qint64 loginVersion, const QString& tokenHash,
                                          const QString& state)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("user"), user);
    obj.insert(QStringLiteral("clientId"), clientId);
    obj.insert(QStringLiteral("loginVersion"), loginVersion);
    obj.insert(QStringLiteral("tokenHash"), tokenHash);
    obj.insert(QStringLiteral("state"), state);
    // QJsonDocument::toJson(Compact) 按键名字母序输出：clientId,loginVersion,state,tokenHash,user
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

bool SessionStore::parseSessionJson(const QByteArray& json, SessionInfo& out)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject obj = doc.object();
    if (!obj.contains(QStringLiteral("user")) || !obj.contains(QStringLiteral("clientId"))
        || !obj.contains(QStringLiteral("loginVersion")) || !obj.contains(QStringLiteral("tokenHash"))
        || !obj.contains(QStringLiteral("state"))) return false;
    const QJsonValue version = obj.value(QStringLiteral("loginVersion"));
    if (!version.isDouble()) return false;   // JSON 数字统一走 double，非数值即非法
    out.user = obj.value(QStringLiteral("user")).toString();
    out.clientId = obj.value(QStringLiteral("clientId")).toString();
    out.loginVersion = qint64(version.toDouble());
    out.tokenHash = obj.value(QStringLiteral("tokenHash")).toString();
    out.state = obj.value(QStringLiteral("state")).toString();
    return !out.user.isEmpty() && !out.clientId.isEmpty() && !out.tokenHash.isEmpty()
            && !out.state.isEmpty();
}

QByteArray SessionStore::versionNeedle(qint64 loginVersion)
{
    // 依赖 buildSessionJson 的字母序输出：loginVersion 紧跟 state 键
    return QByteArray("\"loginVersion\":") + QByteArray::number(loginVersion)
            + ",\"state\"";
}

bool SessionStore::redisEnabled() const
{
    return RedisStore::instance().enabled();
}

int SessionStore::createSession(const QString& clientId, const QString& user,
                                qint64 loginVersion, const QString& tokenPlain)
{
    RedisStore& redis = RedisStore::instance();
    if (!redis.enabled()) return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    if (!isValidClientId(clientId.toLatin1()) || user.isEmpty() || tokenPlain.isEmpty())
        return ENUM_PROTOCOL_CODE_BAD_PARAM;
    const QByteArray json = buildSessionJson(user, clientId, loginVersion,
                                             tokenHashHex(tokenPlain));
    if (!redis.setSession(clientId, user, json, SESSION_TTL_SEC))
        return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    return ENUM_PROTOCOL_CODE_OK;
}

int SessionStore::readSession(const QString& clientId, SessionInfo& out)
{
    RedisStore& redis = RedisStore::instance();
    if (!redis.enabled()) return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    QByteArray json;
    bool exists = false;
    if (!redis.getSession(clientId, json, exists))
        return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    if (!exists || !parseSessionJson(json, out)) return ENUM_PROTOCOL_CODE_SESSION_INVALID;
    return ENUM_PROTOCOL_CODE_OK;
}

int SessionStore::validateResume(const QString& clientId, const QString& tokenPlain,
                                 SessionInfo& out)
{
    const int rc = readSession(clientId, out);
    if (rc != ENUM_PROTOCOL_CODE_OK) return rc;
    if (out.state != QStringLiteral("active")) return ENUM_PROTOCOL_CODE_SESSION_INVALID;
    // 摘要比对在常量时间的字符串比较下进行；明文不出 Redis 层
    if (out.tokenHash != tokenHashHex(tokenPlain))
        return ENUM_PROTOCOL_CODE_BAD_RESUME_TOKEN;
    const int touchRc = touchSession(clientId);
    if (touchRc != ENUM_PROTOCOL_CODE_OK) return touchRc;
    return ENUM_PROTOCOL_CODE_OK;
}

int SessionStore::touchSession(const QString& clientId)
{
    RedisStore& redis = RedisStore::instance();
    if (!redis.enabled()) return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    bool refreshed = false;
    if (!redis.expireKey(QStringLiteral("session:%1").arg(clientId), SESSION_TTL_SEC, refreshed))
        return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    if (!refreshed) return ENUM_PROTOCOL_CODE_SESSION_INVALID;
    return ENUM_PROTOCOL_CODE_OK;
}

int SessionStore::sessionTtl(const QString& clientId, qint64& ttlSeconds)
{
    RedisStore& redis = RedisStore::instance();
    if (!redis.enabled()) return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    if (!redis.keyTtl(QStringLiteral("session:%1").arg(clientId), ttlSeconds))
        return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    return ENUM_PROTOCOL_CODE_OK;
}

int SessionStore::revokeIfMatch(const QString& clientId, const QString& user,
                                qint64 loginVersion, const QString& tokenPlain, bool& revoked)
{
    RedisStore& redis = RedisStore::instance();
    revoked = false;
    if (!redis.enabled()) return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    const QByteArray hashNeedle = "\"tokenHash\":\"" + tokenHashHex(tokenPlain).toLatin1() + "\"";
    if (!redis.evalSessionRevokeIfMatch(clientId, user, versionNeedle(loginVersion),
                                        hashNeedle, revoked))
        return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    return ENUM_PROTOCOL_CODE_OK;
}

int SessionStore::revokeSessionByVersion(const QString& clientId, const QString& user,
                                         qint64 loginVersion, bool& revoked)
{
    RedisStore& redis = RedisStore::instance();
    revoked = false;
    if (!redis.enabled()) return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    if (!redis.evalSessionRevokeIfVersion(clientId, user, versionNeedle(loginVersion), revoked))
        return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    return ENUM_PROTOCOL_CODE_OK;
}

int SessionStore::revokeAllForUser(const QString& user, int& removed)
{
    RedisStore& redis = RedisStore::instance();
    removed = 0;
    if (!redis.enabled()) return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    if (!redis.revokeUserSessions(user, removed))
        return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    return ENUM_PROTOCOL_CODE_OK;
}

int SessionStore::setOnline(const QString& user, qint64 loginVersion)
{
    RedisStore& redis = RedisStore::instance();
    if (!redis.enabled()) return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    if (!redis.setEx(QStringLiteral("online:%1").arg(user), ONLINE_TTL_SEC,
                     QByteArray::number(loginVersion)))
        return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    return ENUM_PROTOCOL_CODE_OK;
}

int SessionStore::deleteOnlineIfVersion(const QString& user, qint64 loginVersion, bool& deleted)
{
    RedisStore& redis = RedisStore::instance();
    deleted = false;
    if (!redis.enabled()) return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    if (!redis.evalOnlineDeleteIfVersion(user, QByteArray::number(loginVersion), deleted))
        return ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE;
    return ENUM_PROTOCOL_CODE_OK;
}
