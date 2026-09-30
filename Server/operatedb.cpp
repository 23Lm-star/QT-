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
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVector>

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

// salt 为空表示迁移前的老数据：保持裸哈希原值比对（登录成功后由调用方补盐升级）
QString applyPasswordSalt(const QString &clientHash, const QString &salt)
{
    if (salt.isEmpty()) {
        return clientHash;
    }
    return QString::fromLatin1(
        QCryptographicHash::hash((clientHash + salt).toUtf8(), QCryptographicHash::Md5).toHex());
}
}

OperateDB::~OperateDB()
{
}

OperateDB &OperateDB::instance()
{
    static OperateDB s;
    return s;
}

OperateDB::OperateDB(QObject *parent) : QObject(parent)
{
}

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
                         << "— 请先执行 Server/migrate_ext_file_001.sql";
        return false;
    }
    int version = 0;
    if (q.next() && !q.value(0).isNull()) {
        version = q.value(0).toInt();
    }
    const int required = kFileAssetSchemaVersion;
    if (version < required) {
        qCCritical(lcDb) << "[checkSchemaVersion] schema 版本" << version << "低于要求的" << required
                         << "— 请先执行 Server/migrate_ext_file_001.sql";
        return false;
    }
    qCDebug(lcDb) << "[checkSchemaVersion] OK, version" << version;
    return true;
}

// ===== REQ-EXT-FILE-001 T6：存量迁移（design §7：幂等、失败不删旧、file_migration_map 双向映射）=====
void OperateDB::migrateLegacyFiles()
{
    {
        PooledSqlConnection probe = DatabaseConnectionPool::instance().acquire();
        if (!probe.isValid()) {
            qCCritical(lcDb) << "[migrateLegacy] 数据库不可达，跳过存量迁移";
            return;
        }
    }
    const QString fsRoot = QDir(Server::getinstance().filesPath).absolutePath();
    // F-3/D5 修复：映射键存 design §2 规定的相对形态 "<filesys>/<user>/..."（跨树确定、可重放），
    // 机器绝对前缀不再进入 file_migration_map。
    const QString fsBase = QFileInfo(fsRoot).fileName();
    int mapped = 0, skipped = 0, failed = 0;
    const QStringList userDirs = QDir(fsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& user : userDirs) {
        if (user == "objects" || user == "tmp" || user == ".staging") {
            continue;   // 非用户根目录（D2 布局/暂存区）
        }
        QDirIterator it(QDir(fsRoot).filePath(user), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString oldAbs = QDir::cleanPath(it.next());
            bool artifact = false;
            const QStringList segs = oldAbs.split('/');
            for (const QString& seg : segs) {
                if (seg == "objects" || seg == ".staging" || seg.endsWith(".part")) {
                    artifact = true;
                    break;
                }
            }
            if (artifact) continue;   // 内容寻址对象与暂存实体不是迁移对象
            const QString rel = QDir(fsRoot).relativeFilePath(oldAbs);
            if (rel.isEmpty() || rel.startsWith("..")) {
                qCWarning(lcDb) << "[migrateLegacy] 文件不在 filesys 根内，未迁移:" << oldAbs;
                ++failed;
                continue;
            }
            const QString oldKey = fsBase + "/" + rel;      // design §2: filesys/<user>/... 相对键
            if (oldKey.size() > 191) {
                qCWarning(lcDb) << "[migrateLegacy] 相对路径超 file_migration_map 列宽(191)，未迁移:" << oldKey;
                ++failed;
                continue;
            }
            bool already = false;
            {
                PooledSqlConnection c = DatabaseConnectionPool::instance().acquire();
                if (!c.isValid()) { qCCritical(lcDb) << "[migrateLegacy] 数据库不可达，迁移中断（旧文件零删除）"; return; }
                QSqlQuery q(c.database());
                q.prepare("SELECT status FROM file_migration_map WHERE old_path=?");
                q.addBindValue(oldKey);
                if (q.exec() && q.next()) already = true;   // 重跑：已有映射记录即跳过（failed 留待人工复核）
            }
            if (already) { ++skipped; continue; }

            const QFileInfo fi(oldAbs);
            const FileObjectStore::CommitResult r = FileObjectStore::instance().commitUpload(
                user, fi.absolutePath(), fi.fileName(), fi.size(), oldAbs, /*copyOnly=*/true);
            PooledSqlConnection c2 = DatabaseConnectionPool::instance().acquire();
            if (!c2.isValid()) { qCCritical(lcDb) << "[migrateLegacy] 数据库不可达，迁移中断（旧文件零删除）"; return; }
            QSqlQuery m(c2.database());
            if (r.ok) {
                // 同名不同内容：不覆盖——条目已按 (1) 安置，映射记 skipped_name_conflict（验收4 后半）
                const QString status = (r.displayName == fi.fileName()) ? "mapped" : "skipped_name_conflict";
                m.prepare("INSERT INTO file_migration_map (old_path,file_id,object_id,status) VALUES (?,?,?,?)");
                m.addBindValue(oldKey);
                m.addBindValue(r.fileId);
                m.addBindValue(r.objectId);
                m.addBindValue(status);
                if (m.exec()) {
                    ++mapped;
                } else {
                    qCWarning(lcDb) << "[migrateLegacy] 映射行写入失败:" << oldAbs << m.lastError().text();
                    ++failed;
                }
            } else {
                // 任一文件失败：记 failed，继续其余，不中断、不删任何旧文件（验收5）
                qCWarning(lcDb) << "[migrateLegacy] 迁移失败:" << oldAbs << r.error;
                m.prepare("INSERT INTO file_migration_map (old_path,file_id,object_id,status) VALUES (?,?,NULL,'failed')");
                m.addBindValue(oldKey);
                m.exec();
                ++failed;
            }
        }
    }
    if (mapped || skipped || failed) {
        qCDebug(lcDb) << "[migrateLegacy] mapped" << mapped << "skipped" << skipped << "failed" << failed;
    }
    if (failed > 0) {
        qCWarning(lcDb) << "[migrateLegacy] 存在 failed/超长记录——进 FILE-002/GROUP-003 前必须复核 file_migration_map";
    }
}

