#include "resumableuploadsession.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

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

void ResumableUploadSession::reset()
{
    m_filePath.clear();
    m_receivedSize = 0;
    m_totalSize = 0;
}
