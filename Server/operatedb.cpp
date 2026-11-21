#include "logging.h"
#include "operatedb.h"
#include "databaseconnectionpool.h"
#include "fileobjectstore.h"
#include "server.h"

#include <QDebug>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegExp>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QVector>

// OperateDB 实现：全部业务 SQL 集中在本文件。每条路径先向连接池借连接（RAII 自动归还），
// 再执行预编译语句。多表写操作使用事务；并发敏感点用行锁（FOR UPDATE）或
// LAST_INSERT_ID 原子分配版本/序号，避免"先读后写"竞态。
// 返回值约定：布尔=false 表示失败；qint64<0 表示失败；字符串空表示未命中。

namespace {
// 生成 32 位 hex 随机盐（熵源为 OS 级密码学安全随机数）
QString generatePasswordSalt()
{
    quint64 entropy[2];
    entropy[0] = QRandomGenerator::system()->generate64();
    entropy[1] = QRandomGenerator::system()->generate64();
    return QString::fromLatin1(
        QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(entropy), sizeof(entropy)),
                                 QCryptographicHash::Sha256).toHex().left(32));
}

// 统一加盐摘要：md5(存储摘要+盐)；REQ-MIG-001 起不再存在空盐老数据分支。
// REQ-PWD-PLAIN-001（密码明文化）：客户端经 TLS 传明文，hashPasswordForStorage 已在
// 内存完成第一层摘要——此处收到的即存储摘要，不再区分"客户端哈希/服务端哈希"。
QString applyPasswordSalt(const QString &passwordHash, const QString &salt)
{
    return QString::fromLatin1(
        QCryptographicHash::hash((passwordHash + salt).toUtf8(), QCryptographicHash::Md5).toHex());
}

// 客户端传明文密码，服务端只在内存中计算摘要后再加盐。
QString hashPasswordForStorage(const QString &password)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Md5).toHex());
}
}

// 单例由静态局部对象持有，连接池随进程退出统一关闭
OperateDB::~OperateDB()
{
}

// 进程内唯一实例；首次调用时线程安全构造
OperateDB &OperateDB::instance()
{
    static OperateDB s;
    return s;
}

// 无额外初始化：连接参数延迟到 connect() 时从 Server 配置读取
OperateDB::OperateDB(QObject *parent) : QObject(parent)
{
}

// 用 Server 的数据库配置初始化全局连接池，并立即做一次连通性探测。
// 探测失败只记 critical 日志，不退出——由 main 的 schema 检查/业务调用决定后续行为。
void OperateDB::connect()
{
    Server& server = Server::getinstance();
    DatabaseConnectionPool::instance().init(
        server.m_dbHost,
        server.m_dbPort,
        server.m_dbUser,
        server.m_dbPassword,
        server.m_dbName
    );
    bool ok = DatabaseConnectionPool::instance().testConnection();
    if (ok) {
        qCDebug(lcDb) << "OperateDB::connect - 数据库连接池初始化成功";
    } else {
        qCCritical(lcDb) << "OperateDB::connect - 数据库连接池初始化失败: testConnection 返回 false";
    }
}

// REQ-EXT-FILE-001 T1：启动 schema 版本检查（D1 fail-fast：库不可达/表缺失/版本低一律拒绝启动，
// 不静默建表、不自动执行迁移——迁移只由 migrate_ext_file_001.sql 人工受控执行）。
bool OperateDB::checkSchemaVersion()
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) {
        qCCritical(lcDb) << "[checkSchemaVersion] 数据库不可达：请先检查 connect.config 与 MySQL 服务";
        return false;
    }
    QSqlQuery q(connection.database());
    if (!q.exec("SELECT MAX(version) FROM schema_version")) {
        qCCritical(lcDb) << "[checkSchemaVersion] 读取 schema_version 失败（表不存在？）:"
                         << q.lastError().text()
                         << "— 请先执行 Server/migrate_ext_file_001.sql、migrate_ext_group_001.sql、"
                            "migrate_ext_group_002.sql、migrate_ext_file_002.sql、migrate_ext_group_003.sql、"
                            "migrate_ext_reclaim_001.sql、migrate_friend_req_001.sql、"
                            "migrate_dir_comp_001.sql 与 migrate_share_entryid_001.sql";
        return false;
    }
    int version = 0;
    if (q.next() && !q.value(0).isNull()) {
        version = q.value(0).toInt();
    }
    // 总门槛取全部迁移版本的最大值：任一迁移缺失都会使库版本低于该值
    const int required = qMax(kFileAssetSchemaVersion,
                              qMax(kGroupSchemaVersion,
                                   qMax(kGroupMessageSchemaVersion,
                                        qMax(kFastUploadSchemaVersion,
                                             qMax(kGroupFileSchemaVersion,
                                                  qMax(kReclaimSchemaVersion,
                                                       qMax(kFriendRequestSchemaVersion,
                                                            qMax(kDirectoryCompensationSchemaVersion,
                                                                 kShareEntryIdSchemaVersion))))))));
    if (version < required) {
        qCCritical(lcDb) << "[checkSchemaVersion] schema 版本" << version << "低于要求的" << required
                         << "— 全新库请按 README 顺序执行 init_test_data.sql 与 Server/migrate_*.sql"
                            "（share_link 新表由 Server/share_link.sql 建立）；已装旧结构库请执行"
                            " Server/migrate_share_entryid_001.sql 完成分享链接改列升级";
        return false;
    }
    qCDebug(lcDb) << "[checkSchemaVersion] OK, version" << version;
    return true;
}

// ===== REQ-DIR-COMP-001：目录操作失败补偿队列 =====
bool OperateDB::enqueueDirectoryCompensation(const QString& operationType,
                                             const QString& account,
                                             const QString& sourcePath,
                                             const QString& targetPath,
                                             qint64 entryId,
                                             const QString& sourceName,
                                             const QString& targetName,
                                             qint64 operationVersion,
                                             const QString& error)
{
    if (operationType.trimmed().isEmpty() || account.trimmed().isEmpty()) return false;
    if (sourcePath.contains("..") || targetPath.contains("..")
        || sourcePath.startsWith('/') || targetPath.startsWith('/')) return false;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return false;
    const QByteArray fingerprint = (operationType + QChar('\x1f') + account + QChar('\x1f')
                                    + sourcePath + QChar('\x1f') + targetPath + QChar('\x1f')
                                    + QString::number(entryId) + QChar('\x1f')
                                    + sourceName + QChar('\x1f') + targetName + QChar('\x1f')
                                    + QString::number(operationVersion)).toUtf8();
    const QString dedupeKey = QString::fromLatin1(
        QCryptographicHash::hash(fingerprint, QCryptographicHash::Sha256).toHex());
    QSqlQuery q(connection.database());
    q.prepare("INSERT INTO directory_operation_compensation "
              "(dedupe_key,operation_type,account,source_path,target_path,entry_id,"
              "source_name,target_name,operation_version,status,retry_count,next_retry_at,last_error) "
              "VALUES (?,?,?,?,?,?, ?,?,?, 'pending',0,NOW(),?) "
              "ON DUPLICATE KEY UPDATE last_error=VALUES(last_error),updated_at=NOW()");
    q.addBindValue(dedupeKey);
    q.addBindValue(operationType);
    q.addBindValue(account);
    q.addBindValue(sourcePath.isEmpty() ? QVariant() : QVariant(sourcePath));
    q.addBindValue(targetPath.isEmpty() ? QVariant() : QVariant(targetPath));
    q.addBindValue(entryId > 0 ? QVariant(entryId) : QVariant());
    q.addBindValue(sourceName.isEmpty() ? QVariant() : QVariant(sourceName));
    q.addBindValue(targetName.isEmpty() ? QVariant() : QVariant(targetName));
    q.addBindValue(operationVersion);
    q.addBindValue(error.left(1024));
    if (!q.exec()) {
        qCWarning(lcDb) << "[dir-comp] 入队失败:" << q.lastError().text()
                        << "type=" << operationType << "account=" << account;
        return false;
    }
    return true;
}

QVector<OperateDB::DirectoryCompensationTask>
OperateDB::claimDirectoryCompensations(const QString& claimOwner, int leaseSeconds, int limit)
{
    QVector<DirectoryCompensationTask> out;
    if (claimOwner.trimmed().isEmpty() || leaseSeconds <= 0 || limit <= 0) return out;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return out;
    QSqlDatabase db = connection.database();
    if (!db.transaction()) return out;
    QSqlQuery q(db);
    const QString sql = QStringLiteral(
        "SELECT task_id,operation_type,account,COALESCE(source_path,''),COALESCE(target_path,''),"
        "COALESCE(entry_id,0),COALESCE(source_name,''),COALESCE(target_name,''),operation_version,"
        "retry_count,next_retry_at,COALESCE(last_error,''),COALESCE(claim_owner,''),claim_until "
        "FROM directory_operation_compensation "
        "WHERE (status='pending' AND next_retry_at<=NOW()) "
        "OR (status='claimed' AND claim_until<NOW()) "
        "ORDER BY next_retry_at ASC,created_at ASC LIMIT %1 FOR UPDATE").arg(limit);
    if (!q.exec(sql)) {
        db.rollback();
        qCWarning(lcDb) << "[dir-comp] 抢占查询失败:" << q.lastError().text();
        return out;
    }
    QVector<qint64> ids;
    while (q.next()) {
        DirectoryCompensationTask task;
        task.taskId = q.value(0).toLongLong();
        task.operationType = q.value(1).toString();
        task.account = q.value(2).toString();
        task.sourcePath = q.value(3).toString();
        task.targetPath = q.value(4).toString();
        task.entryId = q.value(5).toLongLong();
        task.sourceName = q.value(6).toString();
        task.targetName = q.value(7).toString();
        task.operationVersion = q.value(8).toLongLong();
        task.retryCount = q.value(9).toInt();
        task.nextRetryAt = q.value(10).toDateTime();
        task.lastError = q.value(11).toString();
        task.claimOwner = claimOwner;
        ids.append(task.taskId);
        out.append(task);
    }
    for (qint64 id : ids) {
        QSqlQuery u(db);
        u.prepare("UPDATE directory_operation_compensation SET status='claimed',claim_owner=?,"
                  "claim_until=DATE_ADD(NOW(),INTERVAL ? SECOND),updated_at=NOW() "
                  "WHERE task_id=? AND (status='pending' OR claim_until<NOW())");
        u.addBindValue(claimOwner);
        u.addBindValue(leaseSeconds);
        u.addBindValue(id);
        if (!u.exec() || u.numRowsAffected() != 1) {
            db.rollback();
            out.clear();
            return out;
        }
    }
    if (!db.commit()) {
        out.clear();
        return out;
    }
    return out;
}

QVector<OperateDB::DirectoryCompensationTask>
OperateDB::peekDirectoryCompensations(int limit)
{
    QVector<DirectoryCompensationTask> out;
    if (limit <= 0) return out;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return out;
    QSqlQuery q(connection.database());
    const QString sql = QStringLiteral(
        "SELECT task_id,operation_type,account,COALESCE(source_path,''),COALESCE(target_path,''),"
        "COALESCE(entry_id,0),COALESCE(source_name,''),COALESCE(target_name,''),operation_version,"
        "retry_count,next_retry_at,COALESCE(last_error,''),COALESCE(claim_owner,''),claim_until "
        "FROM directory_operation_compensation WHERE status='pending' AND next_retry_at<=NOW() "
        "ORDER BY next_retry_at ASC,created_at ASC LIMIT %1").arg(limit);
    if (!q.exec(sql)) return out;
    while (q.next()) {
        DirectoryCompensationTask task;
        task.taskId = q.value(0).toLongLong();
        task.operationType = q.value(1).toString();
        task.account = q.value(2).toString();
        task.sourcePath = q.value(3).toString();
        task.targetPath = q.value(4).toString();
        task.entryId = q.value(5).toLongLong();
        task.sourceName = q.value(6).toString();
        task.targetName = q.value(7).toString();
        task.operationVersion = q.value(8).toLongLong();
        task.retryCount = q.value(9).toInt();
        task.nextRetryAt = q.value(10).toDateTime();
        task.lastError = q.value(11).toString();
        out.append(task);
    }
    return out;
}

bool OperateDB::completeDirectoryCompensation(qint64 taskId, const QString& claimOwner)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("DELETE FROM directory_operation_compensation WHERE task_id=? AND status='claimed' AND claim_owner=?");
    q.addBindValue(taskId);
    q.addBindValue(claimOwner);
    return q.exec() && q.numRowsAffected() == 1;
}

bool OperateDB::retryDirectoryCompensation(qint64 taskId, const QString& claimOwner,
                                           const QDateTime& nextRetryAt, const QString& error)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("UPDATE directory_operation_compensation SET status='pending',claim_owner=NULL,"
              "claim_until=NULL,retry_count=retry_count+1,next_retry_at=?,last_error=?,updated_at=NOW() "
              "WHERE task_id=? AND status='claimed' AND claim_owner=?");
    q.addBindValue(nextRetryAt);
    q.addBindValue(error.left(1024));
    q.addBindValue(taskId);
    q.addBindValue(claimOwner);
    return q.exec() && q.numRowsAffected() == 1;
}
// ===== REQ-EXT-SESSION-002：启动期在线态对账 =====
// 为什么需要：online 的唯一权威来源是本进程的活连接——置 1 在 handleLogin，置 0 只在
// handleOffLine。服务端被 kill / 崩溃 / 断电时，当时在线的人没有任何机会走 handleOffLine，
// 于是库里永久留下 online=1 的幽灵：在线列表与用户信息把它们当在线返回，"加好友"还会
// 朝幽灵转发请求（无接收者、静默丢弃）。只有该用户自己重新登录再断开才会自愈。
// 为什么可以全量清零：启动瞬间本进程一条连接都没有，任何 online=1 都必然是上个进程的残留；
// 而"断开即归零"由 handleOffLine 保证，故清零后不会冤枉任何真实在线的会话。
// 单实例假设（重要）：本项目是单进程单端口单库。若将来多实例共享同一库，全量清零会误清
// 另一个实例正在服务的在线用户——那时必须改成"按实例/租约"判活，不能沿用本实现。
bool OperateDB::reconcileOnlineFlags()
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) {
        qCCritical(lcDb) << "[reconcileOnline] 数据库不可达：无法对账上一进程遗留的在线态";
        return false;
    }
    QSqlQuery q(connection.database());
    if (!q.exec("SELECT COUNT(*) FROM user_info WHERE online <> 0")) {
        qCCritical(lcDb) << "[reconcileOnline] 统计遗留在线态失败:" << q.lastError().text();
        return false;
    }
    qint64 stale = 0;
    if (q.next()) {
        stale = q.value(0).toLongLong();
    }
    if (stale == 0) {
        qCDebug(lcDb) << "[reconcileOnline] 无遗留在线态，跳过";
        return true;
    }
    QSqlQuery upd(connection.database());
    if (!upd.exec("UPDATE user_info SET online = 0 WHERE online <> 0")) {
        qCCritical(lcDb) << "[reconcileOnline] 归零失败:" << upd.lastError().text();
        return false;
    }
    qCWarning(lcDb) << "[reconcileOnline] 已把上一进程遗留的在线态归零，条数=" << stale
                    << "（服务端异常终止后客户端没机会走 handleOffLine，属预期残留）";
    return true;
}

