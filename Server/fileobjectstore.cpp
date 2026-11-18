#include "logging.h"
#include "fileobjectstore.h"
#include "databaseconnectionpool.h"
#include "operatedb.h"
#include "server.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QVariant>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

// fileobjectstore.cpp — 内容寻址对象存储实现。
// 关键不变量：staging 的大小/SHA-256 复算通过后才 rename 进 objects/；DB 事务失败时
// 尽力把已搬移的对象文件搬回 staging；物理对象只在 refcount 归零且被调度器抢权后删除。
// 所有路径参数先经 normalizeParent 归一为 owner 相对逻辑键，再写入 user_file.parent_path。

namespace {
const int kMaxParentChars = 150;   // user_file.parent_path 列宽
const int kMaxNameBytes   = 70;    // display_name 列宽（UTF-8 字节）
const int kNameSuffixCap  = 100;   // "(N)" 递推上限

QString clampUtf8(const QString& s, int maxBytes);

QString compensationPath(const QString& path)
{
    if (path.isEmpty()) return QString();
    const QString root = QDir(Server::getinstance().filesPath).absolutePath();
    const QString rel = QDir(root).relativeFilePath(QFileInfo(path).absoluteFilePath());
    return (rel.startsWith("..") || rel.startsWith('/'))
            ? QStringLiteral("__INVALID_PATH__") : rel;
}

void enqueueStorageFailure(const QString& type, const QString& owner,
                           const QString& source, const QString& target,
                           const QString& error)
{
    if (owner.isEmpty()) return;
    OperateDB::instance().enqueueDirectoryCompensation(
        type, owner, compensationPath(source), compensationPath(target),
        0, QString(), QString(), 0, error);
}

// 拆分扩展名：最后一个 '.' 之后为 ext；点位于首位或不存在时整体视为 base。
void splitName(const QString& name, QString& base, QString& ext)
{
    const int dot = name.lastIndexOf('.');
    if (dot > 0) {
        base = name.left(dot);
        ext = name.mid(dot);
    } else {
        base = name;
        ext.clear();
    }
}

// 按字符截断到 maxBytes（UTF-8），避免劈开多字节字符
QString clampUtf8(const QString& s, int maxBytes)
{
    if (s.toUtf8().size() <= maxBytes) return s;
    QString out;
    int budget = maxBytes;
    for (const QChar& ch : s) {
        const int w = QString(ch).toUtf8().size();
        if (w > budget) break;
        out.append(ch);
        budget -= w;
    }
    return out;
}

// 同名不同内容递推：base 截短到容纳 "(N)"+ext，避免劈开多字节
QString makeSuffixed(const QString& base, const QString& ext, int n)
{
    const int suffixBytes = QString("(%1)").arg(n).toUtf8().size() + ext.toUtf8().size();
    return clampUtf8(base, kMaxNameBytes - suffixBytes) + QString("(%1)").arg(n) + ext;
}

// 逻辑键形态判定：'/' 开头结尾、无反斜杠、无空段、无 ./.. 段（normalizeParent 幂等入口）
// REQ-EXT-FILE-002 L1/L2 收紧：'/filesys/<owner>/...' 与带盘符（含 ':'）的旧行过去被判为
// canonical，被 normalizeParent 原样回显、静默保留——数据对查询不可见。两者一律判非 canonical，
// 交回 normalizeParent 走段锚定转换（走转换而非静默保留）。
bool isLogicKeyForm(const QString& x)
{
    if (!x.startsWith(QLatin1Char('/')) || !x.endsWith(QLatin1Char('/')) || x.contains(QLatin1Char('\\')))
        return false;
    if (x == QLatin1String("/"))
        return true;
    if (x.contains(QLatin1String("//")))
        return false;
    if (x.contains(QLatin1Char(':')))
        return false;                                   // 带盘符旧行（"/E:/filesys/<owner>/..."）
    const int slash = x.indexOf(QLatin1Char('/'), 1);
    const QString first = (slash < 0) ? x.mid(1) : x.mid(1, slash - 1);
    if (!first.isEmpty()) {
        const QString fsBase = QFileInfo(QDir(Server::getinstance().filesPath).absolutePath()).fileName();
        if (!fsBase.isEmpty() && first.compare(fsBase, Qt::CaseInsensitive) == 0)
            return false;                               // "/filesys/<owner>/..." 形态
    }
    const QStringList segs = x.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& s : segs) {
        if (s == QLatin1String(".") || s == QLatin1String(".."))
            return false;
    }
    return true;
}

// 逻辑键拼接：parentKey（'/' 结尾）+ 目录名 → 目录自身键（'/' 结尾）
QString logicDirKey(const QString& parentKey, const QString& name)
{
    return parentKey + name + QLatin1Char('/');
}

// 同目录下是否存在同名 active 条目（软删行不占名，可被复活）。
bool nameTaken(const QString& owner, const QString& parent, const QString& display, QSqlDatabase& db)
{
    QSqlQuery q(db);
    q.prepare("SELECT file_id FROM user_file WHERE owner_name=? AND parent_path=? AND display_name=? AND status='active' LIMIT 1");
    q.addBindValue(owner);
    q.addBindValue(parent);
    q.addBindValue(display);
    return q.exec() && q.next();
}
}

// 函数内静态对象：C++11 起线程安全，保证进程内唯一实例。
FileObjectStore& FileObjectStore::instance()
{
    static FileObjectStore s;
    return s;
}

FileObjectStore::FileObjectStore(QObject* parent) : QObject(parent) {}

// F-3/D5 修复：不再绝对化（绝对化=工作树相关，跨树键漂移的根因）。
// 归一为 owner 相对逻辑键："用户根内、'/' 结尾"，机器无关、可跨树重放。
QString FileObjectStore::normalizeParent(const QString& owner, const QString& dirPath)
{
    if (owner.isEmpty() || dirPath.isEmpty()) {
        return QString();
    }
    QString x = dirPath;
    x.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (isLogicKeyForm(x)) {
        return x;                                   // 幂等：已是逻辑键
    }
    x = QDir::cleanPath(x);
    if (x.isEmpty()) {
        return QString();
    }

    const QString fsRoot = QDir(Server::getinstance().filesPath).absolutePath();
    const QString rootAbs = fsRoot + QLatin1Char('/') + owner;
    QString rest;
    bool hit = false;
    if (x == rootAbs) {
        hit = true;                                 // actor 用户根 → "/"
    } else if (x.startsWith(rootAbs + QLatin1Char('/'))) {
        rest = x.mid(rootAbs.size() + 1);
        hit = true;
    } else {
        // 相对形态（PDU "./filesys/<user>/..."）与任意树前缀统一走段锚定
        const QString fsBase = QFileInfo(fsRoot).fileName();   // 如 "filesys"
        const QStringList segs = x.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (int i = 0; i + 1 < segs.size(); ++i) {
            if (segs.at(i).compare(fsBase, Qt::CaseInsensitive) == 0
                && segs.at(i + 1).compare(owner, Qt::CaseInsensitive) == 0) {
                rest = QStringList(segs.mid(i + 2)).join(QLatin1Char('/'));
                hit = true;
                break;
            }
        }
    }
    if (!hit) {
        return QString();                           // 不含 <filesys>/<owner> 段：不在 actor 根，无效
    }
    rest = QDir::cleanPath(rest);
    if (rest.isEmpty() || rest == QLatin1String(".")) {
        return QStringLiteral("/");
    }
    if (rest == QLatin1String("..") || rest.startsWith(QLatin1String("../"))
        || rest.contains(QLatin1String("/../"))) {
        return QString();                           // 词法逃逸纵深防御（入口授权应先拦截）
    }
    return QLatin1Char('/') + rest + QLatin1Char('/');
}

