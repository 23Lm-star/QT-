#include "logging.h"
#include "fileobjectstore.h"
#include "databaseconnectionpool.h"
#include "server.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QVariant>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace {
const int kMaxParentChars = 150;   // user_file.parent_path 列宽
const int kMaxNameBytes   = 70;    // display_name 列宽（UTF-8 字节）
const int kNameSuffixCap  = 100;   // "(N)" 递推上限

QString clampUtf8(const QString& s, int maxBytes);

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
bool isLogicKeyForm(const QString& x)
{
    if (!x.startsWith(QLatin1Char('/')) || !x.endsWith(QLatin1Char('/')) || x.contains(QLatin1Char('\\')))
        return false;
    if (x == QLatin1String("/"))
        return true;
    if (x.contains(QLatin1String("//")))
        return false;
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
        bool copyOnly)
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

    QString sha;
    qint64 size = 0;
    if (!hashStaging(stagingPath, sha, size)) {
        r.error = "staging 复算失败";
        return r;
    }
    if (size != expectedSize) {
        r.error = "复算大小与登记不符，拒绝转正";
        qCWarning(lcFs) << "[commitUpload] conn-owner" << owner
                        << "expected" << expectedSize << "actual" << size;
        if (!copyOnly) QFile::remove(stagingPath);   // T6：迁移源=用户旧文件，绝不删除（验收5）
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
            QDir().mkpath(QFileInfo(createdFinalAbs).absolutePath());
            const bool moved = copyOnly
                    ? QFile::copy(stagingPath, createdFinalAbs)   // T6：旧实体复制为对象，源永不删除
                    : QFile::rename(stagingPath, createdFinalAbs);
            if (!moved) {
                r.error = copyOnly ? "对象转正 copy 失败" : "对象转正 rename 失败";
                qCWarning(lcFs) << "[commitUpload]" << r.error << createdFinalAbs;
                if (copyOnly) QFile::remove(createdFinalAbs);     // 清理半成品副本
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
                    if (!objectCreated && !copyOnly) QFile::remove(stagingPath);
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
            q.prepare("UPDATE file_object SET refcount=refcount+1,status='ready' WHERE object_id=?");
            q.addBindValue(oid);
            if (!q.exec()) { r.error = q.lastError().text(); break; }
        }

        if (!db.commit()) {
            r.error = db.lastError().text();
            break;
        }
        if (!objectCreated && !copyOnly) {
            QFile::remove(stagingPath);                           // 内容已在既有对象——物理只留一份（验收1）
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
            if (copyOnly) QFile::remove(createdFinalAbs);          // T6：清理本事务产生的副本，源不动
            else QFile::rename(createdFinalAbs, stagingPath);      // 尽力回退搬移，孤儿对象可恢复
        }
        qCWarning(lcFs) << "[commitUpload] 失败回滚 owner" << owner << r.error;
    }
    return r;
}

bool FileObjectStore::refObject(qint64 objectId)
{
    if (objectId < 0) return false;
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) return false;
    QSqlDatabase db = conn.database();
    if (!db.transaction()) return false;
    QSqlQuery q(db);
    q.prepare("UPDATE file_object SET refcount=refcount+1,status='ready' WHERE object_id=?");
    q.addBindValue(objectId);
    bool ok = q.exec() && q.numRowsAffected() > 0 && db.commit();
    if (!ok) db.rollback();
    return ok;
}

bool FileObjectStore::releaseEntry(qint64 fileId, const QString& owner)
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
                      "status=IF(refcount<=0,'pending_reclaim','ready') WHERE object_id=?");
            q.addBindValue(oid);
            if (!q.exec()) break;                   // 归零→待回收（验收3），绝不删物理文件（D3）
        }
        ok = db.commit();
    } while (false);
    if (!ok) {
        db.rollback();
        qCWarning(lcFs) << "[releaseEntry] 失败 fileId" << fileId << "owner" << owner;
    }
    return ok;
}

bool FileObjectStore::releaseSubtree(qint64 dirFileId, const QString& owner)
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
                          "status=IF(refcount<=0,'pending_reclaim','ready') WHERE object_id=?");
                q.addBindValue(obj.toLongLong());
                if (!q.exec()) { ok = false; break; }
            }
            ok = true;
        }
        if (!ok) break;
        ok = db.commit();
    } while (false);
    if (!ok) {
        db.rollback();
        qCWarning(lcFs) << "[releaseSubtree] 失败 dirFileId" << dirFileId << "owner" << owner;
    }
    return ok;
}

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

