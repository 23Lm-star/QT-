#ifndef REDISSTORE_H
#define REDISSTORE_H

#include <QByteArray>
#include <QString>
#include <QStringList>

class RedisStore
{
public:
    static RedisStore& instance();
    bool enabled() const;
    bool setEx(const QString& key, int seconds, const QByteArray& value);
    bool get(const QString& key, QByteArray& value);
    bool del(const QString& key);
    bool incr(const QString& key, qint64& value);
    bool evalCompareDelete(const QString& key, const QByteArray& version);
    bool mget(const QStringList& keys, QList<QByteArray>& values);

    // ===== REQ-AUTH-CONNECTION-001 阶段二：验证码与会话原子操作 =====
    // 约定：返回值 false = Redis 不可用/命令失败（调用方必须失败关闭，不得伪装成功）；
    // 返回值 true 时业务结果经 out 参数给出。键名一律由已校验的 clientId/user 拼出，
    // 用户输入只走 ARGV，绝不拼进脚本或键名。
    // 消费验证码：原子比较并删除 code+fail；locked 优先。outcome：
    // 0=消费成功 1=输入不符 2=无有效码/已过期 3=锁定中
    bool evalCaptchaConsume(const QString& keyPrefix, const QByteArray& input, int& outcome);
    // 失败计数：INCR fail 并续期；达到阈值时写 lock 并作废 code。
    bool evalCaptchaFailure(const QString& keyPrefix, int maxFails, int lockSeconds,
                            int failTtl, bool& locked);
    // 写入会话（SETEX session:{clientId} + SADD sessions:{user}，单脚本原子）
    bool setSession(const QString& clientId, const QString& user,
                    const QByteArray& json, int ttlSeconds);
    // 读会话；exists 区分键不存在（true+false）与 Redis 故障（false）
    bool getSession(const QString& clientId, QByteArray& json, bool& exists);
    bool keyTtl(const QString& key, qint64& ttlSeconds);
    bool expireKey(const QString& key, int seconds, bool& refreshed);
    // 条件撤销：会话 JSON 同时命中版本与 tokenHash 才删键并移出索引
    bool evalSessionRevokeIfMatch(const QString& clientId, const QString& user,
                                  const QByteArray& versionNeedle, const QByteArray& hashNeedle,
                                  bool& revoked);
    // 条件撤销（仅版本，无 token 场景如 119 退出）：版本命中才删，防止误删同 clientId 新会话
    bool evalSessionRevokeIfVersion(const QString& clientId, const QString& user,
                                    const QByteArray& versionNeedle, bool& revoked);
    // 撤销账号全部会话：枚举 sessions:{user}，逐项核对删除，返回移除数
    bool revokeUserSessions(const QString& user, int& removed);
    // 在线态条件删除：仅当值等于期望版本时删除 online:{user}
    bool evalOnlineDeleteIfVersion(const QString& user, const QByteArray& version, bool& deleted);
    // 集合只读辅助（会话索引核对与探针验证）
    bool sismember(const QString& key, const QString& member, bool& isMember);
    bool scard(const QString& key, qint64& count);
private:
    RedisStore();
    RedisStore(const RedisStore&) = delete;
    RedisStore& operator=(const RedisStore&) = delete;
    bool command(const QList<QByteArray>& args, QByteArray& payload, char& type);
    bool commandInt(const QList<QByteArray>& args, qint64& value);
    bool parseResponse(const QByteArray& data, int& offset, QByteArray& payload, char& type);
    bool parseUrl();
    bool m_enabled = false;
    QString m_host;
    quint16 m_port = 6379;
    QByteArray m_password;
};

#endif