// 流式复算暂存文件：1MiB 缓冲避免整文件读入内存，同时得到实际字节数。
bool FileObjectStore::hashStaging(const QString& stagingPath, QString& shaHex, qint64& sizeBytes) const
{
    QFile f(stagingPath);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    qint64 total = 0;
    const qint64 chunk = 1024 * 1024;
    while (!f.atEnd()) {
        const QByteArray buf = f.read(chunk);
        if (buf.isEmpty() && f.error() != QFile::NoError) {
            return false;
        }
        h.addData(buf);
        total += buf.size();
    }
    shaHex = QString::fromLatin1(h.result().toHex());
    sizeBytes = total;
    return true;
}

// REQ-EXT-FILE-002 §2 步骤4：摘要+大小预检查（去重命中判定）。
// 命中返回 object_id，未命中返回 -1。物理对象唯一性由 uk_sha256 保证，此处只读。
qint64 FileObjectStore::findObjectBySha(const QString& shaHex, qint64 sizeBytes) const
{
    if (shaHex.isEmpty() || sizeBytes < 0) return -1;
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) return -1;
    QSqlQuery q(conn.database());
    q.prepare("SELECT object_id FROM file_object WHERE sha256=? AND size_bytes=? FOR UPDATE");
    q.addBindValue(shaHex);
    q.addBindValue(sizeBytes);
    if (!q.exec() || !q.next()) return -1;
    return q.value(0).toLongLong();
}

// storage_path 是相对 filesys 的路径；objects_tmp/ 占位表示对象尚未转正，返回空。
QString FileObjectStore::objectAbsPath(qint64 objectId) const
{
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) return QString();
    QSqlQuery q(conn.database());
    q.prepare("SELECT storage_path FROM file_object WHERE object_id=?");
    q.addBindValue(objectId);
    if (!q.exec() || !q.next()) {
        return QString();
    }
    const QString rel = q.value(0).toString();
    if (rel.isEmpty() || rel.startsWith("objects_tmp/")) {
        return QString();
    }
    return QDir(Server::getinstance().filesPath).absoluteFilePath(rel);
}

FileObjectStore::CommitResult FileObjectStore::commitUpload(
        const QString& owner, const QString& rawParentPath,
        const QString& wantedName, qint64 expectedSize, const QString& stagingPath,
        bool copyOnly, const QString& expectedSha)
{
    // REQ-EXT-PERF-002：同步入口保留给线程池外的调用方（如 T6 存量迁移 copyOnly）；
    // 上传转正路径改为线程池复算后调用 commitUploadPreHashed，主线程不再等待整文件哈希。
    CommitResult r;
    r.displayName = wantedName;
    QString sha;
    qint64 size = 0;
    if (!hashStaging(stagingPath, sha, size)) {
        r.error = "staging 复算失败";
        return r;
    }
    return commitUploadPreHashed(owner, rawParentPath, wantedName, expectedSize,
                                 stagingPath, copyOnly, expectedSha, sha, size);
}

