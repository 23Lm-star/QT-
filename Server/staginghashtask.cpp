#include "staginghashtask.h"

#include "fileobjectstore.h"
#include "mytcpserver.h"

#include <QMetaObject>

// staginghashtask.cpp — 线程池复算实现：流式 SHA-256 + 结果队列回主线程。
// 不在任务里改任何连接/会话/DB 状态；一切判定与转正都在主线程完成后半段进行。

StagingHashTask::StagingHashTask(quint64 connectionId, quint64 transferId,
                                 const QString& stagingPath)
    : m_connectionId(connectionId),
      m_transferId(transferId),
      m_stagingPath(stagingPath)
{
    // 由 QThreadPool 在 run() 返回后 delete，任务不得在其他地方手动释放。
    setAutoDelete(true);
}

void StagingHashTask::run()
{
    QString shaHex;
    qint64 sizeBytes = -1;
    // 与原主线程复算逐字节同源：FileObjectStore::hashStaging（1MiB 流式，小写 hex）。
    // 该方法只开自己的只读句柄、不触 DB，可安全在线程池调用。
    const bool ok = FileObjectStore::instance().hashStaging(m_stagingPath, shaHex, sizeBytes);

    // 队列连接跨线程投递：转正、会话状态与应答只在主线程发生。
    QMetaObject::invokeMethod(&MyTcpServer::getinstance(), "handleStagingHashFinished",
                              Qt::QueuedConnection,
                              Q_ARG(quint64, m_connectionId),
                              Q_ARG(quint64, m_transferId),
                              Q_ARG(bool, ok),
                              Q_ARG(QString, shaHex),
                              Q_ARG(qint64, sizeBytes));
}
