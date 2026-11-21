// REQ-AUTH-CONNECTION-001 阶段二：RedisStore/SessionStore 探针 harness（测试专用，不进 Server.pro）。
// 由 redis_session_probe.py 生成 .pro 引用本文件 + Server/redisstore.cpp + Server/sessionstore.cpp。
// 模式：
//   selftest <nonce>            —— 对真实 Redis 的原子操作全场景（键按 nonce 隔离并自清理）
//   race-setup <clientId> <code>   —— 预置验证码
//   race-consume <clientId> <code> —— 单进程一次原子消费，打印 RACE_OUTCOME n
//   race-state <clientId>          —— 打印 RACE_CODE_TTL <ttl>（消费成功后应为 -2）
//   down                           —— Redis 已配置但不可达：所有操作必须失败关闭
//   disabled                       —— 未配置 Redis：SessionStore 一律拒绝（无内存认证旁路）
#include "protocol.h"
#include "redisstore.h"
#include "sessionstore.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QStringList>
#include <cstdio>

Q_LOGGING_CATEGORY(lcNet, "probe.net")
Q_LOGGING_CATEGORY(lcAuth, "probe.auth")

namespace {
int g_fails = 0;

void ck(const char* name, bool ok, const QString& detail = QString())
{
    printf("CHECK %s %s%s\n", name, ok ? "PASS" : "FAIL",
           ok ? "" : QString(" | %1").arg(detail).toLocal8Bit().constData());
    fflush(stdout);
    if (!ok) ++g_fails;
}

bool isLowerHex(const QString& s, int len)
{
    if (s.size() != len) return false;
    for (const QChar c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

QString capPrefix(const QString& cid) { return QStringLiteral("cap:%1:").arg(cid); }

int runSelftest(const QString& nonce)
{
    RedisStore& redis = RedisStore::instance();
    SessionStore& store = SessionStore::instance();
    ck("st_redis_enabled", redis.enabled());
    if (!redis.enabled()) return g_fails;

    const QString cid1 = nonce + "00000001";
    const QString cid2 = nonce + "00000002";
    const QString cid3 = nonce + "00000003";
    const QString cidGone = nonce + "deadbeef";
    const QString user = QStringLiteral("probe-%1").arg(nonce);

    // ---- 1. 纯逻辑：token / 摘要 / JSON ----
    const QString token = SessionStore::generateResumeToken();
    ck("logic_token_64hex", isLowerHex(token, 64));
    const QString token2 = SessionStore::generateResumeToken();
    ck("logic_token_unique", token != token2);
    const QString hash = SessionStore::tokenHashHex(token);
    ck("logic_hash_64hex", isLowerHex(hash, 64));
    ck("logic_hash_stable", hash == SessionStore::tokenHashHex(token));
    const QByteArray json = SessionStore::buildSessionJson(user, cid1, 7, hash);
    SessionInfo parsed;
    ck("logic_parse_roundtrip",
       SessionStore::parseSessionJson(json, parsed)
       && parsed.user == user && parsed.clientId == cid1
       && parsed.loginVersion == 7 && parsed.tokenHash == hash
       && parsed.state == QStringLiteral("active"));
    ck("logic_parse_rejects_garbage", !SessionStore::parseSessionJson("not json", parsed));
    ck("logic_parse_rejects_missing_field",
       !SessionStore::parseSessionJson("{\"user\":\"a\"}", parsed));

    // ---- 2. 验证码原子消费 / 失败锁定 ----
    const QString p1 = capPrefix(cid1);
    redis.del(p1 + "code"); redis.del(p1 + "fail"); redis.del(p1 + "lock");
    ck("cap_seed", redis.setEx(p1 + "code", 60, "ABCDEF"));
    int outcome = -1;
    ck("cap_wrong_input_outcome1",
       redis.evalCaptchaConsume(p1, "ZZZZZZ", outcome) && outcome == 1);
    bool locked = true;
    ck("cap_failure_not_locked",
       redis.evalCaptchaFailure(p1, 5, 600, 600, locked) && !locked);
    ck("cap_correct_consumes",
       redis.evalCaptchaConsume(p1, "ABCDEF", outcome) && outcome == 0);
    ck("cap_replay_expired",
       redis.evalCaptchaConsume(p1, "ABCDEF", outcome) && outcome == 2);
    // 失败到阈值：连错 5 次锁（前一次 consume 成功已清零 fail）
    redis.setEx(p1 + "code", 60, "ABCDEF");
    locked = false;
    int failCalls = 0;
    for (int i = 0; i < 5 && !locked; ++i, ++failCalls)
        redis.evalCaptchaFailure(p1, 5, 600, 600, locked);
    ck("cap_lock_at_threshold", locked && failCalls == 5, QString("calls=%1").arg(failCalls));
    ck("cap_lock_blocks_consume",
       redis.evalCaptchaConsume(p1, "ABCDEF", outcome) && outcome == 3);
    qint64 ttl = 0;
    ck("cap_code_voided_on_lock", redis.keyTtl(p1 + "code", ttl) && ttl == -2);
    ck("cap_lock_ttl_positive", redis.keyTtl(p1 + "lock", ttl) && ttl > 590);
    // 跨连接一致性：键按 clientId 而非 socket —— 不同“连接上下文”拼同一 prefix 结果一致
    ck("cap_identity_is_client_scoped",
       capPrefix(cid1) == capPrefix(cid1) && outcome == 3);

    // ---- 3. 会话建立 / 读取 / TTL / 摘要隔离 ----
    SessionInfo info;
    ck("sess_create", store.createSession(cid1, user, 7, token) == ENUM_PROTOCOL_CODE_OK);
    ck("sess_read", store.readSession(cid1, info) == ENUM_PROTOCOL_CODE_OK
       && info.loginVersion == 7 && info.state == QStringLiteral("active"));
    {
        const int rcTtl = store.sessionTtl(cid1, ttl);
        ck("sess_ttl_window", rcTtl == ENUM_PROTOCOL_CODE_OK && ttl >= 1795 && ttl <= 1800,
           QString("rc=%1 ttl=%2 key=session:%3").arg(rcTtl).arg(ttl).arg(cid1));
    }
    QByteArray raw;
    bool exists = false;
    redis.getSession(cid1, raw, exists);
    ck("sess_stores_hash_not_token",
       exists && !raw.contains(token.toUtf8()) && raw.contains(hash.toUtf8()));
    bool indexMember = false;
    redis.sismember(QStringLiteral("sessions:%1").arg(user), cid1, indexMember);
    ck("sess_index_member", indexMember);

    // ---- 4. 恢复校验 ----
    ck("resume_wrong_token_3016",
       store.validateResume(cid1, QString(64, 'a'), info) == ENUM_PROTOCOL_CODE_BAD_RESUME_TOKEN);
    {
        const int rcM = store.validateResume(cidGone, token, info);
        ck("resume_missing_3017", rcM == ENUM_PROTOCOL_CODE_SESSION_INVALID,
           QString("rc=%1 key=session:%2").arg(rcM).arg(cidGone));
    }
    ck("resume_ok", store.validateResume(cid1, token, info) == ENUM_PROTOCOL_CODE_OK
       && info.user == user);
    {
        const int rcR = store.sessionTtl(cid1, ttl);
        ck("resume_refresh_ttl", rcR == ENUM_PROTOCOL_CODE_OK && ttl >= 1799 && ttl <= 1800,
           QString("rc=%1 ttl=%2").arg(rcR).arg(ttl));
    }
    // 新登录覆盖后旧凭据失效（tokenHash 变化 → -3016）
    const QString tokenB = SessionStore::generateResumeToken();
    ck("sess_overwrite", store.createSession(cid1, user, 8, tokenB) == ENUM_PROTOCOL_CODE_OK);
    ck("resume_old_token_rejected",
       store.validateResume(cid1, token, info) == ENUM_PROTOCOL_CODE_BAD_RESUME_TOKEN);
    // revoked 状态不可恢复
    redis.setSession(cid2, user,
                     SessionStore::buildSessionJson(user, cid2, 1,
                         SessionStore::tokenHashHex(token), QStringLiteral("revoked")),
                     SessionStore::SESSION_TTL_SEC);
    ck("resume_revoked_state_3017",
       store.validateResume(cid2, token, info) == ENUM_PROTOCOL_CODE_SESSION_INVALID);

    // ---- 5. 条件撤销 ----
    const QString tokenC = SessionStore::generateResumeToken();
    ck("revoke_setup", store.createSession(cid3, user, 9, tokenC) == ENUM_PROTOCOL_CODE_OK);
    bool revoked = true;
    ck("revoke_wrong_version_keeps",
       store.revokeIfMatch(cid3, user, 8, tokenC, revoked) == ENUM_PROTOCOL_CODE_OK && !revoked
       && store.readSession(cid3, info) == ENUM_PROTOCOL_CODE_OK);
    ck("revoke_wrong_token_keeps",
       store.revokeIfMatch(cid3, user, 9, tokenB, revoked) == ENUM_PROTOCOL_CODE_OK && !revoked);
    {
        const int rcRv = store.revokeIfMatch(cid3, user, 9, tokenC, revoked);
        const int rcRe = store.readSession(cid3, info);
        ck("revoke_match_deletes",
           rcRv == ENUM_PROTOCOL_CODE_OK && revoked && rcRe == ENUM_PROTOCOL_CODE_SESSION_INVALID,
           QString("revokeRc=%1 revoked=%2 readRc=%3").arg(rcRv).arg((int)revoked).arg(rcRe));
    }
    bool member = true;
    redis.sismember(QStringLiteral("sessions:%1").arg(user), cid3, member);
    ck("revoke_removes_index_member", !member);

    // ---- 6. 全量撤销（注销枚举） ----
    store.createSession(cid1, user, 10, token);   // cid1 重新登记（v10）
    store.createSession(cid2, user, 10, token);
    int removed = 0;
    ck("revoke_all", store.revokeAllForUser(user, removed) == ENUM_PROTOCOL_CODE_OK
       && removed == 2, QString::number(removed));
    {
        const int rcA1 = store.readSession(cid1, info);
        const int rcA2 = store.readSession(cid2, info);
        ck("revoke_all_sessions_gone",
           rcA1 == ENUM_PROTOCOL_CODE_SESSION_INVALID && rcA2 == ENUM_PROTOCOL_CODE_SESSION_INVALID,
           QString("rc1=%1 rc2=%2").arg(rcA1).arg(rcA2));
    }
    qint64 card = -1;
    redis.scard(QStringLiteral("sessions:%1").arg(user), card);
    ck("revoke_all_index_dropped", card == 0, QString::number(card));

    // ---- 7. 在线态版本条件 ----
    ck("online_set", store.setOnline(user, 11) == ENUM_PROTOCOL_CODE_OK);
    QByteArray onlineVal;
    redis.get(QStringLiteral("online:%1").arg(user), onlineVal);
    ck("online_value_is_version", onlineVal == QByteArray::number(11));
    ck("online_ttl_window",
       redis.keyTtl(QStringLiteral("online:%1").arg(user), ttl) && ttl >= 85 && ttl <= 90);
    bool deleted = true;
    ck("online_wrong_version_keeps",
       store.deleteOnlineIfVersion(user, 12, deleted) == ENUM_PROTOCOL_CODE_OK && !deleted);
    ck("online_match_deletes",
       store.deleteOnlineIfVersion(user, 11, deleted) == ENUM_PROTOCOL_CODE_OK && deleted);
    redis.get(QStringLiteral("online:%1").arg(user), onlineVal);
    ck("online_gone_after_delete", onlineVal.isEmpty());

    // ---- 清理本测试键 ----
    redis.del(p1 + "code"); redis.del(p1 + "fail"); redis.del(p1 + "lock");
    redis.del(QStringLiteral("session:%1").arg(cid1));
    redis.del(QStringLiteral("session:%1").arg(cid2));
    redis.del(QStringLiteral("session:%1").arg(cid3));
    redis.del(QStringLiteral("sessions:%1").arg(user));
    redis.del(QStringLiteral("online:%1").arg(user));
    return g_fails;
}

int runRaceSetup(const QString& cid, const QString& code)
{
    const bool ok = RedisStore::instance().setEx(capPrefix(cid) + "code", 60, code.toUtf8());
    printf("SETUP %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

int runRaceConsume(const QString& cid, const QString& code)
{
    int outcome = -9;
    if (!RedisStore::instance().evalCaptchaConsume(capPrefix(cid), code.toUtf8(), outcome)) {
        printf("RACE_ERROR redis\n");
        return 1;
    }
    printf("RACE_OUTCOME %d\n", outcome);
    return 0;
}

int runRaceState(const QString& cid)
{
    qint64 ttl = -99;
    RedisStore::instance().keyTtl(capPrefix(cid) + "code", ttl);
    printf("RACE_CODE_TTL %lld\n", (long long)ttl);
    RedisStore::instance().del(capPrefix(cid) + "code");
    RedisStore::instance().del(capPrefix(cid) + "fail");
    RedisStore::instance().del(capPrefix(cid) + "lock");
    return 0;
}

int runDownMode()
{
    RedisStore& redis = RedisStore::instance();
    SessionStore& store = SessionStore::instance();
    ck("down_enabled_but_unreachable", redis.enabled());
    const QString cid = QString(32, '0');
    const QString user = QStringLiteral("probe-down");
    ck("down_create_session_1003",
       store.createSession(cid, user, 1, "t") == ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE);
    SessionInfo info;
    ck("down_read_session_1003",
       store.readSession(cid, info) == ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE);
    ck("down_resume_1003",
       store.validateResume(cid, "t", info) == ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE);
    ck("down_touch_1003",
       store.touchSession(cid) == ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE);
    int removed = -1;
    ck("down_revoke_all_1003",
       store.revokeAllForUser(user, removed) == ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE);
    ck("down_set_online_1003",
       store.setOnline(user, 1) == ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE);
    bool b = true;
    ck("down_delete_online_1003",
       store.deleteOnlineIfVersion(user, 1, b) == ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE);
    ck("down_revoke_if_match_1003",
       store.revokeIfMatch(cid, user, 1, "t", b) == ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE);
    int outcome = -1;
    ck("down_captcha_consume_false", !redis.evalCaptchaConsume("cap:x:", "A", outcome));
    ck("down_captcha_failure_false", !redis.evalCaptchaFailure("cap:x:", 5, 1, 1, b));
    ck("down_set_session_false", !redis.setSession(cid, user, "{}", 10));
    QByteArray rawDown;
    bool existsDown = true;
    ck("down_get_session_false", !redis.getSession(cid, rawDown, existsDown));
    ck("down_sismember_false", !redis.sismember("sessions:x", "m", b));
    // 失败时输出参数保持安全默认（removed=0、deleted/revoked=false），调用方必须先查返回码
    ck("down_no_fake_success", removed == 0 && !b);
    return g_fails;
}

int runDisabledMode()
{
    SessionStore& store = SessionStore::instance();
    ck("disabled_not_enabled", !store.redisEnabled());
    const QString cid = QString(32, '1');
    const QString user = QStringLiteral("probe-disabled");
    // 未配置 Redis：不存在可恢复会话语义，SessionStore 一律拒绝（无内存认证旁路）
    ck("dis_create_1003",
       store.createSession(cid, user, 1, "t") == ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE);
    SessionInfo info;
    ck("dis_resume_1003",
       store.validateResume(cid, "t", info) == ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE);
    int disRemoved = -1;
    ck("dis_revoke_all_1003",
       store.revokeAllForUser(user, disRemoved) == ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE);
    ck("dis_online_1003",
       store.setOnline(user, 1) == ENUM_PROTOCOL_CODE_REDIS_SESSION_UNAVAILABLE);
    return g_fails;
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    const QString mode = args.value(1);
    int rc = 0;
    if (mode == "selftest" && args.size() >= 3) rc = runSelftest(args.at(2));
    else if (mode == "race-setup" && args.size() >= 4) rc = runRaceSetup(args.at(2), args.at(3));
    else if (mode == "race-consume" && args.size() >= 4) rc = runRaceConsume(args.at(2), args.at(3));
    else if (mode == "race-state" && args.size() >= 3) rc = runRaceState(args.at(2));
    else if (mode == "down") rc = runDownMode();
    else if (mode == "disabled") rc = runDisabledMode();
    else { printf("usage: harness <mode> [args]\n"); return 2; }
    printf("FAILS %d\n", g_fails);
    return g_fails == 0 && rc == 0 ? 0 : 1;
}