// REQ-EXT-PERF-002：复算（computedSha/computedSize）已在线程池完成后的转正提交。
// 大小/摘要守卫、事务与文件搬移语义与 commitUpload 逐字节一致——复算只是换了执行线程。
FileObjectStore::CommitResult FileObjectStore::commitUploadPreHashed(
        const QString& owner, const QString& rawParentPath,
        const QString& wantedName, qint64 expectedSize, const QString& stagingPath,
        bool copyOnly, const QString& expectedSha,
        const QString& computedSha, qint64 computedSize)
{
    CommitResult r;
    r.displayName = wantedName;
    if (owner.isEmpty() || wantedName.isEmpty() || expectedSize < 0) {
        r.error = "参数为空";
        return r;
    }
    const QString parent = normalizeParent(owner, rawParentPath);
    if (parent.isEmpty() || parent.size() > kMaxParentChars) {
        r.error = "父路径规范化失败或超长";
        qCWarning(lcFs) << "[commitUpload]" << r.error << rawParentPath;
        return r;
    }

    const QString& sha = computedSha;
    const qint64 size = computedSize;
    if (size != expectedSize) {
        r.error = "复算大小与登记不符，拒绝转正";
        qCWarning(lcFs) << "[commitUpload] conn-owner" << owner
                        << "expected" << expectedSize << "actual" << size;
        if (!copyOnly && QFileInfo::exists(stagingPath) && !QFile::remove(stagingPath)) {
            enqueueStorageFailure(QStringLiteral("delete_staging_file"), owner,
                                  stagingPath, QString(), QStringLiteral("复算大小失败后的暂存清理失败"));
        }   // T6：迁移源=用户旧文件，绝不删除（验收5）
        return r;
    }
    // REQ-EXT-FILE-002 D2：客户端声明的摘要只用于命中判定，这里是执行事实——
    // 与登记摘要（upload_session.client_sha256）不符一律不转正（验收3）。
    // expectedSha 为空 = 旧路径（客户端不提交摘要），行为与 FILE-001 逐字节等价。
    if (!expectedSha.isEmpty() && sha.compare(expectedSha, Qt::CaseInsensitive) != 0) {
        r.error = "复算摘要与登记不符，拒绝转正";
        qCWarning(lcFs) << "[commitUpload] 摘要不符 owner" << owner
                        << "expected" << expectedSha << "actual" << sha;
        if (!copyOnly && QFileInfo::exists(stagingPath) && !QFile::remove(stagingPath)) {
            enqueueStorageFailure(QStringLiteral("delete_staging_file"), owner,
                                  stagingPath, QString(), QStringLiteral("复算摘要失败后的暂存清理失败"));
        }
        return r;
    }

    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) {
        r.error = "数据库不可达";
        return r;
    }
    QSqlDatabase db = conn.database();
    if (!db.transaction()) {
        r.error = "事务开启失败";
        return r;
    }

    bool done = false;
    bool objectCreated = false;
    QString createdFinalAbs;          // rename 已搬移的对象文件——事务失败时尽力回退
    do {
        QSqlQuery q(db);
        // 1) 对象命中（sha 唯一键）——FOR UPDATE 防并发双建
        qint64 oid = -1;
        q.prepare("SELECT object_id FROM file_object WHERE sha256=? FOR UPDATE");
        q.addBindValue(sha);
        if (!q.exec()) { r.error = q.lastError().text(); break; }
        if (q.next()) {
            oid = q.value(0).toLongLong();
        } else {
            // 新对象在本事务内必然伴随一条新引用（条目失败会整体回滚）→ refcount=1
            q.prepare("INSERT INTO file_object (sha256,size_bytes,storage_path,refcount,status) VALUES (?,?,?,1,'ready')");
            q.addBindValue(sha);
            q.addBindValue(size);
            q.addBindValue("objects_tmp/pending");
            if (!q.exec()) { r.error = q.lastError().text(); break; }
            oid = q.lastInsertId().toLongLong();
            const QString rel = QString("objects/%1/%2").arg(sha.left(2)).arg(oid);
            q.prepare("UPDATE file_object SET storage_path=? WHERE object_id=?");
            q.addBindValue(rel);
            q.addBindValue(oid);
            if (!q.exec()) { r.error = q.lastError().text(); break; }
            const QString absRoot = QDir(Server::getinstance().filesPath).absolutePath();
            createdFinalAbs = QDir(absRoot).absoluteFilePath(rel);
            if (!QDir().mkpath(QFileInfo(createdFinalAbs).absolutePath())) {
                r.error = "对象目录创建失败";
                enqueueStorageFailure(QStringLiteral("create_object_directory"), owner,
                                      QString(), QFileInfo(createdFinalAbs).absolutePath(), r.error);
                break;
            }
            const bool moved = copyOnly
                    ? QFile::copy(stagingPath, createdFinalAbs)   // T6：旧实体复制为对象，源永不删除
                    : QFile::rename(stagingPath, createdFinalAbs);
            if (!moved) {
                r.error = copyOnly ? "对象转正 copy 失败" : "对象转正 rename 失败";
                enqueueStorageFailure(QStringLiteral("commit_staged_file"), owner,
                                      stagingPath, createdFinalAbs, r.error);
                qCWarning(lcFs) << "[commitUpload]" << r.error << createdFinalAbs;
                if (copyOnly && QFileInfo::exists(createdFinalAbs)
                        && !QFile::remove(createdFinalAbs)) {
                    enqueueStorageFailure(QStringLiteral("rollback_staged_file"), owner,
                                          createdFinalAbs, QString(), QStringLiteral("清理对象半成品失败"));
                }     // 清理半成品副本
                createdFinalAbs.clear();
                break;
            }
            objectCreated = true;
        }

        // 2) 同名安置：active 同名同对象=幂等命中（保留原名，步骤3 FOR UPDATE 命中即返回，
        //    验收2/跨树重复请求不建重复行、不重复计数）；active 同名不同对象（含目录占名）
        //    才递推 "base(N).ext"；deleted 同名不占名（步骤3 复活原行）。
        QString finalName = clampUtf8(wantedName, kMaxNameBytes);
        bool needSuffix = false;
        q.prepare("SELECT object_id,status FROM user_file "
                  "WHERE owner_name=? AND parent_path=? AND display_name=?");
        q.addBindValue(owner);
        q.addBindValue(parent);
        q.addBindValue(finalName);
        if (q.exec() && q.next() && q.value(1).toString() == "active") {
            const QVariant o = q.value(0);
            if (!(o.isValid() && !o.isNull() && o.toLongLong() == oid))
                needSuffix = true;
        }
        if (needSuffix) {
            QString base, ext;
            splitName(finalName, base, ext);
            bool freed = false;
            for (int n = 1; n <= kNameSuffixCap; ++n) {
                const QString cand = makeSuffixed(base, ext, n);
                if (!nameTaken(owner, parent, cand, db)) {
                    finalName = cand;
                    freed = true;
                    break;
                }
            }
            if (!freed) {
                r.error = "同目录同名候选耗尽";
                break;
            }
        }
        r.displayName = finalName;

        // 3) 条目：复活软删行或新建；引用计数只在真正新增引用时 +1
        q.prepare("SELECT file_id,object_id,status FROM user_file "
                  "WHERE owner_name=? AND parent_path=? AND display_name=? FOR UPDATE");
        q.addBindValue(owner);
        q.addBindValue(parent);
        q.addBindValue(finalName);
        if (!q.exec()) { r.error = q.lastError().text(); break; }
        qint64 fileId = -1;
        if (q.next()) {
            const qint64 rowId = q.value(0).toLongLong();
            const qint64 rowObj = q.value(1).isNull() ? -1 : q.value(1).toLongLong();
            const QString st = q.value(2).toString();
            if (st == "active") {
                if (rowObj == oid) {          // 幂等命中：零写入返回既有条目（验收2）
                    fileId = rowId;
                    r.fileId = rowId;
                    r.objectId = oid;
                    r.dedupHit = true;
                    if (!db.commit()) { r.error = db.lastError().text(); break; }
                    if (!objectCreated && !copyOnly && QFileInfo::exists(stagingPath)
                            && !QFile::remove(stagingPath)) {
                        enqueueStorageFailure(QStringLiteral("delete_staging_file"), owner,
                                              stagingPath, QString(), QStringLiteral("删除重复上传暂存文件失败"));
                    }
                    r.ok = true;
                    done = true;
                    break;
                }
                r.error = "并发同名冲突";   // nameTaken 与 FOR UPDATE 间隙出现新行
                break;
            }
            // status='deleted' → 复活
            q.prepare("UPDATE user_file SET object_id=?,status='active' WHERE file_id=?");
            q.addBindValue(oid);
            q.addBindValue(rowId);
            if (!q.exec()) { r.error = q.lastError().text(); break; }
            fileId = rowId;
        } else {
            q.prepare("INSERT INTO user_file (owner_name,parent_path,display_name,is_dir,object_id,status) "
                      "VALUES (?,?,?,0,?,'active')");
            q.addBindValue(owner);
            q.addBindValue(parent);
            q.addBindValue(finalName);
            q.addBindValue(oid);
            if (!q.exec()) { r.error = q.lastError().text(); break; }
            fileId = q.lastInsertId().toLongLong();
        }

        // 4) 引用计数 +1（对象已存在或本事务新建）
        if (!objectCreated) {
            q.prepare("UPDATE file_object SET refcount=refcount+1,status='ready',reclaim_at=NULL "
                      "WHERE object_id=? AND status IN ('ready','pending_reclaim')");
            q.addBindValue(oid);
            if (!q.exec()) { r.error = q.lastError().text(); break; }
            // 影响 0 行 = 该对象已被调度器抢到删除权（status='reclaiming'）或已删除。
            // 复活输了就让位：整体回滚，客户端重传即可——绝不留下一条指向已回收对象的条目。
            if (q.numRowsAffected() != 1) { r.error = "对象已进入回收，请重新上传"; break; }
        }

        if (!db.commit()) {
            r.error = db.lastError().text();
            break;
        }
        if (!objectCreated && !copyOnly) {
            if (QFileInfo::exists(stagingPath) && !QFile::remove(stagingPath)) {
                enqueueStorageFailure(QStringLiteral("delete_staging_file"), owner,
                                      stagingPath, QString(), QStringLiteral("删除已转正暂存文件失败"));
            }                           // 内容已在既有对象——物理只留一份（验收1）
        }
        r.fileId = fileId;
        r.objectId = oid;
        r.dedupHit = !objectCreated;
        r.ok = true;
        done = true;
    } while (false);

    if (!done) {
        db.rollback();
        if (objectCreated && !createdFinalAbs.isEmpty()) {
            if (copyOnly) {
                if (QFileInfo::exists(createdFinalAbs) && !QFile::remove(createdFinalAbs)) {
                    enqueueStorageFailure(QStringLiteral("rollback_staged_file"), owner,
                                          createdFinalAbs, QString(), QStringLiteral("清理转正半成品失败"));
                }
            } else if (!QFile::rename(createdFinalAbs, stagingPath)) {
                enqueueStorageFailure(QStringLiteral("rollback_staged_file"), owner,
                                      createdFinalAbs, stagingPath, QStringLiteral("转正失败后的回退搬移失败"));
            }      // 尽力回退搬移，孤儿对象可恢复
        }
        qCWarning(lcFs) << "[commitUpload] 失败回滚 owner" << owner << r.error;
    }
    return r;
}

// 条件更新：仅 ready/pending_reclaim 可被加引用，与调度器 reclaiming 抢权互斥。
bool FileObjectStore::refObject(qint64 objectId)
{
    if (objectId < 0) return false;
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) return false;
    QSqlDatabase db = conn.database();
    if (!db.transaction()) return false;
    QSqlQuery q(db);
    q.prepare("UPDATE file_object SET refcount=refcount+1,status='ready',reclaim_at=NULL "
                      "WHERE object_id=? AND status IN ('ready','pending_reclaim')");
    q.addBindValue(objectId);
    bool ok = q.exec() && q.numRowsAffected() > 0 && db.commit();
    if (!ok) db.rollback();
    return ok;
}