bool FileObjectStore::relocateEntry(qint64 fileId, const QString& owner,
                                    const QString& newRawParent, const QString& newDisplayName,
                                    QString& err)
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
        ok = db.commit();
    } while (false);
    if (!ok) db.rollback();
    return ok;
}

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

qint64 FileObjectStore::attachEntry(const QString& owner, const QString& rawParent,
                                    const QString& wantedName, qint64 objectId, QString& err)
{
    err.clear();
    const QString parent = normalizeParent(owner, rawParent);
    if (owner.isEmpty() || parent.isEmpty() || parent.size() > kMaxParentChars
        || wantedName.isEmpty() || objectId < 0) {
        err = "参数为空或父路径超长";
        return -1;
    }
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) { err = "数据库不可达"; return -1; }
    QSqlDatabase db = conn.database();
    if (!db.transaction()) { err = "事务开启失败"; return -1; }
    qint64 resultId = -1;
    bool done = false;
    do {
        QSqlQuery q(db);
        q.prepare("SELECT object_id FROM file_object WHERE object_id=? FOR UPDATE");
        q.addBindValue(objectId);
        if (!q.exec() || !q.next()) { err = "对象不存在"; break; }

        // 同名安置（与 commitUpload 一致）：active 同名同对象=幂等命中保留原名（跨树重复
        // 分享不建重复行）；同名不同对象才 (1) 递推；deleted 同名由下方复活分支承接。
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
            if (!freed) { err = "同目录同名候选耗尽"; break; }
        }

        q.prepare("SELECT file_id,object_id,status FROM user_file "
                  "WHERE owner_name=? AND parent_path=? AND display_name=? FOR UPDATE");
        q.addBindValue(owner);
        q.addBindValue(parent);
        q.addBindValue(finalName);
        if (!q.exec()) { err = q.lastError().text(); break; }
        if (q.next()) {
            const qint64 rowId = q.value(0).toLongLong();
            const qint64 rowObj = q.value(1).isNull() ? -1 : q.value(1).toLongLong();
            const QString st = q.value(2).toString();
            if (st == "active") {
                if (rowObj == objectId) {          // 幂等：同(位置,对象)已存在，不重复计数
                    resultId = rowId;
                    done = true;
                    break;
                }
                err = "并发同名冲突";
                break;
            }
            q.prepare("UPDATE user_file SET object_id=?,is_dir=0,status='active' WHERE file_id=?");
            q.addBindValue(objectId);
            q.addBindValue(rowId);
            if (!q.exec()) { err = q.lastError().text(); break; }
            resultId = rowId;
        } else {
            q.prepare("INSERT INTO user_file (owner_name,parent_path,display_name,is_dir,object_id,status) "
                      "VALUES (?,?,?,0,?,'active')");
            q.addBindValue(owner);
            q.addBindValue(parent);
            q.addBindValue(finalName);
            q.addBindValue(objectId);
            if (!q.exec()) { err = q.lastError().text(); break; }
            resultId = q.lastInsertId().toLongLong();
        }

        q.prepare("UPDATE file_object SET refcount=refcount+1,status='ready' WHERE object_id=?");
        q.addBindValue(objectId);
        if (!q.exec()) { err = q.lastError().text(); break; }
        done = true;
    } while (false);

    if (!done) { db.rollback(); return -1; }
    if (!db.commit()) { db.rollback(); err = db.lastError().text(); return -1; }
    return resultId;
}

bool FileObjectStore::renameEntriesDir(const QString& owner, const QString& oldDirRaw,
                                       const QString& newDirRaw, QString& err)
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
        if (ids.isEmpty()) ok = true;   // 空操作（未迁移旧树无条目）
        if (!ok) break;
        ok = db.commit();
    } while (false);
    if (!ok) db.rollback();
    return ok;
}

bool FileObjectStore::releaseSubtreeByPath(const QString& owner, const QString& dirRaw, QString& err)
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
        if (ids.isEmpty()) { ok = db.commit(); break; }   // 空操作
        for (int i = 0; i < ids.size(); ++i) {
            q.prepare("UPDATE user_file SET status='deleted' WHERE file_id=?");
            q.addBindValue(ids[i]);
            if (!q.exec()) { err = q.lastError().text(); ok = false; break; }
            if (!objs[i].isNull()) {
                q.prepare("UPDATE file_object SET refcount=GREATEST(refcount-1,0),"
                          "status=IF(refcount<=0,'pending_reclaim','ready') WHERE object_id=?");
                q.addBindValue(objs[i].toLongLong());
                if (!q.exec()) { err = q.lastError().text(); ok = false; break; }
            }
            ok = true;
        }
        if (!ok) break;
        ok = db.commit();
    } while (false);
    if (!ok) db.rollback();
    return ok;
}