// ===== REQ-EXT-FILE-001 T4：upload_session 持久化（staging 归属/offset/过期）=====
qint64 OperateDB::createUploadSession(const QString &owner, const QString &parentPath,
                                      const QString &displayName, qint64 expectedSize,
                                      const QString &clientSha256, const QString &stagingPath,
                                      qint64 confirmedOffset, int ttlSec,
                                      const QString &clientRequestId)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return -1;
    QSqlQuery q(connection.database());
    // 新会话状态固定 uploading；expires_at 默认 1 小时后
    q.prepare("INSERT INTO upload_session (owner_name,parent_path,display_name,expected_size,"
              "client_sha256,staging_path,confirmed_offset,status,expires_at,client_request_id) "
              "VALUES (?,?,?,?,?,?,?,'uploading',?,?)");
    q.addBindValue(owner);
    q.addBindValue(parentPath);
    q.addBindValue(displayName);
    q.addBindValue(expectedSize);
    q.addBindValue(clientSha256.isEmpty() ? QVariant() : QVariant(clientSha256));
    q.addBindValue(stagingPath);
    q.addBindValue(confirmedOffset);
    q.addBindValue(QDateTime::currentDateTime().addSecs(ttlSec > 0 ? ttlSec : 3600)
                       .toString("yyyy-MM-dd HH:mm:ss"));
    q.addBindValue(clientRequestId.isEmpty() ? QVariant() : QVariant(clientRequestId));
    if(!q.exec()){
        qCWarning(lcDb) << "[createUploadSession] 失败:" << q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

// REQ-EXT-FILE-002 §2 步骤3：请求级幂等短路的读侧。
// 展示名优先取 user_file（服务端定名含 (1) 后缀），退化到会话登记的请求名。
OperateDB::UploadSessionLookup OperateDB::lookupUploadSessionByRequestId(
        const QString &owner, const QString &clientRequestId)
{
    UploadSessionLookup out;
    if (owner.isEmpty() || clientRequestId.isEmpty()) return out;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return out;
    QSqlQuery q(connection.database());
    // FOR UPDATE 锁住会话行，使并发同请求键的重试串行读取
    q.prepare("SELECT s.session_id,s.status,s.confirmed_offset,s.file_id,s.display_name,"
              "       uf.display_name,s.parent_path,s.expected_size,s.client_sha256 "
              "FROM upload_session s LEFT JOIN user_file uf ON uf.file_id=s.file_id "
              "WHERE s.owner_name=? AND s.client_request_id=? FOR UPDATE");
    q.addBindValue(owner);
    q.addBindValue(clientRequestId);
    if (!q.exec() || !q.next()) return out;
    out.found = true;
    out.sessionId = q.value(0).toLongLong();
    out.status = q.value(1).toString();
    out.confirmedOffset = q.value(2).toLongLong();
    out.fileId = q.value(3).isNull() ? -1 : q.value(3).toLongLong();
    out.displayName = q.value(5).isNull() ? q.value(4).toString() : q.value(5).toString();
    out.parentPath = q.value(6).toString();
    out.expectedSize = q.value(7).toLongLong();
    out.clientSha256 = q.value(8).toString();
    return out;
}

// REQ-EXT-FILE-002 §2 步骤3：失败/过期会话的"同请求键重试"——原地重置，
// 绝不再插一行（uk_req 保证 (owner, client_request_id) 唯一）。
bool OperateDB::restartUploadSession(qint64 sessionId, const QString &parentPath,
                                     const QString &displayName, qint64 expectedSize,
                                     const QString &clientSha256, const QString &stagingPath,
                                     qint64 confirmedOffset, int ttlSec,
                                     const QString &clientRequestId)
{
    if (sessionId < 0) return false;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    // REQ-RESUME-AUDIT-001 M2：摘要为空时跳过 client_sha256 列——29/101 复用路径不带摘要，
    // 不能把首次 97 登记的值抹成 NULL（否则续传转正退化为不校验）。
    const bool hasSha = !clientSha256.isEmpty();
    QString sql = "UPDATE upload_session SET parent_path=?,display_name=?,expected_size=?,";
    if (hasSha) {
        sql += "client_sha256=?,";
    }
    sql += "staging_path=?,confirmed_offset=?,status='uploading',"
           "expires_at=?,file_id=NULL";
    if (!clientRequestId.isEmpty()) {
        sql += ",client_request_id=?";   // S14：按暂存键复用时把请求键改写到本行（任何 reqId 重放都收敛于此）
    }
    sql += " WHERE session_id=?";
    q.prepare(sql);
    q.addBindValue(parentPath);
    q.addBindValue(displayName);
    q.addBindValue(expectedSize);
    if (hasSha) {
        q.addBindValue(clientSha256);
    }
    q.addBindValue(stagingPath);
    q.addBindValue(confirmedOffset);
    q.addBindValue(QDateTime::currentDateTime().addSecs(ttlSec > 0 ? ttlSec : 3600)
                       .toString("yyyy-MM-dd HH:mm:ss"));
    if (!clientRequestId.isEmpty()) {
        q.addBindValue(clientRequestId);
    }
    q.addBindValue(sessionId);
    if (!q.exec()) {
        qCWarning(lcDb) << "[restartUploadSession] 失败:" << q.lastError().text();
        return false;
    }
    return true;
}

// REQ-EXT-TRANSFER-003 S12/D7：按 (owner, staging_path) 找可复用的未完成会话行。
// 暂存路径 = sha1(owner|parent|display|size) 的确定性产物 = "同一逻辑上传"的唯一身份；
// 状态白名单排除 done（已完成由请求级幂等短路处理），取 session_id 最大者。
// REQ-RESUME-AUDIT-001 M1/M2：返回结构体——confirmed_offset 供续传起点封顶，
// client_sha256 供 29/101 复用路径回填内存摘要（转正比对读内存副本，不读该列）。
OperateDB::ReusableUploadSession OperateDB::findReusableUploadSession(const QString &owner,
                                                                      const QString &stagingPath)
{
    ReusableUploadSession out;
    if (owner.isEmpty() || stagingPath.isEmpty()) return out;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return out;
    QSqlQuery q(connection.database());
    q.prepare("SELECT session_id,confirmed_offset,client_sha256 FROM upload_session "
              "WHERE owner_name=? AND staging_path=? "
              "AND status IN ('uploading','expired','failed','verifying') "
              "ORDER BY session_id DESC LIMIT 1");
    q.addBindValue(owner);
    q.addBindValue(stagingPath);
    if (!q.exec() || !q.next()) return out;
    out.sessionId = q.value(0).toLongLong();
    out.confirmedOffset = q.value(1).toLongLong();
    out.clientSha256 = q.value(2).toString();
    return out;
}

// REQ-EXT-FILE-002 §2 步骤4：秒传命中即登记完成态会话（零正文传输）。
// staging_path 空串表示本次请求没有暂存实体（未落任何碎片文件）。
qint64 OperateDB::insertCompletedUploadSession(const QString &owner, const QString &parentPath,
                                              const QString &displayName, qint64 expectedSize,
                                              const QString &clientSha256,
                                              const QString &clientRequestId, qint64 fileId)
{
    if (owner.isEmpty() || clientRequestId.isEmpty() || fileId < 0) return -1;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return -1;
    QSqlQuery q(connection.database());
    q.prepare("INSERT INTO upload_session (owner_name,parent_path,display_name,expected_size,"
              "client_sha256,staging_path,confirmed_offset,status,expires_at,client_request_id,file_id) "
              "VALUES (?,?,?,?,?,'',?,'done',?,?,?)");
    q.addBindValue(owner);
    q.addBindValue(parentPath);
    q.addBindValue(displayName);
    q.addBindValue(expectedSize);
    q.addBindValue(clientSha256.isEmpty() ? QVariant() : QVariant(clientSha256));
    q.addBindValue(expectedSize);                        // confirmed_offset = size（已完成）
    q.addBindValue(QDateTime::currentDateTime().addSecs(3600).toString("yyyy-MM-dd HH:mm:ss"));
    q.addBindValue(clientRequestId);
    q.addBindValue(fileId);
    if (!q.exec()) {
        qCWarning(lcDb) << "[insertCompletedUploadSession] 失败:" << q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

// 更新已确认偏移；只有 status='uploading' 的行会命中。
// 返回 SQL 是否执行成功——会话不存在/状态不符时影响 0 行也返回 true。
bool OperateDB::updateUploadOffset(qint64 sessionId, qint64 confirmedOffset)
{
    if(sessionId < 0) return false;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("UPDATE upload_session SET confirmed_offset=? WHERE session_id=? AND status='uploading'");
    q.addBindValue(confirmedOffset);
    q.addBindValue(sessionId);
    return q.exec();
}

// REQ-EXT-TRANSFER-003 S10：confirmedOffset>=0 时同写 confirmed_offset——
// 完成路径写 expected_size（完成态不依赖节流），失败路径写 0（暂存已删，值无意义）。
bool OperateDB::finishUploadSession(qint64 sessionId, const QString &status, qint64 fileId,
                                    qint64 confirmedOffset)
{
    if(sessionId < 0) return false;
    if(status != "done" && status != "failed" && status != "expired" && status != "verifying")
        return false;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    if (fileId >= 0 && confirmedOffset >= 0) {
        q.prepare("UPDATE upload_session SET status=?,file_id=?,confirmed_offset=? WHERE session_id=?");
        q.addBindValue(status);
        q.addBindValue(fileId);
        q.addBindValue(confirmedOffset);
    } else if (fileId >= 0) {
        q.prepare("UPDATE upload_session SET status=?,file_id=? WHERE session_id=?");
        q.addBindValue(status);
        q.addBindValue(fileId);
    } else if (confirmedOffset >= 0) {
        q.prepare("UPDATE upload_session SET status=?,confirmed_offset=? WHERE session_id=?");
        q.addBindValue(status);
        q.addBindValue(confirmedOffset);
    } else {
        q.prepare("UPDATE upload_session SET status=? WHERE session_id=?");
        q.addBindValue(status);
    }
    q.addBindValue(sessionId);
    return q.exec();
}

// 注册：先查重再插入，两步非事务（并发同名可能由唯一键兜底失败）。
// 客户端传明文密码，服务端只保存加盐摘要，明文密码不落库。
// 返回 true 仅表示 INSERT 成功。
bool OperateDB::handleRegit(char *strname, char *strpwd)
{
    if(strname==nullptr||strpwd==nullptr){
        return false;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("select * from user_info where name = ?");
    q.addBindValue(QString::fromUtf8(strname));
    qCDebug(lcDb)<<"query user exist for:"<<QString::fromUtf8(strname);
    if(!q.exec()){
        qCWarning(lcDb)<<"query user exist failed:"<<q.lastError().text();
        return false;
    }
    if(q.next()){
        return false;
    }

    const QString salt = generatePasswordSalt();
    q.prepare("insert into user_info(name,pwd,salt) values(?,?,?)");
    q.addBindValue(QString::fromUtf8(strname));
    q.addBindValue(applyPasswordSalt(hashPasswordForStorage(QString::fromUtf8(strpwd)), salt));
    q.addBindValue(salt);
    qCDebug(lcDb)<<"insert new user:"<<QString::fromUtf8(strname);
    return q.exec();
}

// 创建安全外链：同 owner+path 已存在则复用（返回 true，不更新令牌/有效期）。
// token_hash 唯一性由表约束保证；expire 显式格式化为 MySQL DATETIME。
bool OperateDB::createShareLink(const QString &hash, const QString &owner, qint64 fileId, const QDateTime &expire)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        qCWarning(lcDb) << "[createShareLink] 获取数据库连接失败（连接无效）";
        return false;
    }
    QSqlQuery q(connection.database());
    // 查重：同一 owner + 同一条目若已存在链接，则直接复用，避免重复记录堆积
    if(!getShareLinkByFile(owner, fileId).isEmpty()){
        qCDebug(lcDb) << "[createShareLink] 该条目已存在分享链接，复用已有记录: owner=" << owner << " fileId=" << fileId;
        return true;
    }
    q.prepare("INSERT INTO share_link (token_hash, owner_name, file_id, expire_time) VALUES (?, ?, ?, ?)");
    q.addBindValue(hash);
    q.addBindValue(owner);
    q.addBindValue(fileId);
    // 必须显式格式化为 "yyyy-MM-dd HH:mm:ss" 字符串再绑定。
    // 直接绑定 QDateTime 时，Qt MySQL 驱动会序列化成 ISO 8601 格式（带 'T' 分隔符，如
    // 2026-07-29T18:00:00），若时间含毫秒/时区，MySQL 可能拒绝或存错。
    // Qt 5.14 的 QMYSQL 驱动在绑定 QDateTime 时会输出 ISO 格式（带 T / 毫秒），
    // MySQL 的 DATETIME 在严格模式下会拒绝带 T 的值，导致插入失败。
    // 这里显式格式化为 MySQL 原生 DATETIME 字符串，避免该问题。
    q.addBindValue(expire.toString("yyyy-MM-dd HH:mm:ss"));
    if(!q.exec()){
        qCWarning(lcDb) << "[createShareLink] 插入分享链接失败:" << q.lastError().text()
                 << " | hash=" << hash << " owner=" << owner << " fileId=" << fileId;
        return false;
    }
    return true;
}

// 按令牌哈希取链接信息；命中时回填 owner/条目标识/过期时间/撤销位，未命中返回 false
bool OperateDB::getShareLinkByHash(const QString &hash, ShareLinkInfo &outInfo)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("SELECT owner_name, file_id, expire_time, revoked FROM share_link WHERE token_hash = ?");
    q.addBindValue(hash);
    if(q.exec() && q.next()){
        outInfo.ownerName = q.value(0).toString();
        outInfo.fileId = q.value(1).toLongLong();
        outInfo.expireTime = q.value(2).toDateTime();
        outInfo.revoked = q.value(3).toInt();
        return true;
    }
    return false;
}

// 按 owner+条目标识取最新 token_hash；未命中返回空串（调用方据此判断"无链接"）
QString OperateDB::getShareLinkByFile(const QString &owner, qint64 fileId)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return QString();
    QSqlQuery q(connection.database());
    // created_at 是秒精度，同秒两行时排序不稳定；以 id 兜底，保证"同一逻辑行"在
    // getShareLinkByFile / getShareLinkInfoByFile 两个入口上取值一致
    q.prepare("SELECT token_hash FROM share_link WHERE owner_name = ? AND file_id = ? "
              "ORDER BY created_at DESC, id DESC LIMIT 1");
    q.addBindValue(owner);
    q.addBindValue(fileId);
    if(q.exec() && q.next()){
        return q.value(0).toString();
    }
    return QString();
}