// 单条目软删：先按 (file_id, owner) 锁行确认归属与 active，再减对象引用。
// 任一步失败整体回滚，保证条目状态与 refcount 一致。
bool FileObjectStore::releaseEntry(qint64 fileId, const QString& owner, const QString& shareRelPath)
{
    if (fileId < 0 || owner.isEmpty()) return false;
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) return false;
    QSqlDatabase db = conn.database();
    if (!db.transaction()) return false;

    bool ok = false;
    do {
        QSqlQuery q(db);
        q.prepare("SELECT object_id,status FROM user_file WHERE file_id=? AND owner_name=? FOR UPDATE");
        q.addBindValue(fileId);
        q.addBindValue(owner);
        if (!q.exec()) break;                       // 归属谓词收口（T5）：非本人行直接失败
        if (!q.next() || q.value(1).toString() != "active") break;
        const QVariant obj = q.value(0);

        q.prepare("UPDATE user_file SET status='deleted' WHERE file_id=?");
        q.addBindValue(fileId);
        if (!q.exec()) break;

        if (!obj.isNull()) {
            const qint64 oid = obj.toLongLong();
            q.prepare("UPDATE file_object SET refcount=GREATEST(refcount-1,0),"
                      "status=IF(refcount<=0,'pending_reclaim','ready'),"
                      "reclaim_at=IF(refcount<=0,NOW(),NULL) WHERE object_id=?");
            q.addBindValue(oid);
            if (!q.exec()) break;                   // 归零→待回收（验收3），绝不删物理文件（D3）
        }
        // REQ-AUDIT-001 R7：链接清理与条目软删同一事务
        if (!shareRelPath.isEmpty()
                && !OperateDB::deleteShareLinkByPathTx(db, owner, shareRelPath)) break;
        ok = db.commit();
    } while (false);
    if (!ok) {
        db.rollback();
        qCWarning(lcFs) << "[releaseEntry] 失败 fileId" << fileId << "owner" << owner;
    }
    return ok;
}

// 目录软删：在内存中用逻辑键前缀匹配收集整棵子树，避免 SQL LIKE 通配歧义。
bool FileObjectStore::releaseSubtree(qint64 dirFileId, const QString& owner, const QString& shareRelPath)
{
    if (dirFileId < 0 || owner.isEmpty()) return false;
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) return false;
    QSqlDatabase db = conn.database();
    if (!db.transaction()) return false;

    bool ok = false;
    do {
        QSqlQuery q(db);
        q.prepare("SELECT parent_path,display_name,is_dir,status FROM user_file WHERE file_id=? AND owner_name=? FOR UPDATE");
        q.addBindValue(dirFileId);
        q.addBindValue(owner);
        if (!q.exec() || !q.next() || q.value(3).toString() != "active") break;
        if (!q.value(2).toBool()) break;            // 非目录请走 releaseEntry
        const QString dirParent = q.value(0).toString();
        const QString dirName = q.value(1).toString();
        const QString dirPath = logicDirKey(dirParent, dirName);

        QList<qint64> fileIds;
        QList<QVariant> objectIds;
        // F-4 修复：dirPath 以 '/' 结尾，旧 LIKE dirPath+"/%" 形成 '//%'——合法键无 "//"，
        // 深度≥2 后代全部漏删。改 owner 全行 FOR UPDATE + 内存 startsWith
        //（与 renameEntriesDir/releaseSubtreeByPath 同构，且免疫名字中的 %/_ 通配）。
        q.prepare("SELECT file_id,object_id,parent_path FROM user_file "
                  "WHERE owner_name=? AND status='active' FOR UPDATE");
        q.addBindValue(owner);
        if (!q.exec()) break;
        while (q.next()) {
            const qint64 fid = q.value(0).toLongLong();
            if (fid == dirFileId) continue;
            if (q.value(2).toString().startsWith(dirPath)) {   // 直接子项 p==dirPath，深层为前缀
                fileIds.append(fid);
                objectIds.append(q.value(1));
            }
        }
        q.prepare("SELECT file_id,object_id FROM user_file WHERE file_id=? FOR UPDATE");
        q.addBindValue(dirFileId);
        if (q.exec() && q.next()) {
            fileIds.prepend(dirFileId);
            objectIds.prepend(q.value(1));   // F-4：object_id 在第2列（曾错取 file_id，减到无关对象）
        }

        for (int i = 0; i < fileIds.size(); ++i) {
            q.prepare("UPDATE user_file SET status='deleted' WHERE file_id=?");
            q.addBindValue(fileIds[i]);
            if (!q.exec()) { ok = false; break; }   // §5：任一步失败必须整体回滚
            const QVariant obj = objectIds[i];
            if (!obj.isNull()) {
                q.prepare("UPDATE file_object SET refcount=GREATEST(refcount-1,0),"
                          "status=IF(refcount<=0,'pending_reclaim','ready'),"
                      "reclaim_at=IF(refcount<=0,NOW(),NULL) WHERE object_id=?");
                q.addBindValue(obj.toLongLong());
                if (!q.exec()) { ok = false; break; }
            }
            ok = true;
        }
        if (!ok) break;
        // REQ-AUDIT-001 R7：链接清理与子树软删同一事务
        if (!shareRelPath.isEmpty()
                && !OperateDB::deleteShareLinkByPathTx(db, owner, shareRelPath)) { ok = false; break; }
        ok = db.commit();
    } while (false);
    if (!ok) {
        db.rollback();
        qCWarning(lcFs) << "[releaseSubtree] 失败 dirFileId" << dirFileId << "owner" << owner;
    }
    return ok;
}

// 目录列表：LEFT JOIN 取对象大小（目录无对象，size 记为 0）。
QList<FileObjectStore::EntryInfo> FileObjectStore::listEntries(
        const QString& owner, const QString& rawParentPath) const
{
    QList<EntryInfo> out;
    const QString parent = normalizeParent(owner, rawParentPath);
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) return out;
    QSqlQuery q(conn.database());
    q.prepare("SELECT uf.file_id,uf.display_name,uf.is_dir,uf.object_id,fo.size_bytes "
              "FROM user_file uf LEFT JOIN file_object fo ON fo.object_id=uf.object_id "
              "WHERE uf.owner_name=? AND uf.parent_path=? AND uf.status='active' ORDER BY uf.display_name");
    q.addBindValue(owner);
    q.addBindValue(parent);
    if (!q.exec()) return out;
    while (q.next()) {
        EntryInfo e;
        e.fileId = q.value(0).toLongLong();
        e.displayName = q.value(1).toString();
        e.isDir = q.value(2).toBool();
        e.objectId = q.value(3).isNull() ? -1 : q.value(3).toLongLong();
        e.sizeBytes = q.value(4).isNull() ? 0 : q.value(4).toLongLong();
        out.append(e);
    }
    return out;
}

QSet<QString> FileObjectStore::deletedDisplayNames(
        const QString& owner, const QString& rawParentPath) const
{
    QSet<QString> out;
    if (owner.isEmpty()) return out;
    const QString parent = normalizeParent(owner, rawParentPath);
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) return out;
    QSqlQuery q(conn.database());
    q.prepare("SELECT display_name FROM user_file WHERE owner_name=? AND parent_path=? AND status='deleted'");
    q.addBindValue(owner);
    q.addBindValue(parent);
    if (!q.exec()) return out;
    while (q.next()) out.insert(q.value(0).toString());
    return out;
}