// ===== REQ-EXT-FILE-001 F-3/D5 修复：存量键一次性确定性转换（幂等可重放）=====
// user_file/upload_session.parent_path 的旧"绝对机器路径"形态 → owner 相对逻辑键；
// file_migration_map.old_path → design §2 相对形态 "<filesys>/<user>/..."。
// 转换后同一逻辑请求在任何工作树命中同一行（跨树幂等/分享/隔离成立）。
// uk_loc 冲突：按 file_id 最早行保留，后到 active 行软删并镜像 releaseEntry 减引用（D3 不物理删）。
void OperateDB::normalizeLogicKeys()
{
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) {
        qCCritical(lcDb) << "[normalizeLogicKeys] 数据库不可达，跳过存量键转换";
        return;
    }
    QSqlDatabase db = conn.database();
    const QString fsRoot = QDir(Server::getinstance().filesPath).absolutePath();
    const QString fsBase = QFileInfo(fsRoot).fileName();
    int ufConverted = 0, ufCollapsed = 0, ufSkippedDeleted = 0, sessConverted = 0;
    int mapConverted = 0, mapDupRemoved = 0, unmapped = 0;

    // 1) user_file——F-5 修复：单遍确定性。判别不依赖首字符 '/'（覆盖 "/filesys/..." 无盘符
    //    旧形态）：canonical 行（'/' 开头且 '/' 结尾且无 "//"）预种子为冲突基准；其余行按
    //    file_id 单遍——撞键时 **不改写 loser 的 parent_path**（改写=与存活行同三重撞
    //    uk_loc→1062 死锁在旧键）。存活优先级：canonical 行 > 最早旧行；loser active→仅
    //    软删+镜像 releaseEntry 减引用（D3 不物理删），deleted→静默跳过（重放稳定无告警）。
    QSet<QString> seenUf;
    {
        QSqlQuery s(db);
        if (s.exec("SELECT owner_name,parent_path,display_name FROM user_file "
                   "WHERE parent_path LIKE '/%' AND parent_path LIKE '%/' AND parent_path NOT LIKE '%//%'")) {
            while (s.next())
                seenUf.insert(s.value(0).toString() + "|" + s.value(1).toString() + "|" + s.value(2).toString());
        }
    }
    {
        QSqlQuery rows(db);
        if (rows.exec("SELECT file_id,owner_name,parent_path,display_name,object_id,status FROM user_file "
                      "WHERE parent_path NOT LIKE '/%' OR parent_path NOT LIKE '%/' OR parent_path LIKE '%//%' "
                      "ORDER BY file_id")) {
            QVector<QVariantList> batch;
            while (rows.next()) {
                QVariantList r;
                for (int i = 0; i < 6; ++i) r.append(rows.value(i));
                batch.append(r);
            }
            for (const QVariantList& r : batch) {
                const qint64 fid = r.at(0).toLongLong();
                const QString owner = r.at(1).toString();
                const QString stored = r.at(2).toString();
                const QString name = r.at(3).toString();
                const QVariant obj = r.at(4);
                const bool active = (r.at(5).toString() == "active");
                const QString key = FileObjectStore::normalizeParent(owner, stored);
                if (key.isEmpty()) {
                    ++unmapped;
                    qCWarning(lcDb) << "[normalizeLogicKeys] user_file 行无法映射，保留原值" << fid << owner << stored;
                    continue;
                }
                if (key == stored) {   // 语义已是逻辑键（形态未 canonical）：仅登记
                    seenUf.insert(owner + "|" + key + "|" + name);
                    continue;
                }
                const QString triple = owner + "|" + key + "|" + name;
                QSqlQuery u(db);
                if (seenUf.contains(triple)) {
                    if (!active) { ++ufSkippedDeleted; continue; }   // 重放稳定：不再触碰、不再告警
                    u.prepare("UPDATE user_file SET status='deleted' WHERE file_id=?");
                    u.addBindValue(fid);
                    if (!u.exec()) { qCWarning(lcDb) << "[normalizeLogicKeys] 冲突收敛失败" << fid << u.lastError().text(); continue; }
                    if (!obj.isNull()) {
                        QSqlQuery d(db);
                        d.prepare("UPDATE file_object SET refcount=GREATEST(refcount-1,0),"
                                  "status=IF(refcount<=0,'pending_reclaim','ready') WHERE object_id=?");
                        d.addBindValue(obj.toLongLong());
                        if (!d.exec()) qCWarning(lcDb) << "[normalizeLogicKeys] 收敛减引用失败" << obj.toLongLong() << d.lastError().text();
                    }
                    ++ufCollapsed;
                } else {
                    u.prepare("UPDATE user_file SET parent_path=? WHERE file_id=?");
                    u.addBindValue(key);
                    u.addBindValue(fid);
                    if (!u.exec()) { qCWarning(lcDb) << "[normalizeLogicKeys] 转换失败" << fid << u.lastError().text(); continue; }
                    seenUf.insert(triple);
                    ++ufConverted;
                }
            }
        }
    }

    // 2) upload_session：同判别（无唯一键，可改写则改写；与会话行同规则跳过 canonical）
    {
        QSqlQuery rows(db);
        if (rows.exec("SELECT session_id,owner_name,parent_path FROM upload_session "
                      "WHERE parent_path NOT LIKE '/%' OR parent_path NOT LIKE '%/' OR parent_path LIKE '%//%' "
                      "ORDER BY session_id")) {
            QVector<QVariantList> batch;
            while (rows.next()) {
                QVariantList r;
                for (int i = 0; i < 3; ++i) r.append(rows.value(i));
                batch.append(r);
            }
            for (const QVariantList& r : batch) {
                const QString key = FileObjectStore::normalizeParent(r.at(1).toString(), r.at(2).toString());
                if (key == r.at(2).toString()) continue;
                if (key.isEmpty()) { ++unmapped; continue; }
                QSqlQuery u(db);
                u.prepare("UPDATE upload_session SET parent_path=? WHERE session_id=?");
                u.addBindValue(key);
                u.addBindValue(r.at(0).toLongLong());
                if (u.exec()) ++sessConverted;
            }
        }
    }

    // 3) file_migration_map：机器绝对 old_path → "<filesys>/..." 相对键；
    //    转换后撞主键（两棵树映射同一旧文件）：按 migrated_at,old_path 序保留先者，删后者。
    {
        QVector<QString> legacy;
        QSqlQuery all(db);
        if (all.exec("SELECT old_path FROM file_migration_map ORDER BY migrated_at, old_path")) {
            while (all.next()) {
                const QString op = all.value(0).toString();
                if (!op.startsWith(fsBase + "/")) legacy.append(op);
            }
        }
        for (const QString& op : legacy) {
            QString n = op;
            n.replace('\\', '/');
            const QStringList segs = QDir::cleanPath(n).split('/', Qt::SkipEmptyParts);
            int idx = -1;
            for (int i = 0; i < segs.size(); ++i) {
                if (segs.at(i).compare(fsBase, Qt::CaseInsensitive) == 0) { idx = i; break; }
            }
            if (idx < 0) {
                ++unmapped;
                qCWarning(lcDb) << "[normalizeLogicKeys] 映射行锚定失败，保留原值" << op;
                continue;
            }
            const QString newKey = QStringList(segs.mid(idx)).join('/');
            if (newKey.size() > 191) { ++unmapped; continue; }
            QSqlQuery c(db);
            c.prepare("SELECT COUNT(*) FROM file_migration_map WHERE old_path=?");
            c.addBindValue(newKey);
            bool taken = false;
            if (c.exec() && c.next()) taken = (c.value(0).toLongLong() > 0);
            QSqlQuery u(db);
            if (taken) {
                u.prepare("DELETE FROM file_migration_map WHERE old_path=?");
                u.addBindValue(op);
                if (u.exec()) ++mapDupRemoved;
            } else {
                u.prepare("UPDATE file_migration_map SET old_path=? WHERE old_path=?");
                u.addBindValue(newKey);
                u.addBindValue(op);
                if (u.exec()) ++mapConverted;
            }
        }
    }

    if (ufConverted || ufCollapsed || ufSkippedDeleted || sessConverted || mapConverted || mapDupRemoved || unmapped) {
        qCDebug(lcDb) << "[normalizeLogicKeys] user_file" << ufConverted << "收敛" << ufCollapsed
                      << "deleted跳过" << ufSkippedDeleted
                      << "upload_session" << sessConverted
                      << "map" << mapConverted << "去重删" << mapDupRemoved
                      << "无法映射" << unmapped;
    }
}

