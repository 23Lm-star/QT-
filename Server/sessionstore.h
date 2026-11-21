#ifndef SESSIONSTORE_H
#define SESSIONSTORE_H
// REQ-AUTH-CONNECTION-001 阶段二：Redis 认证会话底层。
// 会话值 = SessionStore 生成的紧凑 JSON（键按字母序：clientId/loginVersion/state/tokenHash/user）；
// 服务端只存 tokenHash（SHA-256 小写十六进制），明文恢复凭据只在登录响应里回传一次。
// 所有对外方法返回 0=成功，负数=协议标准码（-1003 Redis 不可用 / -3016 凭据错误 /
// -3017 会话失效 / -3018 写入失败）。Redis 故障一律失败关闭，不退回连接内存认证。

#include <QByteArray>
#include <QString>

struct SessionInfo {
    QString user;
    QString clientId;
    qint64 loginVersion = 0;
    QString tokenHash;   // SHA-256 小写十六进制
    QString state;       // active / revoked
};

class SessionStore
{
public:
    static SessionStore& instance();

    static const int SESSION_TTL_SEC = 1800;  // session:{clientId}
    static const int ONLINE_TTL_SEC  = 90;    // online:{user}

    // ===== 纯逻辑（可离线断言） =====
    // 64 位小写十六进制恢复凭据（32 随机字节）
    static QString generateResumeToken();
    static QString tokenHashHex(const QString& tokenPlain);
    static QByteArray buildSessionJson(const QString& user, const QString& clientId,
                                       qint64 loginVersion, const QString& tokenHash,
                                       const QString& state = QStringLiteral("active"));
    static bool parseSessionJson(const QByteArray& json, SessionInfo& out);
    // 条件撤销的版本指针：","loginVersion":N,"state" —— 依赖紧凑 JSON 字母序，随 build 同步
    static QByteArray versionNeedle(qint64 loginVersion);

    bool redisEnabled() const;

    // ===== Redis 底层操作（阶段二提供，阶段三接入业务链路） =====
    // 建立/覆盖会话：SETEX session + SADD sessions:{user}；token 明文只在参数里出现，落盘仅摘要
    int createSession(const QString& clientId, const QString& user,
                      qint64 loginVersion, const QString& tokenPlain);
    int readSession(const QString& clientId, SessionInfo& out);
    // 恢复校验：存在 + state=active + tokenHash 命中；成功后续期 TTL 1800。
    // 0=成功(out 填充)；-1003 Redis 故障；-3016 凭据错误；-3017 不存在/已撤销/过期
    int validateResume(const QString& clientId, const QString& tokenPlain, SessionInfo& out);
    // 心跳续期会话 TTL；键不存在或 Redis 故障返回负码
    int touchSession(const QString& clientId);
    int sessionTtl(const QString& clientId, qint64& ttlSeconds);
    // 条件撤销（按 clientId+版本+tokenHash）；Redis 故障 -1003，否则 0（revoked 给出结果）
    int revokeIfMatch(const QString& clientId, const QString& user,
                      qint64 loginVersion, const QString& tokenPlain, bool& revoked);
    // 仅版本条件撤销（119 退出，服务端不持明文 token）；语义同上
    int revokeSessionByVersion(const QString& clientId, const QString& user,
                               qint64 loginVersion, bool& revoked);
    // 撤销账号全部会话（注销）；返回移除数经 removed 带出
    int revokeAllForUser(const QString& user, int& removed);

    // 在线态（展示性，失败不阻断业务）：SET online:{user}=version EX 90
    int setOnline(const QString& user, qint64 loginVersion);
    // 仅版本匹配才删除在线键（断线回调不得误删新会话的在线态）
    int deleteOnlineIfVersion(const QString& user, qint64 loginVersion, bool& deleted);

private:
    SessionStore() = default;
    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;
};

#endif // SESSIONSTORE_H