// 精确查单条目；未命中返回 fileId=-1 的默认值。
FileObjectStore::EntryInfo FileObjectStore::entryByPath(
        const QString& owner, const QString& rawParentPath, const QString& displayName) const
{
    EntryInfo e;
    const QString parent = normalizeParent(owner, rawParentPath);
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) return e;
    QSqlQuery q(conn.database());
    q.prepare("SELECT uf.file_id,uf.is_dir,uf.object_id,fo.size_bytes "
              "FROM user_file uf LEFT JOIN file_object fo ON fo.object_id=uf.object_id "
              "WHERE uf.owner_name=? AND uf.parent_path=? AND uf.display_name=? AND uf.status='active'");
    q.addBindValue(owner);
    q.addBindValue(parent);
    q.addBindValue(displayName);
    if (!q.exec() || !q.next()) return e;
    e.fileId = q.value(0).toLongLong();
    e.isDir = q.value(1).toBool();
    e.objectId = q.value(2).isNull() ? -1 : q.value(2).toLongLong();
    e.sizeBytes = q.value(3).isNull() ? 0 : q.value(3).toLongLong();
    return e;
}

// 改名/移动只改逻辑键；目录则同事务把后代 parent_path 的前缀整体平移。
bool FileObjectStore::relocateEntry(qint64 fileId, const QString& owner,
                                    const QString& newRawParent, const QString& newDisplayName,
                                    QString& err, const QString& oldShareRel,
                                    const QString& newShareRel)
{
    err.clear();
    const QString newParent = normalizeParent(owner, newRawParent);
    if (fileId < 0 || owner.isEmpty() || newParent.isEmpty() || newDisplayName.isEmpty()
        || newParent.size() > kMaxParentChars) {
        err = "参数为空或父路径超长";
        return false;
    }
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) { err = "数据库不可达"; return false; }
    QSqlDatabase db = conn.database();
    if (!db.transaction()) { err = "事务开启失败"; return false; }
    bool ok = false;
    do {
        QSqlQuery q(db);
        // 先锁自身行：目录改名/移动必须同事务级联子树 parent_path（D5：条目为权威）
        q.prepare("SELECT parent_path,display_name,is_dir FROM user_file "
                  "WHERE file_id=? AND owner_name=? AND status='active' FOR UPDATE");
        q.addBindValue(fileId);
        q.addBindValue(owner);
        if (!q.exec()) { err = q.lastError().text(); break; }
        if (!q.next()) { err = "条目不存在"; break; }
        const QString oldParent = q.value(0).toString();
        const QString oldName = q.value(1).toString();
        const bool isDir = q.value(2).toBool();
        const QString finalName = clampUtf8(newDisplayName, kMaxNameBytes);
        const QString oldDir = logicDirKey(oldParent, oldName);
        const QString newDir = logicDirKey(newParent, finalName);
        if (isDir && newParent.startsWith(oldDir)) {
            err = "不能移入自身或其子目录";   // 与物理 movefile 同语义
            break;
        }

        q.prepare("SELECT file_id FROM user_file "
                  "WHERE owner_name=? AND parent_path=? AND display_name=? AND status='active' LIMIT 1");
        q.addBindValue(owner);
        q.addBindValue(newParent);
        q.addBindValue(finalName);
        if (!q.exec()) { err = q.lastError().text(); break; }
        if (q.next()) { err = "目标名已占用"; break; }

        // 目录：先把所有后代 parent_path 前缀平移（内存前缀比对，避免 LIKE 转义歧义）
        if (isDir && (newParent != oldParent || finalName != oldName)) {
            QList<qint64> ids;
            QList<QString> paths;
            q.prepare("SELECT file_id,parent_path FROM user_file WHERE owner_name=? AND status='active' FOR UPDATE");
            q.addBindValue(owner);
            if (!q.exec()) { err = q.lastError().text(); break; }
            while (q.next()) {
                const QString p = q.value(1).toString();
                if (p.startsWith(oldDir)) {   // 键 '/' 结尾：直接子项 p==oldDir，深层 p 前缀命中
                    ids.append(q.value(0).toLongLong());
                    paths.append(p);
                }
            }
            bool cascaded = true;
            for (int i = 0; i < ids.size(); ++i) {
                const QString shifted = newDir + paths[i].mid(oldDir.size());
                if (shifted.size() > kMaxParentChars) { err = "级联子路径超长"; cascaded = false; break; }
                QSqlQuery u(db);
                u.prepare("UPDATE user_file SET parent_path=? WHERE file_id=?");
                u.addBindValue(shifted);
                u.addBindValue(ids[i]);
                if (!u.exec()) { err = u.lastError().text(); cascaded = false; break; }
            }
            if (!cascaded) break;   // 级联任一步失败 → 整体回滚（§5）
        }

        q.prepare("UPDATE user_file SET parent_path=?,display_name=? WHERE file_id=? AND owner_name=? AND status='active'");
        q.addBindValue(newParent);
        q.addBindValue(finalName);
        q.addBindValue(fileId);
        q.addBindValue(owner);
        if (!q.exec() || q.numRowsAffected() != 1) { err = "更新失败"; break; }
        // REQ-AUDIT-001 R7：链接平移与条目改名同一事务
        if (!oldShareRel.isEmpty() && !newShareRel.isEmpty()
                && !OperateDB::updateShareLinkPathTx(db, owner, oldShareRel, newShareRel)) {
            err = "分享链接平移失败"; break;
        }
        ok = db.commit();
    } while (false);
    if (!ok) db.rollback();
    return ok;
}

// 目录条目无对象（object_id=NULL），同名目录重复创建按幂等成功处理。
bool FileObjectStore::insertDirEntry(const QString& owner, const QString& rawParent,
                                     const QString& displayName, QString& err)
{
    err.clear();
    const QString parent = normalizeParent(owner, rawParent);
    const QString name = clampUtf8(displayName, kMaxNameBytes);
    if (owner.isEmpty() || parent.isEmpty() || name.isEmpty()
        || parent.size() > kMaxParentChars) {
        err = "参数为空或父路径超长";
        return false;
    }
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) { err = "数据库不可达"; return false; }
    QSqlDatabase db = conn.database();
    if (!db.transaction()) { err = "事务开启失败"; return false; }
    bool ok = false;
    do {
        QSqlQuery q(db);
        q.prepare("SELECT file_id,is_dir,status FROM user_file "
                  "WHERE owner_name=? AND parent_path=? AND display_name=? FOR UPDATE");
        q.addBindValue(owner);
        q.addBindValue(parent);
        q.addBindValue(name);
        if (!q.exec()) { err = q.lastError().text(); break; }
        if (q.next()) {
            const qint64 rowId = q.value(0).toLongLong();
            const bool rowDir = q.value(1).toBool();
            const QString st = q.value(2).toString();
            if (st == "active") {
                if (rowDir) { ok = db.commit(); break; }   // 幂等：目录条目已在
                err = "同名文件条目已占用";
                break;
            }
            q.prepare("UPDATE user_file SET object_id=NULL,is_dir=1,status='active' WHERE file_id=?");
            q.addBindValue(rowId);
            if (!q.exec()) { err = q.lastError().text(); break; }
        } else {
            q.prepare("INSERT INTO user_file (owner_name,parent_path,display_name,is_dir,object_id,status) "
                      "VALUES (?,?,?,1,NULL,'active')");
            q.addBindValue(owner);
            q.addBindValue(parent);
            q.addBindValue(name);
            if (!q.exec()) { err = q.lastError().text(); break; }
        }
        ok = db.commit();
    } while (false);
    if (!ok) db.rollback();
    return ok;
}