// 按 owner+条目标识取最新一行的完整信息（getShareLinkByFile 只回 hash，
// 判断"有效/过期/撤销"还需要 expire_time 与 revoked）
bool OperateDB::getShareLinkInfoByFile(const QString &owner, qint64 fileId, ShareLinkInfo &outInfo)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("SELECT token_hash, expire_time, revoked FROM share_link "
              "WHERE owner_name = ? AND file_id = ? ORDER BY created_at DESC, id DESC LIMIT 1");
    q.addBindValue(owner);
    q.addBindValue(fileId);
    if(q.exec() && q.next()){
        outInfo.ownerName = owner;
        outInfo.fileId = fileId;
        outInfo.tokenHash = q.value(0).toString();
        outInfo.expireTime = q.value(1).toDateTime();
        outInfo.revoked = q.value(2).toInt() != 0;
        return true;
    }
    return false;
}

// 续期只延长 expire_time，token 与条目标识不动；
// 已撤销的行（revoked=1）不参与，续期不能让一条被作废的链接复活
bool OperateDB::renewShareLink(const QString &owner, qint64 fileId, const QDateTime &newExpire)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("UPDATE share_link SET expire_time = ? WHERE owner_name = ? AND file_id = ? AND revoked = 0");
    q.addBindValue(newExpire.toString("yyyy-MM-dd HH:mm:ss"));
    q.addBindValue(owner);
    q.addBindValue(fileId);
    if(!q.exec()){
        qCWarning(lcDb) << "[renewShareLink] 续期失败:" << q.lastError().text()
                        << " owner=" << owner << " fileId=" << fileId;
        return false;
    }
    return q.numRowsAffected() > 0;
}

// REQ-AUDIT-001 R6：作废旧链接 + 写入新链接同一事务。
// 任一步失败整体回滚，旧链接保持可用（对齐"重新生成要么换掉、要么保持原样"）。
bool OperateDB::replaceShareLink(const QString &hash, const QString &owner, qint64 fileId,
                                 const QDateTime &expire)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlDatabase db = connection.database();
    if(!db.transaction()){
        qCWarning(lcDb) << "[replaceShareLink] 事务开启失败:" << db.lastError().text();
        return false;
    }
    QSqlQuery del(db);
    del.prepare("DELETE FROM share_link WHERE owner_name = ? AND file_id = ?");
    del.addBindValue(owner);
    del.addBindValue(fileId);
    if(!del.exec()){
        qCWarning(lcDb) << "[replaceShareLink] 作废旧链接失败:" << del.lastError().text();
        db.rollback();
        return false;
    }
    QSqlQuery ins(db);
    ins.prepare("INSERT INTO share_link (token_hash, owner_name, file_id, expire_time) VALUES (?, ?, ?, ?)");
    ins.addBindValue(hash);
    ins.addBindValue(owner);
    ins.addBindValue(fileId);
    ins.addBindValue(expire.toString("yyyy-MM-dd HH:mm:ss"));
    if(!ins.exec()){
        qCWarning(lcDb) << "[replaceShareLink] 写入新链接失败（已回滚，旧链接保留）:" << ins.lastError().text()
                        << " owner=" << owner;
        db.rollback();
        return false;
    }
    if(!db.commit()){
        qCWarning(lcDb) << "[replaceShareLink] 提交失败:" << db.lastError().text();
        db.rollback();
        return false;
    }
    return true;
}

// 跨功能联动（REQ-SHARE-ENTRYID-001）：条目删除时按条目标识清理对应链接记录。
// 重命名/移动不再联动——链接按条目标识定位，改名/移动不影响解析。
// REQ-AUDIT-001 R7：事务内删除单条目的链接记录（调用方负责提交/回滚）
bool OperateDB::deleteShareLinkByFileTx(QSqlDatabase &db, const QString &owner, qint64 fileId)
{
    QSqlQuery q(db);
    q.prepare("DELETE FROM share_link WHERE owner_name = ? AND file_id = ?");
    q.addBindValue(owner);
    q.addBindValue(fileId);
    bool ok = q.exec();
    if(!ok){
        qCWarning(lcDb) << "[deleteShareLinkByFileTx] 失败:" << q.lastError().text()
                 << " owner=" << owner << " fileId=" << fileId;
    }
    return ok;
}

// 事务内按条目标识集合清理（目录子树软删用；空集合=无链接可清，直接成功）
bool OperateDB::deleteShareLinkByFileIdsTx(QSqlDatabase &db, const QString &owner,
                                           const QList<qint64> &fileIds)
{
    if(fileIds.isEmpty()) return true;
    QStringList placeholders;
    for(int i = 0; i < fileIds.size(); ++i) placeholders << QStringLiteral("?");
    QSqlQuery q(db);
    q.prepare(QString("DELETE FROM share_link WHERE owner_name = ? AND file_id IN (%1)")
                  .arg(placeholders.join(QStringLiteral(","))));
    q.addBindValue(owner);
    for(qint64 fid : fileIds) q.addBindValue(fid);
    bool ok = q.exec();
    if(!ok){
        qCWarning(lcDb) << "[deleteShareLinkByFileIdsTx] 失败:" << q.lastError().text()
                 << " owner=" << owner << " count=" << fileIds.size();
    }
    return ok;
}

// 跨功能联动：删除好友时，清理两人互发的私聊记录
bool OperateDB::deleteFriendMessages(const QString &userA, const QString &userB)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("DELETE FROM message_info WHERE (seed_name = ? AND rev_name = ?) OR (seed_name = ? AND rev_name = ?)");
    q.addBindValue(userA);
    q.addBindValue(userB);
    q.addBindValue(userB);
    q.addBindValue(userA);
    bool ok = q.exec();
    if(!ok){
        qCWarning(lcDb) << "[deleteFriendMessages] 失败:" << q.lastError().text()
                 << " a=" << userA << " b=" << userB;
    }
    return ok;
}

// REQ-AUDIT-FULL-001：删除好友与清理双方私聊记录同事务。
// 改前 handleDeleteFriend 提交后另起连接 deleteFriendMessages，消息清理失败不回滚，
// 可能留下"非好友但历史仍在"的窗口。任一步失败整体回滚，不会只删一半。
bool OperateDB::deleteFriendCascade(const QString &userA, const QString &userB)
{
    if(userA.isEmpty() || userB.isEmpty() || userA == userB){
        return false;
    }
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();
    if(!db.transaction()){
        qCWarning(lcDb)<<"deleteFriendCascade 事务开启失败:"<<db.lastError().text();
        return false;
    }
    QSqlQuery q(db);
    q.prepare("DELETE FROM friend WHERE (name = ? AND friendName = ?) OR (name = ? AND friendName = ?)");
    q.addBindValue(userA);
    q.addBindValue(userB);
    q.addBindValue(userB);
    q.addBindValue(userA);
    if(!q.exec()){
        qCWarning(lcDb)<<"deleteFriendCascade 删好友失败:"<<q.lastError().text();
        db.rollback();
        return false;
    }
    q.prepare("DELETE FROM message_info WHERE (seed_name = ? AND rev_name = ?) OR (seed_name = ? AND rev_name = ?)");
    q.addBindValue(userA);
    q.addBindValue(userB);
    q.addBindValue(userB);
    q.addBindValue(userA);
    if(!q.exec()){
        qCWarning(lcDb)<<"deleteFriendCascade 清聊天记录失败:"<<q.lastError().text();
        db.rollback();
        return false;
    }
    if(!db.commit()){
        qCWarning(lcDb)<<"deleteFriendCascade 提交失败:"<<db.lastError().text();
        db.rollback();
        return false;
    }
    return true;
}



