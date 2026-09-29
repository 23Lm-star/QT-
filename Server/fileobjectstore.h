#ifndef FILEOBJECTSTORE_H
#define FILEOBJECTSTORE_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>

// REQ-EXT-FILE-001 T2：内容寻址对象存储层。
// 物理对象唯一（sha256），逻辑条目（user_file）引用对象；
// 对象命中+条目创建+引用计数在同一个 DB 事务内完成；
// 引用归零仅标 pending_reclaim——本分支绝不物理删除对象文件（D3）。
class FileObjectStore : public QObject
{
    Q_OBJECT
public:
    struct CommitResult {
        bool ok = false;
        qint64 fileId = -1;      // 最终落定的 user_file.file_id
        qint64 objectId = -1;
        QString displayName;     // 服务端定名（含 (1) 后缀规则）
        bool dedupHit = false;   // 命中已有对象（秒传地基，本分支不开 PDU）
        QString error;
    };

    struct EntryInfo {
        qint64 fileId = -1;
        QString displayName;
        bool isDir = false;
        qint64 objectId = -1;    // 目录为 -1
        qint64 sizeBytes = 0;
        bool valid() const { return fileId >= 0; }
    };

    static FileObjectStore& instance();

    // 服务端复算 staging 的 SHA-256 与大小（流式，1MiB 缓冲）
    bool hashStaging(const QString& stagingPath, QString& shaHex, qint64& sizeBytes) const;

    // 事务内：find-or-create 对象 + 建/复活条目 + 引用计数。
    // expectedSize 为会话登记大小；复算不一致 = 失败（不转正，验收4）。
    // 新对象：staging 原子 rename 到 objects/{sha前2}/{object_id}；命中已有对象：丢弃 staging。
    CommitResult commitUpload(const QString& owner, const QString& parentPath,
                              const QString& displayName, qint64 expectedSize,
                              const QString& stagingPath);

    // 删除条目（软删）并释放引用；对象引用归零 → pending_reclaim。
    bool releaseEntry(qint64 fileId, const QString& owner);

    // 新建引用（分享=同对象新条目，+1 引用，物理零复制）。
    bool refObject(qint64 objectId);

    // 条目 → 对象绝对路径（下载/迁移用）；对象不存在或已标回收失败返回空。
    QString objectAbsPath(qint64 objectId) const;

    // 列出 parent 下 active 条目（flushfile/listdir 渲染）；无记录返回空表。
    QList<EntryInfo> listEntries(const QString& owner, const QString& parentPath) const;

    // 查询单条目（含目录）；不存在返回 invalid。
    EntryInfo entryByPath(const QString& owner, const QString& parentPath,
                          const QString& displayName) const;

    // 目录软删：递归释放 parent 及子树全部文件条目（同事务）。
    bool releaseSubtree(qint64 dirFileId, const QString& owner);

    // 条目改名/移动：只动逻辑键，引用不变。
    bool relocateEntry(qint64 fileId, const QString& owner,
                       const QString& newParentPath, const QString& newDisplayName,
                       QString& err);

    // 逻辑父路径规范化：cleanPath+绝对化（以 filesPath 根为基准），
    // 所有入口统一经此函数，保证 user_file.parent_path 只有唯一字面形式。
    static QString normalizeParent(const QString& dirPath);

private:
    explicit FileObjectStore(QObject* parent = nullptr);
    FileObjectStore(const FileObjectStore&) = delete;
    FileObjectStore& operator=(const FileObjectStore&) = delete;
};

#endif // FILEOBJECTSTORE_H
