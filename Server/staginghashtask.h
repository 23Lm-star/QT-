#ifndef STAGINGHASHTASK_H
#define STAGINGHASHTASK_H

#include <QRunnable>
#include <QString>

// staginghashtask.h — 上传完成复算任务（QRunnable），由 QThreadPool 异步执行。
// REQ-EXT-PERF-002：整文件 SHA-256 复算从服务端主线程移入线程池，结果经队列连接
// 回主线程继续转正与迟到应答。任务只持路径副本——自开只读句柄、算完即关，不引用
// 连接/会话状态；上下文在复算期间被释放（abort/断线/覆盖）时回传结果由主线程侧丢弃。
class StagingHashTask : public QRunnable
{
public:
    StagingHashTask(quint64 connectionId, quint64 transferId, const QString& stagingPath);
    // 线程池回调：流式复算（复用 hashStaging，算法与 1MiB 分块不变），投递结果回主线程。
    void run() override;

private:
    // 回传结果时定位存活连接（连接可能已断开，找不到即丢弃）。
    quint64 m_connectionId;
    // 原样带回的上传会话标识，主线程按其在上下文表内校验在途复算。
    quint64 m_transferId;
    // 暂存文件绝对路径副本（不持句柄：Windows 下不阻塞后续 rename/remove）。
    QString m_stagingPath;
};

#endif // STAGINGHASHTASK_H