// ===== REQ-EXT-FILE-001 T4：upload_session 持久化（staging 归属/offset/过期）=====
qint64 OperateDB::createUploadSession(const QString &owner, const QString &parentPath,
                                      const QString &displayName, qint64 expectedSize,
                                      const QString &clientSha256, const QString &stagingPath,
                                      qint64 confirmedOffset, int ttlSec)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return -1;
    QSqlQuery q(connection.database());
    q.prepare("INSERT INTO upload_session (owner_name,parent_path,display_name,expected_size,"
              "client_sha256,staging_path,confirmed_offset,status,expires_at) "
              "VALUES (?,?,?,?,?,?,?,'uploading',?)");
    q.addBindValue(owner);
    q.addBindValue(parentPath);
    q.addBindValue(displayName);
    q.addBindValue(expectedSize);
    q.addBindValue(clientSha256.isEmpty() ? QVariant() : QVariant(clientSha256));
    q.addBindValue(stagingPath);
    q.addBindValue(confirmedOffset);
    q.addBindValue(QDateTime::currentDateTime().addSecs(ttlSec > 0 ? ttlSec : 3600)
                       .toString("yyyy-MM-dd HH:mm:ss"));
    if(!q.exec()){
        qCWarning(lcDb) << "[createUploadSession] 失败:" << q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

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

bool OperateDB::finishUploadSession(qint64 sessionId, const QString &status)
{
    if(sessionId < 0) return false;
    if(status != "done" && status != "failed" && status != "expired" && status != "verifying")
        return false;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("UPDATE upload_session SET status=? WHERE session_id=?");
    q.addBindValue(status);
    q.addBindValue(sessionId);
    return q.exec();
}

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
    q.addBindValue(applyPasswordSalt(QString::fromUtf8(strpwd), salt));
    q.addBindValue(salt);
    qCDebug(lcDb)<<"insert new user:"<<QString::fromUtf8(strname);
    return q.exec();
}

