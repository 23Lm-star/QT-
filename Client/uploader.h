// 上传正文发送器：INIT 被受理后按块读取本地文件，逐块等待服务端 32 确认再发下一块。
// 同一时刻只有一个块在途（停等协议），续传从服务端回带的偏移开始。
#ifndef UPLOADER_H
#define UPLOADER_H

#include "protocol.h"

#include <QObject>
#include <QFile>

// REQ-EXT-TRANSFER-003 C1：上传分片策略——按固定块数（1024）生成，夹在 [64 KiB, 1 MiB]。
// 上限**必须 ≤ 服务端 MAX_UPLOAD_DATA_CHUNK**（Server/protocol.h；跨端一致性由 verify.py S-02 静态校验）。
static constexpr qint64 UPLOAD_CHUNK_TARGET_COUNT = 1024;
static constexpr qint64 UPLOAD_CHUNK_MIN = 64 * 1024;
static constexpr qint64 UPLOAD_CHUNK_MAX = 1024 * 1024;

// 单个上传任务的正文发送状态机；由 TransferManager 在 GUI 线程创建和驱动
class Uploader : public QObject
{
    Q_OBJECT
public:
    // 本地源文件绝对路径
    QString strUploadPath;
    // INIT 回带的续传起点（服务端已收字节数）
    qint64 m_resumeOffset;

    // transferId 写入每个 32 请求的 caData，服务端据此归属会话
    Uploader(quint64 transferId, const QString& strUploadPath, qint64 resumeOffset=0, QObject* parent=nullptr);
    // 打开文件、计算块大小、seek 到续传点并发送首块
    void start();
    quint64 transferId() const { return m_transferId; }
public slots:
    // 处理 32 应答：校验偏移推进，complete 则收尾，否则继续下一块
    void acknowledge(bool success, qint64 nextOffset, bool complete);
signals:
    // 交给 Client 统一发送
    void seedMsg(PDU* pdu);
    // 不可恢复错误
    void errnoMsg(const QString& msg);
    // 终态（成功/失败只发一次）
    void finished(bool success);

private:
    // 读取并发送下一块；置 m_waitingAck 等待 32
    void sendNextChunk();
    // 幂等收尾：关文件并发出 finished
    void finish(bool success);

    // wire transfer_id
    quint64 m_transferId;
    // 源文件句柄
    QFile m_file;
    qint64 m_chunkSize = UPLOAD_CHUNK_MAX;   // C1：start() 按文件大小算定
    // 下一块起始偏移（服务端确认后推进）
    qint64 m_nextOffset;
    // 是否有块在途（停等控制）
    bool m_waitingAck;
    // start 后、finish 前为 true
    bool m_active;
    // 防止重复发 finished
    bool m_finished;
};

#endif // UPLOADER_H
