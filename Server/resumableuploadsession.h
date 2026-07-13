#ifndef RESUMABLEUPLOADSESSION_H
#define RESUMABLEUPLOADSESSION_H

#include <QFile>
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
    bool append(const char *data, qint64 len);
    bool isComplete() const;
    qint64 receivedSize() const;
    qint64 totalSize() const;
    void reset();

private:
    QFile m_file;
    qint64 m_receivedSize;
    qint64 m_totalSize;
};

#endif // RESUMABLEUPLOADSESSION_H