bool OperateDB::createShareLink(const QString &hash, const QString &owner, const QString &path, const QDateTime &expire)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        qCWarning(lcDb) << "[createShareLink] 获取数据库连接失败（连接无效）";
        return false;
    }
    QSqlQuery q(connection.database());
    // 查重：同一 owner + 同一文件若已存在链接，则直接复用，避免重复记录堆积
    if(!getShareLinkByPath(owner, path).isEmpty()){
        qCDebug(lcDb) << "[createShareLink] 该文件已存在分享链接，复用已有记录: owner=" << owner << " path=" << path;
        return true;
    }
    q.prepare("INSERT INTO share_link (token_hash, owner_name, relative_path, expire_time) VALUES (?, ?, ?, ?)");
    q.addBindValue(hash);
    q.addBindValue(owner);
    q.addBindValue(path);
    // 必须显式格式化为 "yyyy-MM-dd HH:mm:ss" 字符串再绑定。
    // 直接绑定 QDateTime 时，Qt MySQL 驱动会序列化成 ISO 8601 格式（带 'T' 分隔符，如
    // 2026-07-29T18:00:00），若时间含毫秒/时区，MySQL 可能拒绝或存错。
    // Qt 5.14 的 QMYSQL 驱动在绑定 QDateTime 时会输出 ISO 格式（带 T / 毫秒），
    // MySQL 的 DATETIME 在严格模式下会拒绝带 T 的值，导致插入失败。
    // 这里显式格式化为 MySQL 原生 DATETIME 字符串，避免该问题。
    q.addBindValue(expire.toString("yyyy-MM-dd HH:mm:ss"));
    if(!q.exec()){
        qCWarning(lcDb) << "[createShareLink] 插入分享链接失败:" << q.lastError().text()
                 << " | hash=" << hash << " owner=" << owner << " path=" << path;
        return false;
    }
    return true;
}

