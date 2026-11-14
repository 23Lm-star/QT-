#include "uploadwritetask.h"

#include "logging.h"
#include "mytcpserver.h"

#include <QFile>
#include <QMetaObject>

// uploadwritetask.cpp — 线程池写任务实现：定位写入 + 结果队列回主线程。
// 不在任务里 open/close 文件，关闭成本与句柄竞争都留在会话侧。

UploadWriteTask::UploadWriteTask(quint64 connectionId, quint64 transferId,
                                 const QSharedPointer<QFile> &file,
                                 qint64 offset, const QByteArray &data)
    : m_connectionId(connectionId),
      m_transferId(transferId),
      m_file(file),
      m_offset(offset),
      m_data(data)
{
    // 由 QThreadPool 在 run() 返回后 delete，任务不得在其他地方手动释放。
    setAutoDelete(true);
}

void UploadWriteTask::run()
{
    // REQ-EXT-TRANSFER-003 S3：句柄由会话共享持有（已打开），此处只定位写入；
    // 不 open/不 close——关句柄的代价（Windows CreateFileW ≈ 13 ms）不在本路径上。
    // REQ-RESUME-AUDIT-001：写盘失败此前完全静默（客户端只见 -4006 无归因），补一条告警。
    const bool okOpen = !m_file.isNull() && m_file->isOpen();
    const bool okSeek = okOpen && m_file->seek(m_offset);
    const qint64 written = okSeek ? m_file->write(m_data) : qint64(-1);
    const bool okFlush = (written == m_data.size()) && m_file->flush();
    const bool success = okOpen && okSeek && (written == m_data.size()) && okFlush;
    if (!success) {
        qCWarning(lcFs) << "[transfer] 暂存写入失败 tid=" << m_transferId << "off=" << m_offset
                        << "open=" << okOpen << "seek=" << okSeek << "written=" << written
                        << "expect=" << m_data.size()
                        << "err=" << (okOpen ? m_file->errorString() : QStringLiteral("句柄已空"));
    }

    // 队列连接跨线程投递：连接/会话状态只在主线程修改。
    QMetaObject::invokeMethod(&MyTcpServer::getinstance(), "handleUploadWriteFinished",
                              Qt::QueuedConnection,
                              Q_ARG(quint64, m_connectionId),
                              Q_ARG(quint64, m_transferId),
                              Q_ARG(qint64, m_offset),
                              Q_ARG(qint64, static_cast<qint64>(m_data.size())),
                              Q_ARG(bool, success));
}
