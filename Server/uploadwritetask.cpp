#include "uploadwritetask.h"

#include "mytcpserver.h"

#include <QFile>
#include <QMetaObject>

UploadWriteTask::UploadWriteTask(quint64 connectionId, quint64 transferId, const QString &filePath,
                                 qint64 offset, const QByteArray &data)
    : m_connectionId(connectionId),
      m_transferId(transferId),
      m_filePath(filePath),
      m_offset(offset),
      m_data(data)
{
    setAutoDelete(true);
}

void UploadWriteTask::run()
{
    QFile file(m_filePath);
    bool success = file.open(QIODevice::ReadWrite)
            && file.seek(m_offset)
            && file.write(m_data) == m_data.size()
            && file.flush();
    file.close();

    QMetaObject::invokeMethod(&MyTcpServer::getinstance(), "handleUploadWriteFinished",
                              Qt::QueuedConnection,
                              Q_ARG(quint64, m_connectionId),
                              Q_ARG(quint64, m_transferId),
                              Q_ARG(qint64, m_offset),
                              Q_ARG(qint64, static_cast<qint64>(m_data.size())),
                              Q_ARG(bool, success));
}