bool OperateDB::getShareLinkByHash(const QString &hash, ShareLinkInfo &outInfo)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("SELECT owner_name, relative_path, expire_time, revoked FROM share_link WHERE token_hash = ?");
    q.addBindValue(hash);
    if(q.exec() && q.next()){
        outInfo.ownerName = q.value(0).toString();
        outInfo.relativePath = q.value(1).toString();
        outInfo.expireTime = q.value(2).toDateTime();
        outInfo.revoked = q.value(3).toInt();
        return true;
    }
    return false;
}

QString OperateDB::getShareLinkByPath(const QString &owner, const QString &path)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return QString();
    QSqlQuery q(connection.database());
    q.prepare("SELECT token_hash FROM share_link WHERE owner_name = ? AND relative_path = ? ORDER BY created_at DESC LIMIT 1");
    q.addBindValue(owner);
    q.addBindValue(path);
    if(q.exec() && q.next()){
        return q.value(0).toString();
    }
    return QString();
}

// 跨功能联动：删除文件/目录时，清理对应的分享链接记录
// path 为相对 owner 根目录的路径；若为目录则同时清理其下所有文件（前缀匹配）
bool OperateDB::deleteShareLinkByPath(const QString &owner, const QString &path)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("DELETE FROM share_link WHERE owner_name = ? AND (relative_path = ? OR relative_path LIKE ?)");
    q.addBindValue(owner);
    q.addBindValue(path);
    q.addBindValue(path + "/%");
    bool ok = q.exec();
    if(!ok){
        qCWarning(lcDb) << "[deleteShareLinkByPath] 失败:" << q.lastError().text()
                 << " owner=" << owner << " path=" << path;
    }
    return ok;
}

