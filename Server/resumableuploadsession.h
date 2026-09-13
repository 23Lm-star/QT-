#ifndef RESUMABLEUPLOADSESSION_H
#define RESUMABLEUPLOADSESSION_H

#include <QString>

class ResumableUploadSession
{
public:
    struct BeginResult
    {
        bool ok;
        qint64 offset;
        bool complete;
    };

    ResumableUploadSession();
    ~ResumableUploadSession();

    BeginResult begin(const QString &dirPath,
                      const QString &fileName,
                      qint64 totalSize,
                      bool resumeEnabled);
    bool acceptWrittenChunk(qint64 offset, qint64 len);
    bool isComplete() const;
    qint64 receivedSize() const;
    qint64 totalSize() const;
    QString stagingFilePath() const;
    void reset();

private:
    QString m_filePath;
    qint64 m_receivedSize;
    qint64 m_totalSize;
};

#endif // RESUMABLEUPLOADSESSION_H