// REQ-EXT-FILE-002 §2.1：commitUpload 去掉"哈希+rename"两段后的同名安置+条目+计数事务，
// 供秒传命中分支复用——不复制物理文件、不读客户端路径。与 attachEntry 共用同一实现。
// expectedSize < 0 = 不校验对象大小。
namespace {
// REQ-AUDIT-001 R9：完成态会话登记（在调用方已开启的事务内执行，与条目/引用计数同提交）
qint64 insertCompletedSessionTx(QSqlDatabase& db, const QString& owner, const QString& parent,
                                const QString& displayName, qint64 size, const QString& sha,
                                const QString& requestId, qint64 fileId)
{
    QSqlQuery q(db);
    q.prepare("INSERT INTO upload_session (owner_name,parent_path,display_name,expected_size,"
              "client_sha256,staging_path,confirmed_offset,status,expires_at,client_request_id,file_id) "
              "VALUES (?,?,?,?,?,'',?,'done',?,?,?)");
    q.addBindValue(owner);
    q.addBindValue(parent);
    q.addBindValue(displayName);
    q.addBindValue(size);
    q.addBindValue(sha.isEmpty() ? QVariant() : QVariant(sha));
    q.addBindValue(size);
    q.addBindValue(QDateTime::currentDateTime().addSecs(3600).toString("yyyy-MM-dd HH:mm:ss"));
    q.addBindValue(requestId);
    q.addBindValue(fileId);
    if (!q.exec()) {
        qCWarning(lcFs) << "[attachExistingObject] 完成态会话登记失败:" << q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}
} // namespace

FileObjectStore::AttachResult FileObjectStore::attachExistingObject(
        const QString& owner, const QString& rawParent, const QString& wantedName,
        qint64 objectId, qint64 expectedSize, const QString& clientRequestId,
        qint64* sessionIdOut)
{
    AttachResult r;
    r.displayName = wantedName;
    if (sessionIdOut != nullptr) *sessionIdOut = -1;
    const QString parent = normalizeParent(owner, rawParent);
    if (owner.isEmpty() || parent.isEmpty() || parent.size() > kMaxParentChars
        || wantedName.isEmpty() || objectId < 0) {
        r.error = "参数为空或父路径超长";
        return r;
    }
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) { r.error = "数据库不可达"; return r; }
    QSqlDatabase db = conn.database();
    if (!db.transaction()) { r.error = "事务开启失败"; return r; }
    bool done = false;
    do {
        QSqlQuery q(db);
        q.prepare("SELECT size_bytes FROM file_object WHERE object_id=? FOR UPDATE");
        q.addBindValue(objectId);
        if (!q.exec() || !q.next()) { r.error = "对象不存在"; break; }
        if (expectedSize >= 0 && q.value(0).toLongLong() != expectedSize) {
            r.error = "对象大小与声明不符";
            break;
        }

        // 同名安置（与 commitUpload 一致）：active 同名同对象=幂等命中保留原名（跨树重复
        // 分享/重复秒传不建重复行）；同名不同对象才 (1) 递推；deleted 同名由下方复活分支承接。
        QString finalName = clampUtf8(wantedName, kMaxNameBytes);
        bool needSuffix = false;
        q.prepare("SELECT object_id,status FROM user_file "
                  "WHERE owner_name=? AND parent_path=? AND display_name=?");
        q.addBindValue(owner);
        q.addBindValue(parent);
        q.addBindValue(finalName);
        if (q.exec() && q.next() && q.value(1).toString() == "active") {
            const QVariant o = q.value(0);
            if (!(o.isValid() && !o.isNull() && o.toLongLong() == objectId))
                needSuffix = true;
        }
        if (needSuffix) {
            QString base, ext;
            splitName(finalName, base, ext);
            bool freed = false;
            for (int n = 1; n <= kNameSuffixCap; ++n) {
                const QString cand = makeSuffixed(base, ext, n);
                if (!nameTaken(owner, parent, cand, db)) { finalName = cand; freed = true; break; }
            }
            if (!freed) { r.error = "同目录同名候选耗尽"; break; }
        }
        r.displayName = finalName;

        q.prepare("SELECT file_id,object_id,status FROM user_file "
                  "WHERE owner_name=? AND parent_path=? AND display_name=? FOR UPDATE");
        q.addBindValue(owner);
        q.addBindValue(parent);
        q.addBindValue(finalName);
        if (!q.exec()) { r.error = q.lastError().text(); break; }
        if (q.next()) {
            const qint64 rowId = q.value(0).toLongLong();
            const qint64 rowObj = q.value(1).isNull() ? -1 : q.value(1).toLongLong();
            const QString st = q.value(2).toString();
            if (st == "active") {
                if (rowObj == objectId) {          // 幂等：同(位置,对象)已存在，不重复计数
                    r.fileId = rowId;
                    r.idempotent = true;
                    // R9：幂等命中也要有完成态会话行（同事务，失败即整体回滚）
                    if (!clientRequestId.isEmpty()) {
                        const qint64 sid = insertCompletedSessionTx(db, owner, parent, finalName,
                                                                    expectedSize >= 0 ? expectedSize : 0,
                                                                    QString(), clientRequestId, r.fileId);
                        if (sid < 0) { r.error = "完成态会话登记失败"; break; }
                        if (sessionIdOut != nullptr) *sessionIdOut = sid;
                    }
                    done = true;
                    break;
                }
                r.error = "并发同名冲突";
                break;
            }
            q.prepare("UPDATE user_file SET object_id=?,is_dir=0,status='active' WHERE file_id=?");
            q.addBindValue(objectId);
            q.addBindValue(rowId);
            if (!q.exec()) { r.error = q.lastError().text(); break; }
            r.fileId = rowId;
        } else {
            q.prepare("INSERT INTO user_file (owner_name,parent_path,display_name,is_dir,object_id,status) "
                      "VALUES (?,?,?,0,?,'active')");
            q.addBindValue(owner);
            q.addBindValue(parent);
            q.addBindValue(finalName);
            q.addBindValue(objectId);
            if (!q.exec()) { r.error = q.lastError().text(); break; }
            r.fileId = q.lastInsertId().toLongLong();
        }

        // 引用计数 +1（D3：pending_reclaim 对象被复用即置回 ready，待回收 ≠ 已删除）
        // 谓词里的 status IN (...) 是删除权的对侧：调度器把行抢成 'reclaiming' 后，
        // 这句必然影响 0 行——复活与回收在同一行上串行化，不靠时序约定。
        q.prepare("UPDATE file_object SET refcount=refcount+1,status='ready',reclaim_at=NULL "
                      "WHERE object_id=? AND status IN ('ready','pending_reclaim')");
        q.addBindValue(objectId);
        if (!q.exec()) { r.error = q.lastError().text(); break; }
        if (q.numRowsAffected() != 1) { r.error = "对象已进入回收，请重新上传"; break; }
        // R9：条目、引用计数、完成态会话行同一事务
        if (!clientRequestId.isEmpty()) {
            const qint64 sid = insertCompletedSessionTx(db, owner, parent, finalName,
                                                        expectedSize >= 0 ? expectedSize : -1,
                                                        QString(), clientRequestId, r.fileId);
            if (sid < 0) { r.error = "完成态会话登记失败"; break; }
            if (sessionIdOut != nullptr) *sessionIdOut = sid;
        }
        done = true;
    } while (false);

    if (!done) { db.rollback(); return r; }
    if (!db.commit()) { db.rollback(); r.error = db.lastError().text(); return r; }
    r.ok = true;
    return r;
}

// 轻量包装：不校验对象大小，把 AttachResult 转成 fileId/-1。
qint64 FileObjectStore::attachEntry(const QString& owner, const QString& rawParent,
                                    const QString& wantedName, qint64 objectId, QString& err)
{
    const AttachResult r = attachExistingObject(owner, rawParent, wantedName, objectId, -1);
    err = r.error;
    return r.ok ? r.fileId : -1;
}

