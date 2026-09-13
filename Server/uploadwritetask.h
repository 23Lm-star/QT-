#ifndef UPLOADWRITETASK_H
#define UPLOADWRITETASK_H

#include <QRunnable>
#include <QByteArray>
#include <QString>

class UploadWriteTask : public QRunnable
{
public:
    UploadWriteTask(quint64 connectionId, const QString &filePath,
                    qint64 offset, const QByteArray &data);
    void run() override;

private:
    quint64 m_connectionId;
    QString m_filePath;
    qint64 m_offset;
    QByteArray m_data;
};

#endif // UPLOADWRITETASK_H
