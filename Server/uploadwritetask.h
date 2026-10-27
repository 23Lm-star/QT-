#ifndef UPLOADWRITETASK_H
#define UPLOADWRITETASK_H

#include <QRunnable>
#include <QByteArray>
#include <QString>
#include <QSharedPointer>

// uploadwritetask.h — 单块上传写入任务（QRunnable），由 QThreadPool 异步执行。
// 任务只持有数据副本与句柄共享引用，生命周期由线程池自动 delete（setAutoDelete）。

class QFile;

// REQ-EXT-TRANSFER-003 S3：写任务持暂存句柄的共享所有权（不 open/不 close），
// 只 seek+write+flush；ctx 先销毁也不会让在途写落在已关闭的句柄上。
// 上传写任务：在 QThreadPool 中执行一次定位写入，结果经队列连接回主线程。
class UploadWriteTask : public QRunnable
{
public:
    // data 按值拷贝：调用方栈/缓冲区可能在任务执行前失效。
    UploadWriteTask(quint64 connectionId, quint64 transferId, const QSharedPointer<QFile> &file,
                    qint64 offset, const QByteArray &data);
    // 线程池回调：seek 到 offset 写入并 flush，完成后把结果投递回主线程。
    void run() override;

private:
    // 回传结果时定位连接。
    quint64 m_connectionId;
    // 回传结果时定位传输会话。
    quint64 m_transferId;
    // 与会话共享的已打开暂存句柄。
    QSharedPointer<QFile> m_file;
    // 本块在文件中的绝对偏移。
    qint64 m_offset;
    // 本块数据副本。
    QByteArray m_data;
};

#endif // UPLOADWRITETASK_H