// 目录物理残留改名后的条目级联；oldDir/newDir 均为 '/' 结尾的逻辑键。
// 无匹配条目视为成功（目录条目缺失时的兜底，空操作）。
bool FileObjectStore::renameEntriesDir(const QString& owner, const QString& oldDirRaw,
                                       const QString& newDirRaw, QString& err,
                                       const QString& oldShareRel, const QString& newShareRel)
{
    err.clear();
    const QString oldDir = normalizeParent(owner, oldDirRaw);
    const QString newDir = normalizeParent(owner, newDirRaw);
    if (owner.isEmpty() || oldDir.isEmpty() || newDir.isEmpty()) { err = "参数为空"; return false; }
    if (oldDir == newDir) return true;
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) { err = "数据库不可达"; return false; }
    QSqlDatabase db = conn.database();
    if (!db.transaction()) { err = "事务开启失败"; return false; }
    bool ok = false;
    do {
        QSqlQuery q(db);
        q.prepare("SELECT file_id,parent_path FROM user_file WHERE owner_name=? AND status='active' FOR UPDATE");
        q.addBindValue(owner);
        if (!q.exec()) { err = q.lastError().text(); break; }
        QList<qint64> ids;
        QList<QString> paths;
        while (q.next()) {
            const QString p = q.value(1).toString();
            if (p.startsWith(oldDir)) { ids.append(q.value(0).toLongLong()); paths.append(p); }
        }
        for (int i = 0; i < ids.size(); ++i) {
            const QString shifted = newDir + paths[i].mid(oldDir.size());
            if (shifted.size() > kMaxParentChars) { err = "级联子路径超长"; ok = false; break; }
            QSqlQuery u(db);
            u.prepare("UPDATE user_file SET parent_path=? WHERE file_id=?");
            u.addBindValue(shifted);
            u.addBindValue(ids[i]);
            if (!u.exec()) { err = u.lastError().text(); ok = false; break; }
            ok = true;
        }
        if (ids.isEmpty()) ok = true;   // 空操作（目录条目缺失兜底）
        if (!ok) break;
        // REQ-AUDIT-001 R7：链接平移与条目级联同一事务
        if (!oldShareRel.isEmpty() && !newShareRel.isEmpty()) {
            if (!OperateDB::updateShareLinkPathTx(db, owner, oldShareRel, newShareRel)) {
                err = "分享链接平移失败"; ok = false; break;
            }
        }
        ok = db.commit();
    } while (false);
    if (!ok) db.rollback();
    return ok;
}

// 旧目录物理删除后的条目软删；dir 为 '/' 结尾逻辑键，空匹配直接提交成功。
bool FileObjectStore::releaseSubtreeByPath(const QString& owner, const QString& dirRaw, QString& err,
                                           const QString& shareRelPath)
{
    err.clear();
    const QString dir = normalizeParent(owner, dirRaw);
    if (owner.isEmpty() || dir.isEmpty()) { err = "参数为空"; return false; }
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) { err = "数据库不可达"; return false; }
    QSqlDatabase db = conn.database();
    if (!db.transaction()) { err = "事务开启失败"; return false; }
    bool ok = false;
    do {
        QSqlQuery q(db);
        q.prepare("SELECT file_id,object_id,parent_path FROM user_file WHERE owner_name=? AND status='active' FOR UPDATE");
        q.addBindValue(owner);
        if (!q.exec()) { err = q.lastError().text(); break; }
        QList<qint64> ids;
        QList<QVariant> objs;
        while (q.next()) {
            const QString p = q.value(2).toString();
            if (p.startsWith(dir)) { ids.append(q.value(0).toLongLong()); objs.append(q.value(1)); }
        }
        if (ids.isEmpty()) {
            // REQ-AUDIT-001 R7：即使没有条目，也要在同一事务内清掉该路径的链接记录
            if (!shareRelPath.isEmpty()
                    && !OperateDB::deleteShareLinkByPathTx(db, owner, shareRelPath)) {
                err = "分享链接清理失败"; break;
            }
            ok = db.commit(); break;   // 空操作
        }
        for (int i = 0; i < ids.size(); ++i) {
            q.prepare("UPDATE user_file SET status='deleted' WHERE file_id=?");
            q.addBindValue(ids[i]);
            if (!q.exec()) { err = q.lastError().text(); ok = false; break; }
            if (!objs[i].isNull()) {
                q.prepare("UPDATE file_object SET refcount=GREATEST(refcount-1,0),"
                          "status=IF(refcount<=0,'pending_reclaim','ready'),"
                      "reclaim_at=IF(refcount<=0,NOW(),NULL) WHERE object_id=?");
                q.addBindValue(objs[i].toLongLong());
                if (!q.exec()) { err = q.lastError().text(); ok = false; break; }
            }
            ok = true;
        }
        if (!ok) break;
        // REQ-AUDIT-001 R7：链接清理与条目软删同一事务
        if (!shareRelPath.isEmpty()
                && !OperateDB::deleteShareLinkByPathTx(db, owner, shareRelPath)) {
            err = "分享链接清理失败"; ok = false; break;
        }
        ok = db.commit();
    } while (false);
    if (!ok) db.rollback();
    return ok;
}