// 登录：校验 md5(客户端哈希+盐) 后原子分配 login_version、写登录档案并置 online=1。
// 返回 1 成功 / 0 系统失败（连接、事务、SQL）/ -1 用户名或密码错误。
// 成功时 outVersion 为本次事务内分配的版本号；失败时保证为 0。
int OperateDB::handleLogin(char *strname, char *strpwd, const QString& clientId, quint64& outVersion)
{
    outVersion = 0;
    if(strname==nullptr||strpwd==nullptr){
        return 0;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return 0;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    QString nameParam = QString::fromUtf8(strname);
    const QString passwordHash = hashPasswordForStorage(QString::fromUtf8(strpwd));

    // 按用户名取存储哈希与盐
    q.prepare("SELECT pwd, salt FROM user_info WHERE name = ?");
    q.addBindValue(nameParam);
    qCDebug(lcDb)<<"login select for:"<<nameParam;
    if(!q.exec()){
        qCWarning(lcDb)<<"login select failed:"<<q.lastError().text();
        return 0;
    }
    if(!q.next()){
        qCWarning(lcDb)<<"user name or password not found";
        return -1;
    }
    const QString storedPwd = q.value(0).toString();
    const QString salt = q.value(1).toString();
    const QString effectiveHash = applyPasswordSalt(passwordHash, salt);
    if(effectiveHash != storedPwd){
        qCWarning(lcDb)<<"user name or password not found";
        return -1;
    }

    // ===== REQ-EXT-SESSION-001：登录版本原子分配 + 登录档案 + 在线置位，同一事务 =====
    // LAST_INSERT_ID(login_version+1)：行锁串行化并发登录，版本号唯一单调；禁止读后加一。
    if(!db.transaction()){
        qCCritical(lcDb)<<"login transaction begin failed:"<<db.lastError().text();
        return 0;
    }
    QSqlQuery vq(db);
    vq.prepare("UPDATE user_info SET login_version = LAST_INSERT_ID(login_version + 1) WHERE name = ?");
    vq.addBindValue(nameParam);
    if(!vq.exec() || vq.numRowsAffected() != 1){
        qCCritical(lcDb)<<"login version bump failed:"<<vq.lastError().text();
        db.rollback();
        return 0;
    }
    QSqlQuery lq(db);
    if(!lq.exec("SELECT LAST_INSERT_ID()") || !lq.next()){
        qCCritical(lcDb)<<"login version readback failed:"<<lq.lastError().text();
        db.rollback();
        return 0;
    }
    outVersion = lq.value(0).toULongLong();

    QSqlQuery rq(db);
    rq.prepare("INSERT INTO login_record(username, client_id, login_version, login_time) VALUES(?,?,?,NOW())");
    rq.addBindValue(nameParam);
    rq.addBindValue(clientId);
    rq.addBindValue(static_cast<qlonglong>(outVersion));
    if(!rq.exec()){
        qCCritical(lcDb)<<"login record insert failed:"<<rq.lastError().text();
        db.rollback();
        outVersion = 0;
        return 0;
    }

    QSqlQuery oq(db);
    oq.prepare("update user_info set online = 1 where name = ? and pwd = ?");
    oq.addBindValue(nameParam);
    oq.addBindValue(effectiveHash);
    qCDebug(lcDb)<<"login update online for:"<<nameParam;
    if (!oq.exec()) {
        qCCritical(lcDb) << "UPDATE online status failed:" << oq.lastError();
        db.rollback();
        outVersion = 0;
        return 0;
    }

    if(!db.commit()){
        qCCritical(lcDb)<<"login transaction commit failed:"<<db.lastError().text();
        db.rollback();
        outVersion = 0;
        return 0;
    }
    qCDebug(lcDb)<<"[LOGINVER] user="<<nameParam<<" ver="<<outVersion<<" client_id="<<clientId;
    return 1;
}

// 下线：把 online 归零。执行失败不重试、不报错——启动期对账可清理残留
void OperateDB::handleOffLine(const char *strname)
{
    if(strname==nullptr){
        return ;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return ;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("update user_info set online = 0 where name = ?");
    q.addBindValue(QString::fromUtf8(strname));
    qCDebug(lcDb)<<"offline update for:"<<QString::fromUtf8(strname);
    q.exec();
}

// REQ-AUTH-CONNECTION-001：恢复成功置 online=1；不动 login_version（版本只在登录事务递增）
void OperateDB::handleOnLine(const char *strname)
{
    if(strname==nullptr){
        return ;
    }
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return ;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);
    q.prepare("update user_info set online = 1 where name = ?");
    q.addBindValue(QString::fromUtf8(strname));
    qCDebug(lcDb)<<"online update for:"<<QString::fromUtf8(strname);
    q.exec();
}

// 查用户在线态：命中返回 online 列（0/1）；不存在返回 2；参数或连接失败返回 -1
int OperateDB::handleFindUser(const char *userName)
{
    if(userName==nullptr){
        return -1;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return -1;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("select * from user_info where name = ?");
    q.addBindValue(QString::fromUtf8(userName));
    qCDebug(lcDb)<<"find user:"<<QString::fromUtf8(userName);
    // REQ-AUDIT-FULL-001：查询失败回 -1（数据库不可用），不再误报"用户不存在"
    if(!q.exec()){
        qCWarning(lcDb)<<"handleFindUser exec failed:"<<q.lastError().text();
        return -1;
    }
    if(q.next()){
        return q.value(3).toInt();
    }
    return 2;
}

// 所有用户分页（含离线；排除自己，也排除任一方向已成好友的用户）。
// REQ-PAGE-FETCH-002：keyword 非空时按名字子串匹配（LIKE 通配符以 ! 转义按字面）；
// 结果总量受 LIST_TOTAL_CAP 封顶（totalOut 回带封顶后总数，cappedOut=1 表示实际超限）。
// 按名字字典序稳定排序；调用方保证 limit∈[1,200]，0=全量（≤上限）；offset 为分页起点。
// hasMoreOut=还有下一页；连接失败返回空列表。
QVector<QPair<QString,bool>> OperateDB::handleAllUsers(const QString &strname, const QString &keyword,
                                                       uint limit, uint offset,
                                                       uint *totalOut, bool *hasMoreOut, bool *cappedOut)
{
    QVector<QPair<QString,bool>> ret;
    if(totalOut!=nullptr) *totalOut=0;
    if(hasMoreOut!=nullptr) *hasMoreOut=false;
    if(cappedOut!=nullptr) *cappedOut=false;
    if(strname.isEmpty()){
        return ret;
    }
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return ret;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    QString baseWhere = QLatin1String(
        "name != ? AND name NOT IN ("
        " SELECT friendName FROM friend WHERE name = ?"
        " UNION"
        " SELECT name FROM friend WHERE friendName = ?)");
    // 关键词中的 %/_/! 按字面转义，不允许用户输入改变匹配语义；预编译绑定防注入
    QString likePattern;
    if(!keyword.isEmpty()){
        QString escaped=keyword;
        escaped.replace(QLatin1Char('!'),QLatin1String("!!"))
               .replace(QLatin1Char('%'),QLatin1String("!%"))
               .replace(QLatin1Char('_'),QLatin1String("!_"));
        likePattern=QLatin1Char('%')+escaped+QLatin1Char('%');
        baseWhere+=QLatin1String(" AND name LIKE ? ESCAPE '!'");
    }
    q.prepare("SELECT COUNT(*) FROM user_info WHERE "+baseWhere);
    q.addBindValue(strname);
    q.addBindValue(strname);
    q.addBindValue(strname);
    if(!likePattern.isEmpty()) q.addBindValue(likePattern);
    uint actualTotal=0;
    if(q.exec() && q.next()){
        actualTotal=q.value(0).toUInt();
    }
    const bool capped = actualTotal > LIST_TOTAL_CAP;
    if(cappedOut!=nullptr) *cappedOut=capped;
    const uint total = capped ? LIST_TOTAL_CAP : actualTotal;
    if(totalOut!=nullptr) *totalOut=total;

    // REQ-PAGE-FETCH-002：offset≥封顶总数回空页（封顶内容不可翻达）；
    // limit==0 保留旧全量语义但受上限保护；单页有效 limit 钳制不越界
    if(offset >= total){
        return ret;
    }
    const uint remain = total - offset;
    const uint effLimit = (limit==0) ? remain : qMin(limit, remain);
    q.prepare("SELECT name, online FROM user_info WHERE "+baseWhere+
              " ORDER BY name ASC LIMIT ? OFFSET ?");
    q.addBindValue(strname);
    q.addBindValue(strname);
    q.addBindValue(strname);
    if(!likePattern.isEmpty()) q.addBindValue(likePattern);
    q.addBindValue(effLimit);
    q.addBindValue(offset);
    qCDebug(lcDb)<<"all users query for:"<<strname<<"keyword"<<keyword
                 <<"limit"<<effLimit<<"offset"<<offset<<"total"<<total<<"capped"<<capped;
    if(!q.exec()){
        qCWarning(lcDb)<<"handleAllUsers exec failed:"<<q.lastError().text();
        return ret;
    }
    while(q.next())
    {
        ret.append(qMakePair(q.value(0).toString(), q.value(1).toInt()==1));
    }
    if(hasMoreOut!=nullptr){
        *hasMoreOut = (offset + uint(ret.size())) < total;
    }
    return ret;
}

// 加好友前置检查：判断目标是否在线；在线时把 (请求方, 目标) 落为待处理邀请。
// REQ-AUDIT-001 R4：接受阶段必须命中这条 pending 行，请求方以库内记录为准，
// 客户端随后提交的"请求方"字段不再决定关系方向。
// 返回 1 在线（已登记邀请）/ 0 不在线或不存在 / -1 参数为空或连接不可用。
int OperateDB::handleAddFriend(char *usrname, char *tarname)
{
    if(usrname==nullptr||tarname==nullptr){
        return -1;
    }
    const QString requester = QString::fromUtf8(usrname);
    const QString target = QString::fromUtf8(tarname);
    if(requester.isEmpty() || target.isEmpty() || requester==target){
        return 0;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return -1;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    // REQ-CHAT-ACK-001 R10：不再要求目标在线——离线申请同样登记到 friend_request，
    // 由对方下次登录/刷新好友时经 113/114 拉取处理。这里只校验目标账号存在。
    q.prepare("select 1 from user_info where name = ?");
    q.addBindValue(target);
    if(!q.exec() || !q.next()){
        return 0;
    }
    // 幂等：同一对请求方/目标只保留一条 pending；重复请求不重复登记，也不刷新时间。
    q.prepare("INSERT IGNORE INTO friend_request(requester_name,target_name,status) "
              "VALUES(?,?,'pending')");
    q.addBindValue(requester);
    q.addBindValue(target);
    if(!q.exec()){
        qCWarning(lcDb)<<"addfriend: 邀请登记失败:"<<q.lastError().text()
                       <<" requester="<<requester<<" target="<<target;
        // 登记失败不能放行（否则接受阶段必然失败），按"不在线/不可用"回 0
        return 0;
    }
    return 1;
}

// 接受好友：REQ-AUDIT-001 R4——同一事务内校验待处理邀请并写入双向关系。
// usrname=请求方（由调用方从 PDU 取，仅作查询键），tarname=接受方（连接身份）。
// 返回 1 成功 / 0 无待处理邀请或写入失败 / -1 参数为空或连接不可用。
int OperateDB::handleAddFriendFin(char *usrname, char *tarname)
{
    if(usrname==nullptr||tarname==nullptr){
        return -1;
    }
    const QString requester = QString::fromUtf8(usrname);
    const QString accepter = QString::fromUtf8(tarname);
    if(requester.isEmpty() || accepter.isEmpty() || requester==accepter){
        return 0;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return -1;
    }
    QSqlDatabase db = connection.database();
    if(!db.transaction()){
        qCWarning(lcDb)<<"addfriendFin: 事务开启失败:"<<db.lastError().text();
        return 0;
    }

    QSqlQuery q(db);
    q.prepare("SELECT id FROM friend_request WHERE requester_name=? AND target_name=? "
              "AND status='pending' FOR UPDATE");
    q.addBindValue(requester);
    q.addBindValue(accepter);
    if(!q.exec()){
        qCWarning(lcDb)<<"addfriendFin: 查询邀请失败:"<<q.lastError().text();
        db.rollback();
        return 0;
    }
    if(!q.next()){
        // 无待处理邀请：伪造的接受（含客户端声称的请求方）一律拒绝
        db.rollback();
        qCWarning(lcDb)<<"addfriendFin: 拒绝：无待处理邀请 requester="<<requester<<" accepter="<<accepter;
        return 0;
    }
    const qlonglong requestId = q.value(0).toLongLong();
    q.finish();

    // 双向关系同一事务写入；已存在的行保持幂等
    q.prepare("INSERT IGNORE INTO friend (name, friendName) VALUES (?, ?)");
    q.addBindValue(requester);
    q.addBindValue(accepter);
    if(!q.exec()){ qCWarning(lcDb)<<"addfriendFin: 写请求方向失败:"<<q.lastError().text(); db.rollback(); return 0; }

    q.prepare("INSERT IGNORE INTO friend (name, friendName) VALUES (?, ?)");
    q.addBindValue(accepter);
    q.addBindValue(requester);
    if(!q.exec()){ qCWarning(lcDb)<<"addfriendFin: 写接受方向失败:"<<q.lastError().text(); db.rollback(); return 0; }

    q.prepare("UPDATE friend_request SET status='accepted', handled_at=NOW() WHERE id=?");
    q.addBindValue(requestId);
    if(!q.exec()){ qCWarning(lcDb)<<"addfriendFin: 更新邀请状态失败:"<<q.lastError().text(); db.rollback(); return 0; }

    if(!db.commit()){
        qCWarning(lcDb)<<"addfriendFin: 提交失败:"<<db.lastError().text();
        db.rollback();
        return 0;
    }
    qCDebug(lcDb)<<"addfriendFin:"<<requester<<"<->"<<accepter;
    return 1;
}

// 好友列表：UNION 合并两个方向并自动去重。
// REQ-PAGE-FETCH-002：真分页取代 001 数量截断——按名字字典序稳定排序，limit/offset 分页
// （调用方保证 limit∈[1,200]，0=全量≤上限）；总量受 LIST_TOTAL_CAP 封顶。连接失败返回空列表。
QStringList OperateDB::handleFlushFriend(const QString &strname, uint limit, uint offset,
                                         uint *totalOut, bool *hasMoreOut, bool *cappedOut)
{
    QStringList ret;
    if(totalOut!=nullptr) *totalOut=0;
    if(hasMoreOut!=nullptr) *hasMoreOut=false;
    if(cappedOut!=nullptr) *cappedOut=false;
    if(strname.isEmpty()){
        return ret;
    }
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return QStringList();
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    // 查询 name=strname 时的 friendName，以及 friendName=strname 时的 name（UNION 去重）
    const QString unionSql = QLatin1String(
        "SELECT friendName AS n FROM friend WHERE name = ?"
        " UNION"
        " SELECT name AS n FROM friend WHERE friendName = ?");
    q.prepare("SELECT COUNT(*) FROM ("+unionSql+") t");
    q.addBindValue(strname);
    q.addBindValue(strname);
    uint actualTotal=0;
    // REQ-AUDIT-FULL-001：查询失败显式告警，不再静默当作"无好友"
    if(q.exec() && q.next()){
        actualTotal=q.value(0).toUInt();
    }else{
        qCWarning(lcDb)<<"handleFlushFriend count failed:"<<q.lastError().text();
        return QStringList();
    }
    const bool capped = actualTotal > LIST_TOTAL_CAP;
    if(cappedOut!=nullptr) *cappedOut=capped;
    const uint total = capped ? LIST_TOTAL_CAP : actualTotal;
    if(totalOut!=nullptr) *totalOut=total;

    // offset≥封顶总数回空页；limit==0 旧全量语义受上限保护；单页有效 limit 钳制不越界
    if(offset >= total){
        return ret;
    }
    const uint remain = total - offset;
    const uint effLimit = (limit==0) ? remain : qMin(limit, remain);
    q.prepare("SELECT n FROM ("+unionSql+") t ORDER BY n ASC LIMIT ? OFFSET ?");
    q.addBindValue(strname);
    q.addBindValue(strname);
    q.addBindValue(effLimit);
    q.addBindValue(offset);
    qCDebug(lcDb)<<"friend page for:"<<strname<<"limit"<<effLimit<<"offset"<<offset
                 <<"total"<<total<<"capped"<<capped;
    if(!q.exec()){
        qCWarning(lcDb)<<"handleFlushFriend exec failed:"<<q.lastError().text();
        return QStringList();
    }
    while(q.next()){
        ret.append(q.value(0).toString());
    }
    if(hasMoreOut!=nullptr){
        *hasMoreOut = (offset + uint(ret.size())) < total;
    }
    return ret;
}

// 删除好友：事务内删除双向记录，任一步失败即回滚（不会只删一半）
bool OperateDB::handleDeleteFriend(char *userName, char *tarName)
{
    if(userName==nullptr||tarName==nullptr){
        return 0;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();
    
    // 开启事务
    db.transaction();
    QSqlQuery q(db);

    // 删除双向好友记录
    q.prepare("DELETE FROM friend WHERE (name = ? AND friendName = ?) OR (name = ? AND friendName = ?)");
    q.addBindValue(QString::fromUtf8(userName));
    q.addBindValue(QString::fromUtf8(tarName));
    q.addBindValue(QString::fromUtf8(tarName));
    q.addBindValue(QString::fromUtf8(userName));
    if(!q.exec()) {
        db.rollback();
        return false;
    }

    if(!db.commit()) {
        db.rollback();
        return false;
    }
    
    qCDebug(lcDb)<<"deletefriend:"<<QString::fromUtf8(userName)<<"-"<<QString::fromUtf8(tarName);
    return true;
}

// 落库私聊消息：REQ-AUDIT-001 R1/R2 起 issuccess 恒写 0（待送达），由实时转发或
// 离线补发真正写出成功后调用 markChatDelivered 置 1。返回接收方是否在线。
// INSERT 失败只记日志并回带 storedOut=false，不影响返回值。
bool OperateDB::seedchatMsg(char *seedName, char *revName, QString msg, QString seedTime, bool *storedOut, qint64 *idOut)
{
    if(storedOut != nullptr)*storedOut = false;
    if(idOut != nullptr)*idOut = -1;
    if(seedName==nullptr||revName==nullptr){
        return 0;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("select * from user_info where name = ? and online = 1");
    q.addBindValue(QString::fromUtf8(revName));
    bool ans=1;
    ans&=q.exec();
    ans&=q.next();
    qCDebug(lcDb)<<"select online ans:"<<ans<<" for:"<<QString::fromUtf8(revName);

    q.prepare(R"(insert into message_info(seed_name,rev_name,msg,issuccess,seed_time)
        values(?,?,?,?,?))");
    q.addBindValue(QString::fromUtf8(seedName));
    q.addBindValue(QString::fromUtf8(revName));
    q.addBindValue(msg);
    q.addBindValue(0);                                  // R1/R2：待送达，写出成功后才置 1
    q.addBindValue(seedTime);
    bool execRet=q.exec();
    qCDebug(lcDb)<<"chat msg insert:"<<execRet;
    if(storedOut != nullptr)*storedOut = execRet;      // 16 应答的入库结果口径
    if(execRet && idOut != nullptr)*idOut = q.lastInsertId().toLongLong();
    return ans;
}

// 拉取未送达私聊消息（issuccess=0），只读不标记（REQ-AUDIT-001 R2）。
// 返回扁平列表，每条消息依次为 msg、seed_time；idOut 回带对应主键（供写出成功后标记）。
// 返回空也可能是确实没有消息。按主键升序＝接收顺序。
QStringList OperateDB::getchatMsg(char *seedName, char *revName, QVector<qlonglong> *idOut, bool *truncatedOut)
{
    QStringList ret;
    if(truncatedOut != nullptr) *truncatedOut=false;
    if(idOut != nullptr) idOut->clear();
    if(seedName==nullptr||revName==nullptr){
        return ret;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return ret;
    }
    QSqlDatabase db = connection.database();

    QSqlQuery q(db);
    // REQ-PAGE-FETCH-001：单批最多 PAGE_MAX_LIMIT 条（多取一条判定截断），仍按主键升序取最旧的一批
    q.prepare("select id,msg,seed_time from message_info "
              "where seed_name = ? and rev_name = ? and issuccess = 0 order by id asc limit ?");
    q.addBindValue(QString::fromUtf8(seedName));
    q.addBindValue(QString::fromUtf8(revName));
    q.addBindValue(uint(PAGE_MAX_LIMIT) + 1);
    qCDebug(lcDb)<<"getchatMsg for:"<<QString::fromUtf8(seedName)<<"->"<<QString::fromUtf8(revName);
    if(!q.exec()){
        qCWarning(lcDb)<<"getchatMsg select failed:"<<q.lastError().text();
        return ret;
    }
    while(q.next()){
        if(idOut != nullptr) idOut->append(q.value(0).toLongLong());
        ret.append(q.value(1).toString());   // msg
        ret.append(q.value(2).toString());   // seed_time
    }
    if(truncatedOut != nullptr){
        *truncatedOut = ret.size() > int(PAGE_MAX_LIMIT) * 2;
    }
    if(ret.size() > int(PAGE_MAX_LIMIT) * 2){
        ret = ret.mid(0, int(PAGE_MAX_LIMIT) * 2);    // 超上限截断，剩余确认后可再拉
        if(idOut != nullptr) idOut->resize(int(PAGE_MAX_LIMIT));
    }
    return ret;
}

// REQ-PAGE-FETCH-001：聊天历史分页（双方全部历史，含已送达；只读不改 issuccess）。
// cursorId=0 从最新一页开始，>0 只取 id<cursorId 的更早消息；按 id 降序取 limit+1 条判定 has_more。
QVector<OperateDB::ChatHistoryRow> OperateDB::getChatHistoryPage(const QString &self, const QString &peer,
                                                                 qint64 cursorId, uint limit,
                                                                 uint *totalOut, bool *hasMoreOut,
                                                                 qint64 *oldestOut)
{
    QVector<ChatHistoryRow> rows;
    if(totalOut!=nullptr) *totalOut=0;
    if(hasMoreOut!=nullptr) *hasMoreOut=false;
    if(oldestOut!=nullptr) *oldestOut=0;
    if(self.isEmpty() || peer.isEmpty() || limit==0){
        return rows;
    }
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return rows;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);
    const QString pairWhere = QLatin1String(
        "(seed_name = ? AND rev_name = ?) OR (seed_name = ? AND rev_name = ?)");
    q.prepare("SELECT COUNT(*) FROM message_info WHERE "+pairWhere);
    q.addBindValue(self); q.addBindValue(peer);
    q.addBindValue(peer); q.addBindValue(self);
    if(q.exec() && q.next()){
        if(totalOut!=nullptr) *totalOut=q.value(0).toUInt();
    }
    // 注意括号：pairWhere 含 OR，游标条件必须约束整个双方组合，不能只约束后半段
    QString sql=QStringLiteral("SELECT id, seed_name, msg, seed_time FROM message_info WHERE (")
            + pairWhere + QLatin1String(")");
    if(cursorId>0){
        sql+=QLatin1String(" AND id < ?");
    }
    sql+=QLatin1String(" ORDER BY id DESC LIMIT ?");
    q.prepare(sql);
    q.addBindValue(self); q.addBindValue(peer);
    q.addBindValue(peer); q.addBindValue(self);
    if(cursorId>0) q.addBindValue(cursorId);
    q.addBindValue(limit+1);
    if(!q.exec()){
        qCWarning(lcDb)<<"getChatHistoryPage failed:"<<q.lastError().text();
        return rows;
    }
    while(q.next()){
        ChatHistoryRow r;
        r.id=q.value(0).toLongLong();
        r.fromSelf=(q.value(1).toString()==self);
        r.msg=q.value(2).toString();
        r.time=q.value(3).toString();
        rows.append(r);
    }
    if(hasMoreOut!=nullptr){
        *hasMoreOut = rows.size() > int(limit);
    }
    if(rows.size() > int(limit)){
        rows = rows.mid(0, int(limit));               // 丢弃多取的判定行
    }
    if(!rows.isEmpty() && oldestOut!=nullptr){
        *oldestOut = rows.last().id;                  // id 降序 → 末条最旧
    }
    return rows;
}

// 标记已送达：只更新仍为未送达的行，重复调用无副作用。
bool OperateDB::markChatDelivered(const QVector<qlonglong> &ids)
{
    if(ids.isEmpty()){
        return true;
    }
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QStringList placeholders;
    for(int i=0;i<ids.size();++i){
        placeholders.append(QStringLiteral("?"));
    }
    QSqlQuery q(connection.database());
    q.prepare(QStringLiteral("update message_info set issuccess = 1 "
                             "where id in (%1) and issuccess = 0").arg(placeholders.join(QLatin1Char(','))));
    for(int i=0;i<ids.size();++i){
        q.addBindValue(ids.at(i));
    }
    if(!q.exec()){
        qCWarning(lcDb)<<"markChatDelivered failed:"<<q.lastError().text();
        return false;
    }
    return true;
}

// REQ-CHAT-ACK-001：客户端 110 确认收货后按「ID + 收件人」双条件标记，
// 伪造的他人消息 ID 不会命中本账号的行（rev_name = ?）。幂等：只更新 issuccess=0。
bool OperateDB::markChatDeliveredFor(const QString &rev, const QVector<qlonglong> &ids)
{
    if(rev.isEmpty() || ids.isEmpty()){
        return true;
    }
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QStringList placeholders;
    for(int i=0;i<ids.size();++i){
        placeholders.append(QStringLiteral("?"));
    }
    QSqlQuery q(connection.database());
    q.prepare(QStringLiteral("update message_info set issuccess = 1 "
                             "where rev_name = ? and issuccess = 0 and id in (%1)")
              .arg(placeholders.join(QLatin1Char(','))));
    q.addBindValue(rev);
    for(int i=0;i<ids.size();++i){
        q.addBindValue(ids.at(i));
    }
    if(!q.exec()){
        qCWarning(lcDb)<<"markChatDeliveredFor failed:"<<q.lastError().text();
        return false;
    }
    return true;
}

// REQ-CHAT-ACK-001 R10：本人仍待处理的好友申请，按登记时间倒序（早的先处理）
QStringList OperateDB::listPendingFriendRequests(const QString &target, bool *truncatedOut)
{
    QStringList ret;
    if(truncatedOut!=nullptr) *truncatedOut=false;
    if(target.isEmpty()){
        return ret;
    }
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return ret;
    }
    QSqlQuery q(connection.database());
    // REQ-PAGE-FETCH-001：只取最近 PAGE_TRUNCATE_DAYS 天内的 pending 申请，至多 PAGE_MAX_LIMIT 条
    q.prepare("select requester_name from friend_request "
              "where target_name = ? and status = 'pending' "
              "and created_at >= DATE_SUB(NOW(), INTERVAL ? DAY) "
              "order by created_at asc, id asc limit ?");
    q.addBindValue(target);
    q.addBindValue(uint(PAGE_TRUNCATE_DAYS));
    q.addBindValue(uint(PAGE_MAX_LIMIT));
    if(!q.exec()){
        qCWarning(lcDb)<<"listPendingFriendRequests failed:"<<q.lastError().text();
        return ret;
    }
    while(q.next()){
        ret.append(q.value(0).toString());
    }
    if(truncatedOut!=nullptr){
        // 窗口内条数已达上限即视为截断（窗口外更早的申请不再提示）
        *truncatedOut = ret.size() >= int(PAGE_MAX_LIMIT);
    }
    return ret;
}

// 拒绝好友申请：只清理本人收到的 pending 行，不建立关系、不删对方账号
bool OperateDB::rejectFriendRequest(const QString &requester, const QString &target)
{
    if(requester.isEmpty() || target.isEmpty()){
        return false;
    }
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlQuery q(connection.database());
    q.prepare("update friend_request set status = 'rejected', handled_at = NOW() "
              "where requester_name = ? and target_name = ? and status = 'pending'");
    q.addBindValue(requester);
    q.addBindValue(target);
    if(!q.exec()){
        qCWarning(lcDb)<<"rejectFriendRequest failed:"<<q.lastError().text();
        return false;
    }
    return true;
}

// 查询用户信息：命中回填 name 与 online 并返回 true；不存在或连接失败返回 false
bool OperateDB::handleGetUserInfo(const char *userName, QString &outName, int &outOnline)
{
    if(userName==nullptr){
        return false;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);
    //使用预编译查询防止 SQL 注入
    q.prepare("select name, online from user_info where name = ?");
    q.addBindValue(QString::fromUtf8(userName));
    if(!q.exec() || !q.next()){
        return false;
    }
    outName = q.value(0).toString();
    outOnline = q.value(1).toInt();
    return true;
}

// 注销账户：密码校验通过后单事务删除消息、好友、用户行；
// share_link/publish_links/login_record 的清理失败不回滚（表可能尚未迁移，视为成功）。
// 返回 true 表示事务已提交。
bool OperateDB::handleLogoff(const char* name, const char* pwd)
{
    if(name==nullptr||pwd==nullptr){
        return false;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();

    db.transaction();
    QSqlQuery q(db);

    // 1. 按用户名取哈希与盐后校验密码
    q.prepare("SELECT pwd, salt FROM user_info WHERE name = ?");
    q.addBindValue(QString::fromUtf8(name));
    if(!q.exec() || !q.next()){
        qCWarning(lcDb)<<"logoff: user/pwd mismatch or user not found:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }
    const QString effectiveHash = applyPasswordSalt(
        hashPasswordForStorage(QString::fromUtf8(pwd)), q.value(1).toString());
    if(effectiveHash != q.value(0).toString()){
        qCWarning(lcDb)<<"logoff: user/pwd mismatch or user not found:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    // 2. 删除 message_info 中涉及该用户的记录
    q.prepare("DELETE FROM message_info WHERE seed_name = ? OR rev_name = ?");
    q.addBindValue(QString::fromUtf8(name));
    q.addBindValue(QString::fromUtf8(name));
    if(!q.exec()){
        qCWarning(lcDb)<<"logoff: failed to delete messages for:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    // 3. 删除 friend 表中涉及该用户的所有记录（双向）
    q.prepare("DELETE FROM friend WHERE name = ? OR friendName = ?");
    q.addBindValue(QString::fromUtf8(name));
    q.addBindValue(QString::fromUtf8(name));
    if(!q.exec()){
        qCWarning(lcDb)<<"logoff: failed to delete friend records for:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    // 4. 删除 user_info 表中的用户记录
    q.prepare("DELETE FROM user_info WHERE name = ? AND pwd = ?");
    q.addBindValue(QString::fromUtf8(name));
    q.addBindValue(effectiveHash);
    if(!q.exec()){
        qCWarning(lcDb)<<"logoff: failed to delete user_info for:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    // 3.5 — 删除 share_link 表
    q.prepare("DELETE FROM share_link WHERE owner_name = ?");
    q.addBindValue(QString::fromUtf8(name));
    if(!q.exec()){
        qCDebug(lcDb)<<"logoff: warning - failed to delete share_links (might be empty):"<<QString::fromUtf8(name);
        // 不回滚，视为成功
    }

    // 3.6 — 删除 publish_links 表（既有表，补充清理）
    q.prepare("DELETE FROM publish_links WHERE creator_name = ?");
    q.addBindValue(QString::fromUtf8(name));
    if(!q.exec()){
        qCDebug(lcDb)<<"logoff: warning - failed to delete publish_links (might be empty):"<<QString::fromUtf8(name);
        // 不回滚，视为成功
    }

    // 3.7 — REQ-EXT-SESSION-001：清理 login_record 档案（表未迁移时同样视为成功）
    q.prepare("DELETE FROM login_record WHERE username = ?");
    q.addBindValue(QString::fromUtf8(name));
    if(!q.exec()){
        qCDebug(lcDb)<<"logoff: warning - failed to delete login_records (might be empty):"<<QString::fromUtf8(name);
        // 不回滚，视为成功
    }

    // ===== REQ-AUDIT-001 R3：账号资产级联清理（与本事务同一提交） =====
    // 目标：同名重注册不得看到/继承任何前账号资产。顺序：先收集受影响对象，再删引用，
    // 最后按"表内真相"重算引用计数（与回收调度器同口径），归零置 pending_reclaim。
    const QString ownerName = QString::fromUtf8(name);
    QList<qint64> affectedObjects;
    {
        QSqlQuery oq(db);
        oq.prepare("SELECT DISTINCT object_id FROM user_file WHERE owner_name=? AND object_id IS NOT NULL");
        oq.addBindValue(ownerName);
        if(!oq.exec()){ qCWarning(lcDb)<<"logoff: collect user_file objects failed:"<<oq.lastError().text(); db.rollback(); return false; }
        while(oq.next()) affectedObjects.append(oq.value(0).toLongLong());

        oq.prepare("SELECT DISTINCT object_id FROM group_file WHERE uploader_name=? AND object_id IS NOT NULL");
        oq.addBindValue(ownerName);
        if(!oq.exec()){ qCWarning(lcDb)<<"logoff: collect group_file objects failed:"<<oq.lastError().text(); db.rollback(); return false; }
        while(oq.next()){
            const qint64 oid = oq.value(0).toLongLong();
            if(!affectedObjects.contains(oid)) affectedObjects.append(oid);
        }
    }

    // 群：本人为群主的 active 群 → 解散；其余本人 active 成员行 → left（保留历史只读）
    q.prepare("UPDATE group_info gi JOIN group_member gm ON gm.group_id=gi.group_id "
              "SET gi.status='dissolved', gi.updated_at=CURRENT_TIMESTAMP "
              "WHERE gm.user_name=? AND gm.role='owner' AND gm.status='active' AND gi.status='active'");
    q.addBindValue(ownerName);
    if(!q.exec()){ qCWarning(lcDb)<<"logoff: dissolve owned groups failed:"<<q.lastError().text(); db.rollback(); return false; }

    q.prepare("UPDATE group_member SET status='left', updated_at=CURRENT_TIMESTAMP "
              "WHERE user_name=? AND status='active'");
    q.addBindValue(ownerName);
    if(!q.exec()){ qCWarning(lcDb)<<"logoff: leave groups failed:"<<q.lastError().text(); db.rollback(); return false; }

    // 待处理邀请：本人收到的与本人发出的
    q.prepare("DELETE FROM group_invitation WHERE (target_name=? OR inviter_name=?) AND status='pending'");
    q.addBindValue(ownerName);
    q.addBindValue(ownerName);
    if(!q.exec()){ qCWarning(lcDb)<<"logoff: delete group invitations failed:"<<q.lastError().text(); db.rollback(); return false; }

    // 文件：删除本人个人条目与本人上传的群文件；内容对象只减引用，由回收调度器按宽限期清理
    q.prepare("DELETE FROM user_file WHERE owner_name=?");
    q.addBindValue(ownerName);
    if(!q.exec()){ qCWarning(lcDb)<<"logoff: delete user_file failed:"<<q.lastError().text(); db.rollback(); return false; }

    q.prepare("DELETE FROM group_file WHERE uploader_name=?");
    q.addBindValue(ownerName);
    if(!q.exec()){ qCWarning(lcDb)<<"logoff: delete group_file failed:"<<q.lastError().text(); db.rollback(); return false; }

    q.prepare("DELETE FROM upload_session WHERE owner_name=?");
    q.addBindValue(ownerName);
    if(!q.exec()){
        // upload_session 未迁移的旧库：不阻断注销（其余资产已清理）
        qCDebug(lcDb)<<"logoff: warning - failed to delete upload_session:"<<q.lastError().text();
    }

    // 待处理好友邀请（REQ-AUDIT-001 R4）：表未迁移时不阻断注销
    q.prepare("DELETE FROM friend_request WHERE requester_name=? OR target_name=?");
    q.addBindValue(ownerName);
    q.addBindValue(ownerName);
    if(!q.exec()){
        qCDebug(lcDb)<<"logoff: warning - failed to delete friend_request:"<<q.lastError().text();
    }

    // 按表内真相重算受影响对象的引用计数（active 个人条目 ∪ active 群文件）
    for(qint64 oid : affectedObjects){
        QSqlQuery fo(db);
        fo.prepare("SELECT refcount,status FROM file_object WHERE object_id=? FOR UPDATE");
        fo.addBindValue(oid);
        if(!fo.exec() || !fo.next()){ continue; }   // 对象已不存在：跳过
        if(fo.value(1).toString() == QLatin1String("reclaiming")){
            continue;                               // 调度器已抢删除权：不复活、不重算
        }
        QSqlQuery cnt(db);
        cnt.prepare("SELECT (SELECT COUNT(*) FROM user_file WHERE object_id=? AND status='active')"
                    " + (SELECT COUNT(*) FROM group_file WHERE object_id=? AND status='active')");
        cnt.addBindValue(oid);
        cnt.addBindValue(oid);
        if(!cnt.exec() || !cnt.next()){ qCWarning(lcDb)<<"logoff: recount object failed:"<<cnt.lastError().text(); db.rollback(); return false; }
        const int refs = cnt.value(0).toInt();
        QSqlQuery up(db);
        up.prepare("UPDATE file_object SET refcount=?, status=?, reclaim_at=? WHERE object_id=?");
        up.addBindValue(refs);
        up.addBindValue(refs <= 0 ? QStringLiteral("pending_reclaim") : QStringLiteral("ready"));
        // 显式格式化为 MySQL DATETIME 字符串（QMYSQL 直接绑 QDateTime 会带 'T'，严格模式拒收）
        up.addBindValue(refs <= 0
                        ? QVariant(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"))
                        : QVariant(QVariant::String));
        up.addBindValue(oid);
        if(!up.exec()){ qCWarning(lcDb)<<"logoff: update object refcount failed:"<<up.lastError().text(); db.rollback(); return false; }
    }

    if(!db.commit()){
        qCWarning(lcDb)<<"logoff: commit failed for:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    qCDebug(lcDb)<<"logoff: successfully deleted user:"<<QString::fromUtf8(name);
    return true;
}

// 旧版公开链接：一条 INSERT，无查重；失败返回 false
bool OperateDB::createPublishLink(const QString &code, const QString &path, const QString &creator)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("INSERT INTO publish_links (short_code, file_path, creator_name) VALUES (?, ?, ?)");
    q.addBindValue(code);
    q.addBindValue(path);
    q.addBindValue(creator);
    return q.exec();
}

namespace {
// 构造失败结果：清零群/版本/事件字段，避免调用方误读半成品状态
OperateDB::GroupResult groupFailure(int code)
{
    OperateDB::GroupResult result;
    result.code = code;
    result.groupId = 0;
    result.version = 0;
    result.eventType = 0;
    return result;
}

// 名称/幂等键校验：非空、不超列宽、不含 NUL 与 CR/LF/TAB（防止破坏协议与日志）
bool groupIsValidName(const QString &value, int maxLength)
{
    return !value.isEmpty() && value.size() <= maxLength && !value.contains(QChar('\0'))
            && !value.contains(QRegExp("[\\r\\n\\t]"));
}

// 取 actor 在群内的角色（owner/admin/member）；非 active 成员或 SQL 失败返回空串。
// 调用方多以空串作为"无权限/非成员"的统一判据。
QString groupRole(QSqlDatabase &db, quint64 groupId, const QString &actor)
{
    QSqlQuery q(db);
    q.prepare("SELECT role FROM group_member WHERE group_id=? AND user_name=? AND status='active'");
    q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
    q.addBindValue(actor);
    return q.exec() && q.next() ? q.value(0).toString() : QString();
}

// 群内全部 active 成员账号，用于事件广播名单（不含已退出/被移除者）
QStringList groupActiveUsers(QSqlDatabase &db, quint64 groupId)
{
    QStringList users;
    QSqlQuery q(db);
    q.prepare("SELECT user_name FROM group_member WHERE group_id=? AND status='active'");
    q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
    if (q.exec()) {
        while (q.next()) users.append(q.value(0).toString());
    }
    return users;
}

// 群列表单行编码：group_id、group_name、status、role、version（顺序即协议契约）
void groupAppendRecord(QByteArray &body, quint64 groupId, const QString &groupName,
                       const QString &status, const QString &role, quint64 version)
{
    groupAppendU64(body, groupId);
    groupAppendString(body, groupName.toUtf8());
    groupAppendString(body, status.toUtf8());
    groupAppendString(body, role.toUtf8());
    groupAppendU64(body, version);
}

// REQ-EXT-GROUP-002 T6：成员事件系统消息——调用方已在同一事务内，本函数沿用同一
// 序号通道（group_info.last_seq 原子递增）追加一条 msg_type='system' 行。
// 返回 false 时调用方必须回滚；code 为业务失败码（-2105 序号分配/入库失败）。
bool groupAppendSystemMessage(QSqlDatabase &db, quint64 groupId, const QString &sender,
                              const QString &content, OperateDB::GroupMessageRecord &out, int &code)
{
    QSqlQuery q(db);
    q.prepare("UPDATE group_info SET last_seq=LAST_INSERT_ID(last_seq+1) WHERE group_id=?");
    q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
    if (!q.exec() || q.numRowsAffected() != 1) { code = -2105; return false; }
    q.prepare("SELECT LAST_INSERT_ID()");
    if (!q.exec() || !q.next()) { code = -2105; return false; }
    const quint64 seq = q.value(0).toULongLong();
    if (seq == 0) { code = -2105; return false; }
    q.prepare("INSERT INTO group_message(group_id,seq,client_msg_id,sender_name,msg_type,content) "
              "VALUES (?,?,NULL,?,'system',?)");
    q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
    q.addBindValue(QVariant::fromValue<qulonglong>(seq));
    q.addBindValue(sender);
    q.addBindValue(content);
    if (!q.exec()) { code = -2105; return false; }
    const quint64 msgId = q.lastInsertId().toULongLong();
    q.prepare("SELECT ROUND(UNIX_TIMESTAMP(created_at)*1000) FROM group_message WHERE msg_id=?");
    q.addBindValue(QVariant::fromValue<qulonglong>(msgId));
    if (!q.exec() || !q.next()) { code = -2105; return false; }
    out.seq = seq;
    out.msgId = msgId;
    out.sender = sender;
    out.msgType = 1;
    out.createdAtMs = q.value(0).toULongLong();
    out.content = content;
    return true;
}
}

// 群操作总入口：先分流只读查询（建群/群列表/邀请/成员/摘要），其余写操作
// 统一开事务并锁定 group_info 行，再按角色做授权与变更。
// code=0 成功；负数为业务错误；写成功时 version 为提交后的群版本。
OperateDB::GroupResult OperateDB::handleGroupOperation(const GroupRequest &request,
                                                       const QString &actor)
{
    if (actor.isEmpty()) return groupFailure(-1001); // 未认证主体
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return groupFailure(-1002); // 数据库不可达
    QSqlDatabase db = connection.database();
    GroupResult result = groupFailure(-1000);
    QSqlQuery q(db);

    if (request.operation == GroupCreate) {
        if (!groupIsValidName(request.groupName, 128)) return groupFailure(-1003);
        if (!db.transaction()) return groupFailure(-1004);
        q.prepare("INSERT INTO group_info (group_name, creator_name, status, version) VALUES (?, ?, 'active', 1)");
        q.addBindValue(request.groupName);
        q.addBindValue(actor);
        if (!q.exec()) { db.rollback(); return groupFailure(-1005); }
        const quint64 groupId = q.lastInsertId().toULongLong();
        q.prepare("INSERT INTO group_member (group_id,user_name,role,status) VALUES (?,?,'owner','active')");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
        q.addBindValue(actor);
        if (!q.exec() || !db.commit()) { db.rollback(); return groupFailure(-1006); }
        result = groupFailure(0);
        result.groupId = groupId;
        result.version = 1;
        result.notifyUsers << actor;
        result.eventType = 1;
        groupAppendU64(result.responseBody, groupId);
        return result;
    }

    // ---- 只读分支：群列表（仅 active 成员，最近更新的群在前） ----
    if (request.operation == GroupList) {
        // REQ-PAGE-FETCH-001：多取一条判定截断，body 末尾追加 u8 truncated（旧端多出 1 字节不读）
        q.prepare("SELECT gi.group_id,gi.group_name,gi.status,gm.role,gi.version "
                  "FROM group_member gm JOIN group_info gi ON gi.group_id=gm.group_id "
                  "WHERE gm.user_name=? AND gm.status='active' ORDER BY gi.updated_at DESC LIMIT ?");
        q.addBindValue(actor);
        q.addBindValue(uint(PAGE_MAX_LIMIT) + 1);
        if (!q.exec()) return groupFailure(-1007);
        QByteArray body;
        QList<QByteArray> rows;
        while (q.next()) {
            QByteArray row;
            groupAppendRecord(row, q.value(0).toULongLong(), q.value(1).toString(),
                              q.value(2).toString(), q.value(3).toString(), q.value(4).toULongLong());
            rows.append(row);
        }
        bool truncated = rows.size() > int(PAGE_MAX_LIMIT);
        if (truncated) rows = rows.mid(0, int(PAGE_MAX_LIMIT));
        groupAppendU32(body, quint32(rows.size()));
        for (const QByteArray &row : rows) body.append(row);
        body.append(char(truncated ? 1 : 0));
        result = groupFailure(0);
        result.responseBody = body;
        return result;
    }

    // ---- 只读分支：我收到的、仍 pending 的邀请 ----
    if (request.operation == GroupQueryInvitations) {
        // REQ-PAGE-FETCH-001：只取最近 PAGE_TRUNCATE_DAYS 天内，至多 PAGE_MAX_LIMIT 条
        q.prepare("SELECT i.invitation_id,i.group_id,gi.group_name,i.inviter_name,i.status,"
                  "UNIX_TIMESTAMP(i.expires_at) FROM group_invitation i "
                  "JOIN group_info gi ON gi.group_id=i.group_id "
                  "WHERE i.target_name=? AND i.status='pending' "
                  "AND i.created_at >= DATE_SUB(NOW(), INTERVAL ? DAY) "
                  "ORDER BY i.created_at DESC LIMIT ?");
        q.addBindValue(actor);
        q.addBindValue(uint(PAGE_TRUNCATE_DAYS));
        q.addBindValue(uint(PAGE_MAX_LIMIT) + 1);
        if (!q.exec()) return groupFailure(-1008);
        QList<QByteArray> rows;
        while (q.next()) {
            QByteArray row;
            groupAppendU64(row, q.value(0).toULongLong());
            groupAppendU64(row, q.value(1).toULongLong());
            groupAppendString(row, q.value(2).toString().toUtf8());
            groupAppendString(row, q.value(3).toString().toUtf8());
            groupAppendString(row, q.value(4).toString().toUtf8());
            groupAppendU64(row, q.value(5).toULongLong());
            rows.append(row);
        }
        bool truncated = rows.size() > int(PAGE_MAX_LIMIT);
        if (truncated) rows = rows.mid(0, int(PAGE_MAX_LIMIT));
        QByteArray body;
        groupAppendU32(body, quint32(rows.size()));
        for (const QByteArray &row : rows) body.append(row);
        body.append(char(truncated ? 1 : 0));
        result = groupFailure(0);
        result.responseBody = body;
        return result;
    }

    // REQ-EXT-GROUP-002：群消息操作走独立子路径（发送/拉取/已读/未读摘要）
    if (request.operation == GroupSendMessage || request.operation == GroupPullMessages
            || request.operation == GroupReadConfirm || request.operation == GroupUnreadSummary) {
        return handleGroupMessageOperation(request, actor);
    }

    if (request.groupId == 0 && request.operation != GroupAcceptInvitation
            && request.operation != GroupRejectInvitation) return groupFailure(-1009);

    quint64 groupId = request.groupId;
    if (request.operation == GroupAcceptInvitation || request.operation == GroupRejectInvitation) {
        q.prepare("SELECT group_id FROM group_invitation WHERE invitation_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.invitationId));
        if (!q.exec() || !q.next()) return groupFailure(-1010);
        groupId = q.value(0).toULongLong();
    }

    if (request.operation == GroupListMembers || request.operation == GroupSummary) {
        q.prepare("SELECT status FROM group_info WHERE group_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
        if (!q.exec() || !q.next()) return groupFailure(-1011);
        const QString groupStatus = q.value(0).toString();
        if (groupRole(db, groupId, actor).isEmpty()) return groupFailure(-1012);
        if (request.operation == GroupListMembers) {
            // REQ-PAGE-FETCH-001：多取一条判定截断，body 末尾追加 u8 truncated
            q.prepare("SELECT user_name,role,status FROM group_member WHERE group_id=? AND status='active' "
                      "ORDER BY joined_at LIMIT ?");
            q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
            q.addBindValue(uint(PAGE_MAX_LIMIT) + 1);
            if (!q.exec()) return groupFailure(-1013);
            QList<QByteArray> rows;
            while (q.next()) {
                QByteArray row;
                groupAppendString(row, q.value(0).toString().toUtf8());
                groupAppendString(row, q.value(1).toString().toUtf8());
                groupAppendString(row, q.value(2).toString().toUtf8());
                rows.append(row);
            }
            bool truncated = rows.size() > int(PAGE_MAX_LIMIT);
            if (truncated) rows = rows.mid(0, int(PAGE_MAX_LIMIT));
            QByteArray body;
            groupAppendU32(body, quint32(rows.size()));
            for (const QByteArray &row : rows) body.append(row);
            body.append(char(truncated ? 1 : 0));
            result = groupFailure(0);
            result.groupId = groupId;
            result.responseBody = body;
            return result;
        }
        q.prepare("SELECT group_name,status,version FROM group_info WHERE group_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
        if (!q.exec() || !q.next()) return groupFailure(-1014);
        const QString name = q.value(0).toString();
        const quint64 version = q.value(2).toULongLong();
        q.prepare("SELECT COUNT(*) FROM group_member WHERE group_id=? AND status='active'");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
        if (!q.exec() || !q.next()) return groupFailure(-1015);
        QByteArray body;
        groupAppendU64(body, groupId);
        groupAppendString(body, name.toUtf8());
        groupAppendString(body, groupStatus.toUtf8());
        groupAppendU64(body, version);
        groupAppendU32(body, q.value(0).toUInt());
        result = groupFailure(0);
        result.groupId = groupId;
        result.version = version;
        result.responseBody = body;
        return result;
    }

    // ---- 写操作统一路径：行锁 group_info（串行化版本号），事务内完成鉴权与变更 ----
    if (!db.transaction()) return groupFailure(-1016);
    q.prepare("SELECT status,version FROM group_info WHERE group_id=? FOR UPDATE");
    q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
    if (!q.exec() || !q.next()) { db.rollback(); return groupFailure(-1017); }
    const QString groupStatus = q.value(0).toString();
    quint64 version = q.value(1).toULongLong();

    if (request.operation == GroupAcceptInvitation || request.operation == GroupRejectInvitation) {
        if (groupStatus != "active") { db.rollback(); return groupFailure(-1020); }
        q.prepare("SELECT target_name,status,expires_at,inviter_name FROM group_invitation WHERE invitation_id=? FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.invitationId));
        if (!q.exec() || !q.next()) { db.rollback(); return groupFailure(-1018); }
        if (q.value(0).toString() != actor || q.value(1).toString() != "pending") {
            db.rollback(); return groupFailure(-1019);
        }
        const bool expired = q.value(2).toDateTime() < QDateTime::currentDateTime();
        if (request.operation == GroupAcceptInvitation) {
            if (expired) { db.rollback(); return groupFailure(-1020); }
            q.prepare("SELECT status FROM group_member WHERE group_id=? AND user_name=? FOR UPDATE");
            q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(actor);
            if (!q.exec()) { db.rollback(); return groupFailure(-1021); }
            if (q.next()) {
                q.prepare("UPDATE group_member SET status='active',role='member',updated_at=CURRENT_TIMESTAMP "
                          "WHERE group_id=? AND user_name=?");
            } else {
                q.prepare("INSERT INTO group_member(group_id,user_name,role,status) VALUES (?,?,'member','active')");
            }
            q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(actor);
            if (!q.exec()) { db.rollback(); return groupFailure(-1022); }
            q.prepare("UPDATE group_invitation SET status='accepted',handled_at=NOW() WHERE invitation_id=?");
            q.addBindValue(QVariant::fromValue<qulonglong>(request.invitationId));
            if (!q.exec()) { db.rollback(); return groupFailure(-1023); }
            // REQ-EXT-GROUP-002 T6：加入事件在同一事务、同一序号通道追加系统消息
            GroupMessageRecord joinMsg;
            int sysCode = 0;
            if (!groupAppendSystemMessage(db, groupId, actor,
                                          actor + QStringLiteral(" 加入了群聊"), joinMsg, sysCode)) {
                db.rollback(); return groupFailure(sysCode);
            }
            ++version;
            q.prepare("UPDATE group_info SET version=? WHERE group_id=?");
            q.addBindValue(QVariant::fromValue<qulonglong>(version)); q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
            if (!q.exec() || !db.commit()) { db.rollback(); return groupFailure(-1024); }
            result = groupFailure(0); result.groupId = groupId; result.version = version;
            result.notifyUsers = groupActiveUsers(db, groupId); result.eventType = 1;
            result.message = joinMsg; result.broadcastMessage = true;
            groupAppendU64(result.responseBody, groupId);
            return result;
        }
        q.prepare("UPDATE group_invitation SET status=?,handled_at=NOW() WHERE invitation_id=?");
        q.addBindValue(expired ? "expired" : "rejected");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.invitationId));
        if (!q.exec() || !db.commit()) { db.rollback(); return groupFailure(-1025); }
        result = groupFailure(0); result.groupId = groupId; result.eventType = 2;
        return result;
    }

    // 写操作共同前置：调用者必须是 active 成员，且群未解散；之后按角色细分授权
    const QString actorRole = groupRole(db, groupId, actor);
    if (actorRole.isEmpty()) { db.rollback(); return groupFailure(-1026); }
    if (groupStatus != "active") { db.rollback(); return groupFailure(-1027); }

    if (request.operation == GroupInvite) {
        if (actorRole != "owner" && actorRole != "admin") { db.rollback(); return groupFailure(-1028); }
        if (!groupIsValidName(request.targetName, 32) || !groupIsValidName(request.requestKey, 64)) { db.rollback(); return groupFailure(-1029); }
        q.prepare("SELECT 1 FROM user_info WHERE name=?"); q.addBindValue(request.targetName);
        if (!q.exec() || !q.next()) { db.rollback(); return groupFailure(-1030); }
        q.prepare("SELECT status FROM group_member WHERE group_id=? AND user_name=? FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(request.targetName);
        if (!q.exec()) { db.rollback(); return groupFailure(-1031); }
        if (q.next() && q.value(0).toString() == "active") { db.rollback(); return groupFailure(-1032); }
        q.prepare("SELECT invitation_id,status FROM group_invitation WHERE group_id=? AND request_key=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(request.requestKey);
        if (q.exec() && q.next()) {
            const quint64 invitationId = q.value(0).toULongLong();
            if (!db.commit()) { db.rollback(); return groupFailure(-1033); }
            result = groupFailure(0); result.groupId = groupId; result.eventType = 2; result.notifyUsers << request.targetName;
            groupAppendU64(result.responseBody, invitationId); return result;
        }
        q.prepare("SELECT 1 FROM group_invitation WHERE group_id=? AND target_name=? AND status='pending' FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(request.targetName);
        if (q.exec() && q.next()) { db.rollback(); return groupFailure(-1034); }
        q.prepare("INSERT INTO group_invitation(group_id,inviter_name,target_name,status,expires_at,request_key) "
                  "VALUES (?,?,?,'pending',DATE_ADD(NOW(),INTERVAL 7 DAY),?)");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(actor);
        q.addBindValue(request.targetName); q.addBindValue(request.requestKey);
        if (!q.exec() || !db.commit()) { db.rollback(); return groupFailure(-1035); }
        result = groupFailure(0); result.groupId = groupId; result.eventType = 2; result.notifyUsers << request.targetName;
        groupAppendU64(result.responseBody, q.lastInsertId().toULongLong()); return result;
    }

    GroupMessageRecord sysMsg;
    bool sysMsgPending = false;
    int sysCode = 0;
    // 退出：群主不能直接退群（须先转让或解散）；仅 active 行被置为 left
    if (request.operation == GroupLeave) {
        if (actorRole == "owner") { db.rollback(); return groupFailure(-1036); }
        q.prepare("UPDATE group_member SET status='left',updated_at=CURRENT_TIMESTAMP WHERE group_id=? AND user_name=? AND status='active'");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(actor);
        if (!q.exec() || q.numRowsAffected() != 1) { db.rollback(); return groupFailure(-1037); }
        // REQ-EXT-GROUP-002 T6：退出事件系统消息（同一事务/序号通道）
        if (!groupAppendSystemMessage(db, groupId, actor,
                                      actor + QStringLiteral(" 退出了群聊"), sysMsg, sysCode)) {
            db.rollback(); return groupFailure(sysCode);
        }
        sysMsgPending = true;
    } else if (request.operation == GroupRemoveMember) {
        // 移除：owner/admin 可操作，但目标必须是普通 member（不能移除同级/群主）
        if (actorRole != "owner" && actorRole != "admin") { db.rollback(); return groupFailure(-1038); }
        q.prepare("SELECT role,status FROM group_member WHERE group_id=? AND user_name=? FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(request.targetName);
        if (!q.exec() || !q.next() || q.value(1).toString() != "active" || q.value(0).toString() != "member") { db.rollback(); return groupFailure(-1039); }
        q.prepare("UPDATE group_member SET status='removed',updated_at=CURRENT_TIMESTAMP WHERE group_id=? AND user_name=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(request.targetName);
        if (!q.exec()) { db.rollback(); return groupFailure(-1040); }
        // REQ-EXT-GROUP-002 T6：移除事件系统消息（同一事务/序号通道）
        if (!groupAppendSystemMessage(db, groupId, actor,
                                      request.targetName + QStringLiteral(" 被移出群聊"), sysMsg, sysCode)) {
            db.rollback(); return groupFailure(sysCode);
        }
        sysMsgPending = true;
    } else if (request.operation == GroupSetRole) {
        // 设角色：仅 owner 可把他人设为 admin/member；SQL 里排除 role='owner' 防误改
        if (actorRole != "owner" || (request.role != "admin" && request.role != "member")) { db.rollback(); return groupFailure(-1041); }
        q.prepare("UPDATE group_member SET role=?,updated_at=CURRENT_TIMESTAMP WHERE group_id=? AND user_name=? AND status='active' AND role<>'owner'");
        q.addBindValue(request.role); q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(request.targetName);
        if (!q.exec() || q.numRowsAffected() != 1) { db.rollback(); return groupFailure(-1042); }
    } else if (request.operation == GroupTransferOwner) {
        // 转让：目标升 owner，原 owner 降 admin；两条 UPDATE 都必须命中 1 行，否则回滚
        if (actorRole != "owner") { db.rollback(); return groupFailure(-1043); }
        q.prepare("UPDATE group_member SET role='owner',updated_at=CURRENT_TIMESTAMP WHERE group_id=? AND user_name=? AND status='active' AND role<>'owner'");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(request.targetName);
        if (!q.exec() || q.numRowsAffected() != 1) { db.rollback(); return groupFailure(-1044); }
        q.prepare("UPDATE group_member SET role='admin',updated_at=CURRENT_TIMESTAMP WHERE group_id=? AND user_name=? AND status='active' AND role='owner'");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(actor);
        if (!q.exec() || q.numRowsAffected() != 1) { db.rollback(); return groupFailure(-1045); }
    } else if (request.operation == GroupDissolve) {
        // 解散：仅 owner；只改 group_info.status，成员行保留供历史只读
        if (actorRole != "owner") { db.rollback(); return groupFailure(-1046); }
        q.prepare("UPDATE group_info SET status='dissolved' WHERE group_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
        if (!q.exec()) { db.rollback(); return groupFailure(-1047); }
    } else {
        db.rollback(); return groupFailure(-1048);
    }

    ++version;
    q.prepare("UPDATE group_info SET version=? WHERE group_id=?");
    q.addBindValue(QVariant::fromValue<qulonglong>(version)); q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
    if (!q.exec() || !db.commit()) { db.rollback(); return groupFailure(-1049); }
    result = groupFailure(0);
    result.groupId = groupId;
    result.version = version;
    result.notifyUsers = groupActiveUsers(db, groupId);
    result.eventType = 1;
    if (sysMsgPending) { result.message = sysMsg; result.broadcastMessage = true; }
    groupAppendU64(result.responseBody, groupId);
    return result;
}

// ===== REQ-EXT-GROUP-002：群消息操作（发送/拉取/已读/未读摘要） =====
// 权限事实源与 GROUP-001 同源：group_info.status='active' + group_member.status='active'。
// 失败码段 -2xxx：-2101 非成员 / -2102 群不存在或已解散 / -2103 内容/幂等键非法 /
// -2104 幂等冲突异常 / -2105 序号分配或入库失败。不泄露他群存在性。
OperateDB::GroupResult OperateDB::handleGroupMessageOperation(const GroupRequest &request,
                                                              const QString &actor)
{
    if (actor.isEmpty()) return groupFailure(-1001); // 未认证主体
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return groupFailure(-1002); // 数据库不可达
    QSqlDatabase db = connection.database();
    GroupResult result = groupFailure(-2000);
    QSqlQuery q(db);

    // ---- 未读摘要（95/96）：按 actor 汇总其全部 active 群，不需要 group_id ----
    if (request.operation == GroupUnreadSummary) {
        q.prepare("SELECT gm.group_id,gi.last_seq,gm.last_read_seq,"
                  "(SELECT COUNT(*) FROM group_message m WHERE m.group_id=gm.group_id "
                  " AND m.seq>gm.last_read_seq AND m.seq<=gi.last_seq) "
                  "FROM group_member gm JOIN group_info gi ON gi.group_id=gm.group_id "
                  "WHERE gm.user_name=? AND gm.status='active' AND gi.status='active' "
                  "ORDER BY gm.group_id");
        q.addBindValue(actor);
        if (!q.exec()) return groupFailure(-2105);
        QList<QByteArray> rows;
        while (q.next()) {
            QByteArray row;
            groupAppendU64(row, q.value(0).toULongLong());
            groupAppendU64(row, q.value(1).toULongLong());
            groupAppendU64(row, q.value(2).toULongLong());
            groupAppendU64(row, q.value(3).toULongLong());
            rows.append(row);
        }
        QByteArray body;
        groupAppendU32(body, quint32(rows.size()));
        for (const QByteArray &row : rows) body.append(row);
        result = groupFailure(0);
        result.responseBody = body;
        return result;
    }

    // 以下三类操作必须携带有效 group_id
    if (request.groupId == 0) return groupFailure(-2102);

    if (request.operation == GroupSendMessage) {
        // 服务端权威校验：内容非空且 <=2000B UTF-8；幂等键非空且 <=64B。
        // 载荷无 sender 字段——发送者一律取 actor。
        const QByteArray contentUtf8 = request.content.toUtf8();
        const QByteArray clientKeyUtf8 = request.clientMsgId.toUtf8();
        if (contentUtf8.isEmpty() || contentUtf8.size() > 2000
                || clientKeyUtf8.isEmpty() || clientKeyUtf8.size() > 64) {
            return groupFailure(-2103);
        }
        if (!db.transaction()) return groupFailure(-2105);
        // 群行锁：串行化同群发送，序号不重复、不倒退由行锁保证，uk_group_seq 兜底
        q.prepare("SELECT status,last_seq FROM group_info WHERE group_id=? FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        if (!q.exec() || !q.next() || q.value(0).toString() != "active") {
            db.rollback(); return groupFailure(-2102);
        }
        // 成员门：非成员/已退出/被移除即时失效
        q.prepare("SELECT role FROM group_member WHERE group_id=? AND user_name=? AND status='active' FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(actor);
        if (!q.exec() || !q.next()) { db.rollback(); return groupFailure(-2101); }
        // 幂等短路：同 client_msg_id 已存在 → 返回既有消息；REPEATABLE READ 下 gap 锁
        // 使并发同键重试串行，后到事务命中本分支。
        q.prepare("SELECT seq,msg_id,sender_name,msg_type,ROUND(UNIX_TIMESTAMP(created_at)*1000),content "
                  "FROM group_message WHERE group_id=? AND client_msg_id=? FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(request.clientMsgId);
        if (!q.exec()) { db.rollback(); return groupFailure(-2105); }
        if (q.next()) {
            GroupMessageRecord existing;
            existing.seq = q.value(0).toULongLong();
            existing.msgId = q.value(1).toULongLong();
            existing.sender = q.value(2).toString();
            existing.msgType = quint8(q.value(3).toString() == "system" ? 1 : 0);
            existing.createdAtMs = q.value(4).toULongLong();
            existing.content = q.value(5).toString();
            if (existing.sender != actor) { db.rollback(); return groupFailure(-2104); }
            if (!db.commit()) { db.rollback(); return groupFailure(-2104); }
            result = groupFailure(0);
            result.groupId = request.groupId;
            result.message = existing;
            result.broadcastMessage = false; // 重放不重复广播
            groupAppendString(result.responseBody, clientKeyUtf8);
            groupAppendU64(result.responseBody, existing.seq);
            groupAppendU64(result.responseBody, existing.msgId);
            groupAppendU64(result.responseBody, existing.createdAtMs);
            return result;
        }
        // 新消息：事务内原子分配序号（LAST_INSERT_ID 为连接级，同事务同连接）
        q.prepare("UPDATE group_info SET last_seq=LAST_INSERT_ID(last_seq+1),version=version+1 WHERE group_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        if (!q.exec() || q.numRowsAffected() != 1) { db.rollback(); return groupFailure(-2105); }
        q.prepare("SELECT LAST_INSERT_ID()");
        if (!q.exec() || !q.next()) { db.rollback(); return groupFailure(-2105); }
        const quint64 seq = q.value(0).toULongLong();
        if (seq == 0) { db.rollback(); return groupFailure(-2105); }
        q.prepare("INSERT INTO group_message(group_id,seq,client_msg_id,sender_name,msg_type,content) "
                  "VALUES (?,?,?,?,'text',?)");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(QVariant::fromValue<qulonglong>(seq));
        q.addBindValue(request.clientMsgId);
        q.addBindValue(actor);
        q.addBindValue(request.content);
        if (!q.exec()) { db.rollback(); return groupFailure(-2105); }
        const quint64 msgId = q.lastInsertId().toULongLong();
        q.prepare("SELECT ROUND(UNIX_TIMESTAMP(created_at)*1000) FROM group_message WHERE msg_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(msgId));
        if (!q.exec() || !q.next()) { db.rollback(); return groupFailure(-2105); }
        const quint64 createdAtMs = q.value(0).toULongLong();
        if (!db.commit()) { db.rollback(); return groupFailure(-2105); }
        // 先提交后广播：广播动作由 MsgHandler 在本函数返回后触发（设计 §5.2 不变式）
        result = groupFailure(0);
        result.groupId = request.groupId;
        result.message.seq = seq;
        result.message.msgId = msgId;
        result.message.sender = actor;
        result.message.msgType = 0;
        result.message.createdAtMs = createdAtMs;
        result.message.content = request.content;
        result.broadcastMessage = true;
        result.notifyUsers = groupActiveUsers(db, request.groupId);
        groupAppendString(result.responseBody, clientKeyUtf8);
        groupAppendU64(result.responseBody, seq);
        groupAppendU64(result.responseBody, msgId);
        groupAppendU64(result.responseBody, createdAtMs);
        return result;
    }

    if (request.operation == GroupPullMessages) {
        const quint8 mode = request.pullMode;       // 0 前向补齐 / 1 后向翻页，越界由解析层拒绝
        const int limit = qBound(1, int(request.limit), 100);
        q.prepare("SELECT status,last_seq FROM group_info WHERE group_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        if (!q.exec() || !q.next() || q.value(0).toString() != "active") return groupFailure(-2102);
        const quint64 groupLastSeq = q.value(1).toULongLong();
        q.prepare("SELECT last_read_seq FROM group_member WHERE group_id=? AND user_name=? AND status='active'");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(actor);
        if (!q.exec() || !q.next()) return groupFailure(-2101);
        const quint64 lastReadSeq = q.value(0).toULongLong();

        q.prepare(QStringLiteral("SELECT seq,msg_id,sender_name,msg_type,"
                                 "ROUND(UNIX_TIMESTAMP(created_at)*1000),content FROM group_message "
                                 "WHERE group_id=? AND seq %1 ? ORDER BY seq %2 LIMIT ?")
                  .arg(mode == 0 ? QStringLiteral(">") : QStringLiteral("<="),
                       mode == 0 ? QStringLiteral("ASC") : QStringLiteral("DESC")));
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(QVariant::fromValue<qulonglong>(request.cursorSeq));
        q.addBindValue(limit + 1); // 多取一条判定 has_more
        if (!q.exec()) return groupFailure(-2105);
        QList<QByteArray> records;
        while (q.next()) {
            const quint64 recSeq = q.value(0).toULongLong();
            QByteArray row;
            groupAppendU64(row, recSeq);
            groupAppendU64(row, q.value(1).toULongLong());
            groupAppendString(row, q.value(2).toString().toUtf8());
            row.append(char(quint8(q.value(3).toString() == "system" ? 1 : 0)));
            groupAppendU64(row, q.value(4).toULongLong());
            groupAppendString(row, q.value(5).toString().toUtf8());
            if (mode == 1) records.prepend(row); else records.append(row);
        }
        bool hasMore = records.size() > limit;
        if (hasMore) { if (mode == 1) records.removeFirst(); else records.removeLast(); }
        // next_cursor = 本批"实际返回"边界 seq（mode0=最大 / mode1=最小）：
        // 必须在裁掉多取行之后取，否则前向会跳过边界消息、后向会原地踏步。
        quint64 nextCursor = request.cursorSeq;
        if (!records.isEmpty()) {
            int cursorOffset = 0;
            quint64 boundary = 0;
            if (mode == 0) groupReadU64(records.last(), cursorOffset, boundary);
            else groupReadU64(records.first(), cursorOffset, boundary);
            nextCursor = boundary;
        }
        q.prepare("SELECT COUNT(*) FROM group_message WHERE group_id=? AND seq>? AND seq<=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(QVariant::fromValue<qulonglong>(lastReadSeq));
        q.addBindValue(QVariant::fromValue<qulonglong>(groupLastSeq));
        if (!q.exec() || !q.next()) return groupFailure(-2105);
        const quint64 unread = q.value(0).toULongLong();

        QByteArray body;
        body.append(char(mode));
        body.append(char(hasMore ? 1 : 0));
        groupAppendU64(body, nextCursor);
        groupAppendU64(body, groupLastSeq);
        groupAppendU64(body, unread);
        groupAppendU32(body, quint32(records.size()));
        for (const QByteArray &row : records) body.append(row);
        result = groupFailure(0);
        result.groupId = request.groupId;
        result.responseBody = body;
        return result;
    }

    if (request.operation == GroupReadConfirm) {
        // 单调推进：不可回退、不可越权代推（user_name 恒为 actor）、不可超过群水位
        q.prepare("SELECT status FROM group_info WHERE group_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        if (!q.exec() || !q.next() || q.value(0).toString() != "active") return groupFailure(-2102);
        q.prepare("SELECT 1 FROM group_member WHERE group_id=? AND user_name=? AND status='active'");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(actor);
        if (!q.exec() || !q.next()) return groupFailure(-2101);
        q.prepare("UPDATE group_member SET last_read_seq="
                  "LEAST(GREATEST(last_read_seq,?),"
                  "(SELECT last_seq FROM group_info WHERE group_id=?)) "
                  "WHERE group_id=? AND user_name=? AND status='active'");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.readSeq));
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(actor);
        if (!q.exec()) return groupFailure(-2105);
        q.prepare("SELECT last_read_seq FROM group_member WHERE group_id=? AND user_name=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(actor);
        if (!q.exec() || !q.next()) return groupFailure(-2105);
        result = groupFailure(0);
        result.groupId = request.groupId;
        groupAppendU64(result.responseBody, q.value(0).toULongLong());
        return result;
    }

    return groupFailure(-2000);
}

// ===== REQ-EXT-GROUP-003：群文件目录与访问控制 =====
// 判定点（全部服务端、每请求即时执行；与 GROUP-001/002 同源）：
//   P0 actor 有效 → P1 群存在 → P2 成员门(active) → P3 解散门(写操作) → P4 归属/存在性
//   → P5 删除授权(owner 或 上传者本人) → P6 名称(调用方预校验) → P7 对象存在性。
// 失败码：-1001 未认证 / -1002 库不可达 / -2101 非成员 / -2102 群不存在或已解散 /
//        -2103 归属或权限非法 / -2105 对象缺失或安置失败。不泄露他群存在性。
OperateDB::GroupFileResult OperateDB::handleGroupFileOperation(GroupFileRequest &request,
                                                              const QString &actor)
{
    GroupFileResult result;
    if (actor.isEmpty()) { result.code = -1001; return result; }
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) { result.code = -1002; return result; }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    // P1：群存在（取状态）
    q.prepare("SELECT status FROM group_info WHERE group_id=?");
    q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
    if (!q.exec() || !q.next()) { result.code = -2102; return result; }
    const QString groupStatus = q.value(0).toString();

    // P2：成员门（status='active'；left/removed/无行一律拒绝——退出/被移除即时失权）
    const QString role = groupRole(db, request.groupId, actor);
    if (role.isEmpty()) { result.code = -2101; return result; }

    // P3：解散门——写操作（上传/删除）要求群 active；读操作（列表/下载）成员可只读历史
    const bool writeOp = (request.operation == GroupFileUploadInit
                          || request.operation == GroupFileDelete);
    if (writeOp && groupStatus != "active") { result.code = -2102; return result; }

    if (request.operation == GroupFileList) {
        q.prepare("SELECT COUNT(*) FROM group_file WHERE group_id=? AND status='active'");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        if (!q.exec() || !q.next()) { result.code = -2105; return result; }
        const quint32 total = q.value(0).toUInt();

        q.prepare("SELECT group_file_id,size_bytes,UNIX_TIMESTAMP(created_at)*1000,"
                  "uploader_name,display_name FROM group_file "
                  "WHERE group_id=? AND status='active' ORDER BY group_file_id LIMIT ? OFFSET ?");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(int(request.limit));
        q.addBindValue(int(request.offset));
        if (!q.exec()) { result.code = -2105; return result; }
        QList<QByteArray> records;
        while (q.next()) {
            QByteArray row;
            // 元数据四元：名称、大小、上传者、时间（不含任何物理路径）
            groupAppendU64(row, q.value(0).toULongLong());
            groupAppendU64(row, q.value(1).toULongLong());
            groupAppendU64(row, q.value(2).toULongLong());
            groupAppendString(row, q.value(3).toString().toUtf8());
            groupAppendString(row, q.value(4).toString().toUtf8());
            records.append(row);
        }
        QByteArray body;
        groupAppendU32(body, quint32(records.size()));
        groupAppendU32(body, total);
        for (const QByteArray &row : records) body.append(row);
        result.code = 0;
        result.responseBody = body;
        return result;
    }

    if (request.operation == GroupFileUploadInit) {
        if (request.fileSize < 0 || request.shaHex.isEmpty() || request.displayName.isEmpty()) {
            result.code = -2103;
            return result;
        }
        // P7 去重预检查：同 sha256+size 的既有物理对象（FOR UPDATE 防并发双建）
        q.prepare("SELECT object_id FROM file_object WHERE sha256=? AND size_bytes=? FOR UPDATE");
        q.addBindValue(request.shaHex);
        q.addBindValue(request.fileSize);
        qint64 objectId = -1;
        if (q.exec() && q.next()) objectId = q.value(0).toLongLong();

        if (objectId >= 0) {
            // 秒传命中：个人条目 + 群条目 + 引用计数同事务；零正文传输
            const FileObjectStore::GroupAttachResult ar = FileObjectStore::instance().attachGroupFile(
                        actor, request.groupId, request.displayName, objectId, request.fileSize,
                        /*createUserEntry=*/true);
            if (!ar.ok) {
                qCWarning(lcDb) << "[GroupFile] 秒传安置失败 group=" << request.groupId
                                << "原因=" << ar.error;
                result.code = -2105;
                return result;
            }
            result.code = 0;
            result.hit = true;
            result.groupFileId = quint64(ar.groupFileId);
            request.hostFileId = ar.fileId;
            request.objectId = objectId;
            request.displayName = ar.groupDisplayName;
            return result;
        }
        // 未命中：不建对象/条目，由调用方建立暂存会话（staging + upload_session）走既有分块
        result.code = 0;
        result.hit = false;
        return result;
    }

    if (request.operation == GroupFileDelete) {
        // P5 授权在 releaseGroupFile 内与 P4 归属同事务判定：owner 可删任意，上传者可删己
        const bool isOwner = (role == "owner");
        QString err;
        if (!FileObjectStore::instance().releaseGroupFile(qint64(request.groupFileId),
                                                          request.groupId, actor, isOwner, err)) {
            qCWarning(lcDb) << "[GroupFile] 删除拒绝 group=" << request.groupId
                            << " gf=" << request.groupFileId << "原因=" << err;
            result.code = -2103;
            return result;
        }
        result.code = 0;
        return result;
    }

    if (request.operation == GroupFileDownload) {
        // P4 归属：group_file_id 必须与信封 group_id 同群且 active（不泄露他群存在性）
        q.prepare("SELECT object_id,size_bytes,display_name FROM group_file "
                  "WHERE group_file_id=? AND group_id=? AND status='active'");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupFileId));
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        if (!q.exec() || !q.next()) { result.code = -2103; return result; }
        const qint64 objectId = q.value(0).toLongLong();
        request.objectId = objectId;
        request.sizeBytes = q.value(1).toLongLong();
        request.displayName = q.value(2).toString();

        // P7 对象存在性（物理绝对路径由服务端解析，绝不接受客户端路径）
        q.prepare("SELECT storage_path FROM file_object WHERE object_id=?");
        q.addBindValue(objectId);
        if (!q.exec() || !q.next()) { result.code = -2105; return result; }
        result.code = 0;
        return result;
    }

    result.code = -2103;
    return result;
}
