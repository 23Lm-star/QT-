#ifndef RESUMABLEUPLOADSESSION_H
#define RESUMABLEUPLOADSESSION_H

#include <QString>
#include <QSharedPointer>

class QFile;

// 单次上传会话：记录已确认字节数与目标，负责创建/定位暂存文件。
// 断点续传不变量：acceptWrittenChunk 只接受紧接 confirmed offset 的连续块；
// 会话本身不写磁盘，写入由 UploadWriteTask 在 QThreadPool 中完成。
class ResumableUploadSession
{
public:
    struct BeginResult
    {
        // 暂存文件是否就绪。
        bool ok;
        // 续传起点（已落盘字节数）。
        qint64 offset;
        // 文件在 begin 时已完整。
        bool complete;
    };

    ResumableUploadSession();
    // 释放暂存句柄共享引用并清空状态。
    ~ResumableUploadSession();

    // 旧上传路径：直接写入目标目录下的最终文件名，支持按现有大小续传。
    BeginResult begin(const QString &dirPath,
                      const QString &fileName,
                      qint64 totalSize,
                      bool resumeEnabled);

    // REQ-EXT-FILE-001 T4：内容寻址上传暂存。staging 按 (owner|parent|display|size)
    // 派生确定性路径，重连/续传命中同一 staging；完成前绝不落最终目录（§4）。
    BeginResult beginStaged(const QString &owner,
                            const QString &parentPath,
                            const QString &displayName,
                            qint64 totalSize,
                            bool resumeEnabled);
    // 仅在 offset == 已确认字节数且不越过总大小时推进进度（连续性检查）。
    bool acceptWrittenChunk(qint64 offset, qint64 len);
    // receivedSize >= totalSize。
    bool isComplete() const;
    // 已确认写入的字节数（confirmed offset）。
    qint64 receivedSize() const;
    // 会话登记的文件总大小。
    qint64 totalSize() const;
    // 暂存/目标文件绝对路径。
    QString stagingFilePath() const;
    // REQ-EXT-TRANSFER-003 S1/S2：暂存句柄共享所有权——beginStaged 打开后不关闭，
    // 写任务持引用做 seek+write+flush；最后一个引用释放即关闭。
    QSharedPointer<QFile> stagingFile() const { return m_stagingFile; }
    void releaseStagingFile() { m_stagingFile.clear(); }
    // 清空句柄、路径、进度与归属信息（不删除磁盘文件）。
    void reset();

    // 逻辑条目定位（commitUpload 用）
    QString owner() const { return m_owner; }
    QString parentPath() const { return m_parent; }
    QString displayName() const { return m_display; }

private:
    // 目标文件或 .staging/*.part 的绝对路径。
    QString m_filePath;
    // confirmed offset：连续写入的字节数。
    qint64 m_receivedSize;
    // 会话开始时登记的预期大小。
    qint64 m_totalSize;
    QSharedPointer<QFile> m_stagingFile;   // REQ-EXT-TRANSFER-003：暂存打开句柄（仅 beginStaged 持有）
    // T4 暂存归属（仅 beginStaged 写入）
    QString m_owner;
    QString m_parent;
    QString m_display;
};

#endif // RESUMABLEUPLOADSESSION_H