// 跨功能联动：重命名/移动文件时，平移 share_link.relative_path
// - 精确匹配 oldPath（文件自身）
// - 前缀匹配 oldPath/（目录下的子文件整体平移）
bool OperateDB::updateShareLinkPath(const QString &owner, const QString &oldPath, const QString &newPath)
{
    if(oldPath == newPath) return true;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlDatabase db = connection.database();

    // 1) 文件自身：relative_path = oldPath → newPath
    QSqlQuery q1(db);
    q1.prepare("UPDATE share_link SET relative_path = ? WHERE owner_name = ? AND relative_path = ?");
    q1.addBindValue(newPath);
    q1.addBindValue(owner);
    q1.addBindValue(oldPath);
    bool ok = q1.exec();
    if(!ok){
        qCWarning(lcDb) << "[updateShareLinkPath] 精确更新失败:" << q1.lastError().text()
                 << " owner=" << owner << " old=" << oldPath << " new=" << newPath;
    }

    // 2) 目录子文件：oldPath/sub → newPath/sub（仅匹配严格长于 oldPath 的条目）
    if(!oldPath.isEmpty()){
        QSqlQuery q2(db);
        q2.prepare("UPDATE share_link SET relative_path = CONCAT(?, SUBSTRING(relative_path, ?)) "
                   "WHERE owner_name = ? AND relative_path LIKE ? AND relative_path <> ?");
        q2.addBindValue(newPath + "/");
        q2.addBindValue(oldPath.size() + 2);          // 1-based：跳过 "oldPath/" 之后
        q2.addBindValue(owner);
        q2.addBindValue(oldPath + "/%");
        q2.addBindValue(oldPath);
        if(!q2.exec()){
            qCWarning(lcDb) << "[updateShareLinkPath] 目录前缀平移失败:" << q2.lastError().text();
            ok = false;
        }
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

int OperateDB::handleLogin(char *strname, char *strpwd)
{
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
    const QString clientHash = QString::fromUtf8(strpwd);

    // 按用户名取存储哈希与盐（salt 为空 = 迁移前老数据）
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
    QString effectiveHash = applyPasswordSalt(clientHash, salt);
    if(effectiveHash != storedPwd){
        qCWarning(lcDb)<<"user name or password not found";
        return -1;
    }

    // 老用户登录成功 → 自动补盐升级（条件更新，不覆盖并发下已写入的新盐）
    if(salt.isEmpty()){
        QSqlQuery upgrade(db);
        const QString newSalt = generatePasswordSalt();
        const QString newHash = applyPasswordSalt(clientHash, newSalt);
        upgrade.prepare("UPDATE user_info SET pwd = ?, salt = ? WHERE name = ? AND salt = ''");
        upgrade.addBindValue(newHash);
        upgrade.addBindValue(newSalt);
        upgrade.addBindValue(nameParam);
        if(upgrade.exec() && upgrade.numRowsAffected() > 0){
            effectiveHash = newHash;
        }
    }

    q.prepare("update user_info set online = 1 where name = ? and pwd = ?");
    q.addBindValue(nameParam);
    q.addBindValue(effectiveHash);
    qCDebug(lcDb)<<"login update online for:"<<nameParam;
    if (!q.exec()) {
        qCCritical(lcDb) << "UPDATE online status failed:" << q.lastError();
        return 0;
    }
    return 1;
}

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
    q.exec();
    if(q.next()){
        return q.value(3).toInt();
    }
    return 2;
}

QStringList OperateDB::handleOnlineUsers(const QString &strname)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return QStringList();
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    // 查询在线且不是自己，且不在好友表中的用户
    q.prepare(R"(SELECT name FROM user_info 
                 WHERE online = 1 AND name != ? 
                 AND name NOT IN (
                     SELECT friendName FROM friend WHERE name = ?
                     UNION
                     SELECT name FROM friend WHERE friendName = ?
                 ))");
    q.addBindValue(strname);
    q.addBindValue(strname);
    q.addBindValue(strname);
    qCDebug(lcDb)<<"online users query for:"<<strname;
    q.exec();

    QStringList ret;
    while(q.next())
    {
        ret.append(q.value(0).toString());
    }
    return ret;
}

