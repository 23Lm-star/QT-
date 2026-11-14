#include "resumableuploadsession.h"
#include "server.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

// resumableuploadsession.cpp — 上传会话的暂存文件定位与进度推进。
// 旧路径直接写最终文件；新路径写 filesys/.staging/<sha1(owner|parent|display|size)>.part，
// 只有 commitUpload 复算通过后才 rename 进 objects/，因此暂存目录不会出现半成品正式文件。

ResumableUploadSession::ResumableUploadSession()
    : m_receivedSize(0),
      m_totalSize(0)
{
}

ResumableUploadSession::~ResumableUploadSession()
{
    reset();
}

ResumableUploadSession::BeginResult ResumableUploadSession::begin(const QString &dirPath,
                                                                  const QString &fileName,
                                                                  qint64 totalSize,
                                                                  bool resumeEnabled)
{
    reset();

    BeginResult result = {false, 0, false};
    if (dirPath.isEmpty() || fileName.isEmpty() || fileName.contains('/')
            || fileName.contains('\\') || totalSize < 0) {
        return result;
    }

    QDir dir(dirPath);
    if (!dir.exists()) {
        return result;
    }

    // 续传判定：已有文件小于总长 → 从现有大小追加；恰好等于总长 → 直接完成。
    m_filePath = dir.filePath(fileName);
    QFileInfo fileInfo(m_filePath);
    qint64 offset = 0;
    if (resumeEnabled && fileInfo.exists() && fileInfo.isFile()) {
        const qint64 existingSize = fileInfo.size();
        if (existingSize > 0 && existingSize < totalSize) {
            offset = existingSize;
        } else if (existingSize == totalSize) {
            m_receivedSize = existingSize;
            m_totalSize = totalSize;
            return {true, existingSize, true};
        }
    }

    // 无可续传内容时用 Truncate 清掉残留，避免旧字节混入本次上传。
    QFile file(m_filePath);
    QIODevice::OpenMode mode = QIODevice::WriteOnly;
    mode |= ((resumeEnabled && offset > 0) ? QIODevice::Append : QIODevice::Truncate);
    if (!file.open(mode)) {
        reset();
        return result;
    }
    file.close();

    m_receivedSize = offset;
    m_totalSize = totalSize;
    result.ok = true;
    result.offset = offset;
    result.complete = isComplete();
    return result;
}

// REQ-RESUME-AUDIT-001 M1：确定性暂存路径单独暴露——调用方需在建会话前先查
// upload_session 行（confirmed_offset 封顶 + 摘要回填），路径计算必须先于 beginStaged。
QString ResumableUploadSession::stagedPathFor(const QString &owner, const QString &parentPath,
                                              const QString &displayName, qint64 totalSize)
{
    // 确定性 token：同一 (owner, parent, name, size) 重连必然落到同一 .part。
    // SHA-1 仅用于生成短文件名，不承担内容完整性职责（完整性由 commitUpload 的 SHA-256 复算保证）。
    const QByteArray key = (owner + "|" + parentPath + "|" + displayName + "|" + QString::number(totalSize))
                               .toUtf8();
    const QString token = QString::fromLatin1(
        QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex());
    return QDir(Server::getinstance().filesPath).absoluteFilePath(
        QStringLiteral(".staging/") + token + QStringLiteral(".part"));
}

// REQ-EXT-FILE-001 T4：暂存路径 = filesys/.staging/<sha1(owner|parent|display|size)>.part
// 同参数重连命中同一 staging（断点续传）；完成前不进最终目录（§4）。
ResumableUploadSession::BeginResult ResumableUploadSession::beginStaged(
        const QString &owner, const QString &parentPath, const QString &displayName,
        qint64 totalSize, bool resumeEnabled, qint64 confirmedCap)
{
    reset();

    BeginResult result = {false, 0, false};
    if (owner.isEmpty() || parentPath.isEmpty() || displayName.isEmpty()
            || displayName.contains('/') || displayName.contains('\\')
            || totalSize < 0) {
        return result;
    }

    QDir stageRoot(QDir(Server::getinstance().filesPath).absoluteFilePath(".staging"));
    if (!stageRoot.exists() && !stageRoot.mkpath(".")) {
        return result;
    }
    m_filePath = stagedPathFor(owner, parentPath, displayName, totalSize);
    m_owner = owner;
    m_parent = parentPath;
    m_display = displayName;

    // REQ-EXT-TRANSFER-003 S1（方案 B）：暂存句柄打开后**不移交、不关闭**，挂在会话上共享所有权。
    // 写任务只 seek+write+flush（S3）；无需 Append——每块显式 seek 到 chunkOffset。

    // 已有 .part 小于总长 → 从其大小处续传；等于总长 → 直接视为完成（后续走校验/转正）。
    // REQ-RESUME-AUDIT-001 M1：起点以会话行 confirmed_offset 封顶——物理可能超前（崩溃半块/
    // 尾部垃圾）也可能落后（TTL 已删 .part 而行仍在），两个方向都不能当已确认数据续。
    // 物理超出总长视为不可信残留，整文件重传（与既有 existingSize>totalSize 收敛一致）。
    QFileInfo fileInfo(m_filePath);
    qint64 offset = 0;
    if (resumeEnabled && fileInfo.exists() && fileInfo.isFile()) {
        const qint64 existingSize = fileInfo.size();
        if (existingSize > totalSize) {
            offset = 0;
        } else if (confirmedCap >= 0 && existingSize > confirmedCap) {
            // 截到已确认偏移续传；[offset, existingSize) 的残留字节会被后续分块按绝对偏移覆写
            offset = confirmedCap;
        } else {
            offset = existingSize;
        }
        if (offset == totalSize) {
            m_receivedSize = offset;
            m_totalSize = totalSize;
            return {true, offset, true};
        }
    }

    QSharedPointer<QFile> file(new QFile(m_filePath));
    QIODevice::OpenMode mode = QIODevice::ReadWrite;
    if (offset == 0) {
        mode |= QIODevice::Truncate;   // 重头（含 resumeEnabled=false）：清掉既有残留
    }
    if (!file->open(mode)) {
        reset();
        return result;
    }
    m_stagingFile = file;

    m_receivedSize = offset;
    m_totalSize = totalSize;
    result.ok = true;
    result.offset = offset;
    result.complete = isComplete();
    return result;
}

// 进度只在“紧接当前 confirmed offset 且不越界”时推进，任何空洞/重块都拒绝。
bool ResumableUploadSession::acceptWrittenChunk(qint64 offset, qint64 len)
{
    if (offset != m_receivedSize || len <= 0 || len > m_totalSize - m_receivedSize) {
        return false;
    }
    m_receivedSize += len;
    return true;
}

bool ResumableUploadSession::isComplete() const
{
    return m_totalSize >= 0 && m_receivedSize >= m_totalSize;
}

qint64 ResumableUploadSession::receivedSize() const
{
    return m_receivedSize;
}

qint64 ResumableUploadSession::totalSize() const
{
    return m_totalSize;
}

QString ResumableUploadSession::stagingFilePath() const
{
    return m_filePath;
}

// 只清理内存态；磁盘上的 .part 保留，供断点续传或 TTL 回收。
void ResumableUploadSession::reset()
{
    // REQ-EXT-TRANSFER-003 S2：会话重置即释放共享句柄（最后一个引用释放才会真正关闭）
    m_stagingFile.clear();
    m_filePath.clear();
    m_receivedSize = 0;
    m_totalSize = 0;
    m_owner.clear();
    m_parent.clear();
    m_display.clear();
}