// ===== REQ-EXT-GROUP-003：群文件引用（对象层）=====
// D3 口径：refcount 语义扩展为 "user_file(active) ∪ group_file(active)" 的活跃逻辑引用数；
// 群引用 +1 / -1 与个人条目共用同一计数器，归零 → pending_reclaim，绝不物理删。
FileObjectStore::GroupAttachResult FileObjectStore::attachGroupFile(
        const QString& owner, quint64 groupId, const QString& wantedName,
        qint64 objectId, qint64 sizeBytes, bool createUserEntry, qint64 existingFileId)
{
    GroupAttachResult r;
    r.groupDisplayName = wantedName;
    if (owner.isEmpty() || groupId == 0 || objectId < 0 || wantedName.isEmpty()) {
        r.error = "参数为空或非法";
        return r;
    }
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) { r.error = "数据库不可达"; return r; }
    QSqlDatabase db = conn.database();
    if (!db.transaction()) { r.error = "事务开启失败"; return r; }

    bool done = false;
    do {
        QSqlQuery q(db);
        // 0) 对象存在且大小相符（FOR UPDATE，与 commitUpload / attachExistingObject 同源口径）
        q.prepare("SELECT size_bytes FROM file_object WHERE object_id=? FOR UPDATE");
        q.addBindValue(objectId);
        if (!q.exec() || !q.next()) { r.error = "对象不存在"; break; }
        const qint64 objSize = q.value(0).toLongLong();
        const qint64 size = sizeBytes >= 0 ? sizeBytes : objSize;
        if (objSize != size) { r.error = "对象大小与声明不符"; break; }

        // 1) 上传者个人根逻辑条目（可选）：群文件与个人根命名空间解耦，仅为逻辑文件标识。
        qint64 fileId = existingFileId;
        if (createUserEntry) {
            const QString parent = normalizeParent(owner, QStringLiteral("/"));
            if (parent.isEmpty()) { r.error = "个人根归一失败"; break; }
            QString pName = clampUtf8(wantedName, kMaxNameBytes);
            bool pSuffix = false;
            q.prepare("SELECT object_id,status FROM user_file "
                      "WHERE owner_name=? AND parent_path=? AND display_name=?");
            q.addBindValue(owner);
            q.addBindValue(parent);
            q.addBindValue(pName);
            if (q.exec() && q.next() && q.value(1).toString() == "active") {
                const QVariant o = q.value(0);
                if (!(o.isValid() && !o.isNull() && o.toLongLong() == objectId)) pSuffix = true;
            }
            if (pSuffix) {
                QString base, ext;
                splitName(pName, base, ext);
                bool freed = false;
                for (int n = 1; n <= kNameSuffixCap; ++n) {
                    const QString cand = makeSuffixed(base, ext, n);
                    if (!nameTaken(owner, parent, cand, db)) { pName = cand; freed = true; break; }
                }
                if (!freed) { r.error = "个人根同名候选耗尽"; break; }
            }
            q.prepare("SELECT file_id,object_id,status FROM user_file "
                      "WHERE owner_name=? AND parent_path=? AND display_name=? FOR UPDATE");
            q.addBindValue(owner);
            q.addBindValue(parent);
            q.addBindValue(pName);
            if (!q.exec()) { r.error = q.lastError().text(); break; }
            bool newRef = true;
            if (q.next()) {
                const qint64 rowId = q.value(0).toLongLong();
                const qint64 rowObj = q.value(1).isNull() ? -1 : q.value(1).toLongLong();
                const QString st = q.value(2).toString();
                if (st == "active") {
                    if (rowObj != objectId) { r.error = "个人根并发同名冲突"; break; }
                    fileId = rowId;
                    newRef = false;                       // 幂等：同(位置,对象)已存在，不重复计数
                } else {
                    q.prepare("UPDATE user_file SET object_id=?,is_dir=0,status='active' WHERE file_id=?");
                    q.addBindValue(objectId);
                    q.addBindValue(rowId);
                    if (!q.exec()) { r.error = q.lastError().text(); break; }
                    fileId = rowId;
                }
            } else {
                q.prepare("INSERT INTO user_file (owner_name,parent_path,display_name,is_dir,object_id,status) "
                          "VALUES (?,?,?,0,?,'active')");
                q.addBindValue(owner);
                q.addBindValue(parent);
                q.addBindValue(pName);
                q.addBindValue(objectId);
                if (!q.exec()) { r.error = q.lastError().text(); break; }
                fileId = q.lastInsertId().toLongLong();
            }
            if (newRef) {
                q.prepare("UPDATE file_object SET refcount=refcount+1,status='ready',reclaim_at=NULL "
                      "WHERE object_id=? AND status IN ('ready','pending_reclaim')");
                q.addBindValue(objectId);
                if (!q.exec()) { r.error = q.lastError().text(); break; }
                if (q.numRowsAffected() != 1) { r.error = "对象已进入回收，请重新上传"; break; }
            }
        }
        r.fileId = fileId;

        // 2) 群目录条目：群内同名按 uk_group_file_name 去重，同名同对象幂等
        QString gName = clampUtf8(wantedName, kMaxNameBytes);
        bool gSuffix = false;
        q.prepare("SELECT object_id,status FROM group_file WHERE group_id=? AND display_name=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
        q.addBindValue(gName);
        if (q.exec() && q.next() && q.value(1).toString() == "active") {
            const QVariant o = q.value(0);
            if (!(o.isValid() && !o.isNull() && o.toLongLong() == objectId)) gSuffix = true;
        }
        if (gSuffix) {
            QString base, ext;
            splitName(gName, base, ext);
            bool freed = false;
            for (int n = 1; n <= kNameSuffixCap; ++n) {
                const QString cand = makeSuffixed(base, ext, n);
                QSqlQuery t(db);
                t.prepare("SELECT 1 FROM group_file WHERE group_id=? AND display_name=? AND status='active' LIMIT 1");
                t.addBindValue(QVariant::fromValue<qulonglong>(groupId));
                t.addBindValue(cand);
                if (!(t.exec() && t.next())) { gName = cand; freed = true; break; }
            }
            if (!freed) { r.error = "群目录同名候选耗尽"; break; }
        }
        r.groupDisplayName = gName;

        q.prepare("SELECT group_file_id,object_id,status FROM group_file "
                  "WHERE group_id=? AND display_name=? FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
        q.addBindValue(gName);
        if (!q.exec()) { r.error = q.lastError().text(); break; }
        if (q.next()) {
            const qint64 gfId = q.value(0).toLongLong();
            const qint64 gfObj = q.value(1).isNull() ? -1 : q.value(1).toLongLong();
            const QString st = q.value(2).toString();
            if (st == "active") {
                if (gfObj != objectId) { r.error = "群内并发同名冲突"; break; }
                r.groupFileId = gfId;                     // 幂等命中：零写入返回既有引用
                done = true;
                break;
            }
            q.prepare("UPDATE group_file SET object_id=?,file_id=?,uploader_name=?,size_bytes=?,status='active' "
                      "WHERE group_file_id=?");
            q.addBindValue(objectId);
            q.addBindValue(fileId >= 0 ? QVariant::fromValue<qint64>(fileId) : QVariant());
            q.addBindValue(owner);
            q.addBindValue(size);
            q.addBindValue(gfId);
            if (!q.exec()) { r.error = q.lastError().text(); break; }
            r.groupFileId = gfId;
        } else {
            q.prepare("INSERT INTO group_file (group_id,file_id,object_id,uploader_name,display_name,size_bytes,status) "
                      "VALUES (?,?,?,?,?,?,'active')");
            q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
            q.addBindValue(fileId >= 0 ? QVariant::fromValue<qint64>(fileId) : QVariant());
            q.addBindValue(objectId);
            q.addBindValue(owner);
            q.addBindValue(gName);
            q.addBindValue(size);
            if (!q.exec()) { r.error = q.lastError().text(); break; }
            r.groupFileId = q.lastInsertId().toLongLong();
        }
        // 3) 群引用 +1
        q.prepare("UPDATE file_object SET refcount=refcount+1,status='ready',reclaim_at=NULL "
                      "WHERE object_id=? AND status IN ('ready','pending_reclaim')");
        q.addBindValue(objectId);
        if (!q.exec()) { r.error = q.lastError().text(); break; }
        if (q.numRowsAffected() != 1) { r.error = "对象已进入回收，请重新上传"; break; }
        done = true;
    } while (false);

    if (!done) { db.rollback(); return r; }
    if (!db.commit()) { db.rollback(); r.error = db.lastError().text(); return r; }
    r.ok = true;
    return r;
}

// 群文件软删：group_file 置 removed 并减引用；group_id 作为归属谓词防跨群操作。
bool FileObjectStore::releaseGroupFile(qint64 groupFileId, quint64 groupId,
                                       const QString& actor, bool actorIsOwner, QString& err)
{
    err.clear();
    if (groupFileId < 0 || groupId == 0 || actor.isEmpty()) { err = "参数为空"; return false; }
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) { err = "数据库不可达"; return false; }
    QSqlDatabase db = conn.database();
    if (!db.transaction()) { err = "事务开启失败"; return false; }

    bool ok = false;
    do {
        QSqlQuery q(db);
        // 归属谓词：group_file_id 必须与信封 group_id 同群（不泄露他群存在性）
        q.prepare("SELECT object_id,uploader_name,status FROM group_file "
                  "WHERE group_file_id=? AND group_id=? FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qint64>(groupFileId));
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
        if (!q.exec() || !q.next() || q.value(2).toString() != "active") { err = "群文件不存在"; break; }
        if (!actorIsOwner && q.value(1).toString() != actor) { err = "无权删除他人群文件"; break; }
        const QVariant obj = q.value(0);

        q.prepare("UPDATE group_file SET status='removed' WHERE group_file_id=?");
        q.addBindValue(QVariant::fromValue<qint64>(groupFileId));
        if (!q.exec()) { err = q.lastError().text(); break; }

        if (!obj.isNull()) {
            q.prepare("UPDATE file_object SET refcount=GREATEST(refcount-1,0),"
                      "status=IF(refcount<=0,'pending_reclaim','ready'),"
                      "reclaim_at=IF(refcount<=0,NOW(),NULL) WHERE object_id=?");
            q.addBindValue(obj.toLongLong());
            if (!q.exec()) { err = q.lastError().text(); break; }
        }
        ok = db.commit();
    } while (false);
    if (!ok) {
        db.rollback();
        qCWarning(lcFs) << "[releaseGroupFile] 失败 groupFileId" << groupFileId << "err" << err;
    }
    return ok;
}
