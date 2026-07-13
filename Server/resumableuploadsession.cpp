#include "resumableuploadsession.h"

#include <QDir>
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

    BeginResult result;
    result.ok = false;
    result.offset = 0;
    result.complete = false;

    if (dirPath.isEmpty()
            || fileName.isEmpty()
            || fileName.contains('/')
            || fileName.contains('\\')
            || totalSize < 0) {
        return result;
    }

    QDir dir(dirPath);
    if (!dir.exists()) {
        return result;
    }

    const QString filePath = dir.filePath(fileName);
    QFileInfo fileInfo(filePath);
    qint64 offset = 0;

    if (resumeEnabled && fileInfo.exists() && fileInfo.isFile()) {
        const qint64 existsSize = fileInfo.size();
        if (existsSize > 0 && existsSize < totalSize) {
            offset = existsSize;
        } else if (existsSize == totalSize) {
            m_receivedSize = existsSize;
            m_totalSize = totalSize;
            result.ok = true;
            result.offset = existsSize;
            result.complete = true;
            return result;
        }
    }

    m_file.setFileName(filePath);
    QIODevice::OpenMode mode = QIODevice::WriteOnly;
    if (resumeEnabled && offset > 0) {
        mode |= QIODevice::Append;
    } else {
        mode |= QIODevice::Truncate;
    }

    if (!m_file.open(mode)) {
        reset();
        return result;
    }

    m_receivedSize = offset;
    m_totalSize = totalSize;
    result.ok = true;
    result.offset = offset;
    result.complete = isComplete();
    if (result.complete) {
        m_file.flush();
        m_file.close();
    }
    return result;
}

bool ResumableUploadSession::append(const char *data, qint64 len)
{
    if (data == nullptr || len < 0 || !m_file.isOpen()) {
        return false;
    }

    if (len == 0) {
        return true;
    }

    const qint64 remaining = m_totalSize - m_receivedSize;
    if (remaining <= 0) {
        return true;
    }

    const qint64 writeLen = len > remaining ? remaining : len;
    const qint64 writeCount = m_file.write(data, writeLen);
    if (writeCount != writeLen) {
        return false;
    }

    m_receivedSize += writeCount;
    if (isComplete()) {
        m_file.flush();
        m_file.close();
    }
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

void ResumableUploadSession::reset()
{
    if (m_file.isOpen()) {
        m_file.flush();
        m_file.close();
    }
    m_file.setFileName(QString());
    m_receivedSize = 0;
    m_totalSize = 0;
}