int OperateDB::handleAddFriend(char *usrname, char *tarname)
{
    if(usrname==nullptr||tarname==nullptr){
        return -1;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return -1;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("select * from user_info where name = ? and online = 1");
    q.addBindValue(QString::fromUtf8(tarname));
    q.exec();
    if(q.next()){
        return 1;
    }
    return 0;
}

int OperateDB::handleAddFriendFin(char *usrname, char *tarname)
{
    if(usrname==nullptr||tarname==nullptr){
        return -1;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return -1;
    }
    QSqlDatabase db = connection.database();
    
    // 开启事务
    db.transaction();
    QSqlQuery q(db);

    // 使用两个名字字段存储好友关系
    q.prepare("INSERT INTO friend (name, friendName) VALUES (?, ?)");
    q.addBindValue(QString::fromUtf8(usrname));
    q.addBindValue(QString::fromUtf8(tarname));
    if(!q.exec()) {
        db.rollback();
        return 0;
    }

    if(!db.commit()) {
        db.rollback();
        return 0;
    }
    
    qCDebug(lcDb)<<"AddFriendFin:"<<QString::fromUtf8(usrname)<<"<->"<<QString::fromUtf8(tarname);
    return 1;
}

QStringList OperateDB::handleFlushFriend(const QString &strname)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return QStringList();
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    // 查询 name=strname 时的 friendName，以及 friendName=strname 时的 name
    q.prepare(R"(SELECT friendName FROM friend WHERE name = ? 
                 UNION 
                 SELECT name FROM friend WHERE friendName = ?)");
    q.addBindValue(strname);
    q.addBindValue(strname);
    q.exec();

    QStringList ret;
    while(q.next()){
        ret.append(q.value(0).toString());
    }
    return ret;
}

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

bool OperateDB::seedchatMsg(char *seedName, char *revName, QString msg, QString seedTime)
{
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
    q.addBindValue(ans ? 1 : 0);
    q.addBindValue(seedTime);
    bool execRet=q.exec();
    qCDebug(lcDb)<<"chat msg insert:"<<execRet;
    return ans;
}

QStringList OperateDB::getchatMsg(char *seedName, char *revName)
{
    QStringList ret;
    if(seedName==nullptr||revName==nullptr){
        return ret;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return ret;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("select msg,seed_time from message_info where seed_name = ? and rev_name = ? and issuccess = 0");
    q.addBindValue(QString::fromUtf8(seedName));
    q.addBindValue(QString::fromUtf8(revName));
    qCDebug(lcDb)<<"getchatMsg for:"<<QString::fromUtf8(seedName)<<"->"<<QString::fromUtf8(revName);
    if(!q.exec()){
        return ret;
    }
    while(q.next()){
        QString retItem1(q.value(0).toString());
        QString retItem2(q.value(1).toString());
        ret.append(retItem1);
        ret.append(retItem2);
    }

    q.prepare("update message_info set issuccess = 1 where seed_name = ? and rev_name = ?");
    q.addBindValue(QString::fromUtf8(seedName));
    q.addBindValue(QString::fromUtf8(revName));
    qCDebug(lcDb)<<"update msg issuccess=1 ret:"<<q.exec();
    return ret;
}

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

    // 1. 按用户名取哈希与盐后校验密码（salt 为空 = 迁移前老数据，按裸哈希比对）
    q.prepare("SELECT pwd, salt FROM user_info WHERE name = ?");
    q.addBindValue(QString::fromUtf8(name));
    if(!q.exec() || !q.next()){
        qCWarning(lcDb)<<"logoff: user/pwd mismatch or user not found:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }
    const QString effectiveHash = applyPasswordSalt(QString::fromUtf8(pwd), q.value(1).toString());
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

    if(!db.commit()){
        qCWarning(lcDb)<<"logoff: commit failed for:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    qCDebug(lcDb)<<"logoff: successfully deleted user:"<<QString::fromUtf8(name);
    return true;
}

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
