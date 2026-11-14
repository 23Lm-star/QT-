// 上传发送器实现：停等式分块发送，任意时刻至多一块在途。
#include "protocol.h"
#include "uploader.h"

#include <QTimer>
#include <cstring>

// 仅记录参数，不做 IO；真正的打开与续传定位在 start()
Uploader::Uploader(quint64 transferId, const QString &strUploadPath, qint64 resumeOffset, QObject *parent)
    : QObject(parent),
      strUploadPath(strUploadPath),
      m_resumeOffset(resumeOffset),
      m_transferId(transferId),
      m_nextOffset(0),
      m_waitingAck(false),
      m_active(false),
      m_finished(false)
{
}

// 初始化：打开源文件、按总大小定块长（1024 块、夹在 64KiB..1MiB）、seek 到续传点
void Uploader::start()
{
    // 防重入
    if (m_active) {
        return;
    }

    m_file.setFileName(strUploadPath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        emit errnoMsg("Upload file open failed");
        finish(false);
        return;
    }

    m_nextOffset = qMax<qint64>(0, m_resumeOffset);
    // C2：chunk = clamp(ceil(size / 1024), 64 KiB, 1 MiB)；size==0 取 MIN（由 C3 兜底不发送）
    const qint64 totalSize = m_file.size();
    m_chunkSize = qBound<qint64>(UPLOAD_CHUNK_MIN,
                                 (totalSize + UPLOAD_CHUNK_TARGET_COUNT - 1) / UPLOAD_CHUNK_TARGET_COUNT,
                                 UPLOAD_CHUNK_MAX);
    if (m_nextOffset > m_file.size() || !m_file.seek(m_nextOffset)) {
        emit errnoMsg("Upload file seek failed");
        finish(false);
        return;
    }

    m_active = true;
    sendNextChunk();
}

// 32 应答处理：推进偏移；成功且未完成时用 singleShot(0) 让出事件循环再发下一块
void Uploader::acknowledge(bool success, qint64 nextOffset, bool complete)
{
    // 迟到/重复应答直接忽略
    if (!m_active || !m_waitingAck) {
        return;
    }

    m_waitingAck = false;
    // REQ-RESUME-AUDIT-001 M4：偏移必须严格连续（= 本块起点 + 本块长度），
    // 跳跃/回退一律失败收口，防止错位数据继续发送。
    if (!success || nextOffset != m_nextOffset + m_sentLen) {
        emit errnoMsg("Upload chunk rejected");
        finish(false);
        return;
    }

    m_nextOffset = nextOffset;
    if (complete) {
        finish(true);
        return;
    }

    QTimer::singleShot(0, this, &Uploader::sendNextChunk);
}

// 读取 m_nextOffset 处的一块并发出 31；caData 携带块偏移、续传魔数与 transfer_id
void Uploader::sendNextChunk()
{
    if (!m_active || m_waitingAck) {
        return;
    }

    // REQ-RESUME-AUDIT-001 M4：显式定位读取，不依赖顺序读的隐式游标。
    if (!m_file.seek(m_nextOffset)) {
        emit errnoMsg("Upload file seek failed");
        finish(false);
        return;
    }
    const QByteArray chunk = m_file.read(m_chunkSize);
    m_sentLen = chunk.size();
    // 未收到 complete 就 EOF，说明服务端确认落后于本地读取
    if (chunk.isEmpty()) {
        emit errnoMsg("Upload file ended before server confirmation");
        finish(false);
        return;
    }

    PDU *pdu = mkPDU(chunk.size());
    pdu->uiType = ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST;
    const uint resumeMagic = UPLOAD_RESUME_MAGIC;
    memcpy(pdu->caData + UPLOAD_CHUNK_OFFSET, &m_nextOffset, sizeof(m_nextOffset));
    memcpy(pdu->caData + UPLOAD_RESUME_MAGIC_OFFSET, &resumeMagic, sizeof(resumeMagic));
    memcpy(pdu->caData + TRANSFER_ID_OFFSET, &m_transferId, sizeof(m_transferId));   // REQ-EXT-TRANSFER-001
    memcpy(pdu->caMsg, chunk.constData(), chunk.size());
    m_waitingAck = true;
    emit seedMsg(pdu);
}

// 幂等收尾：置终态、关文件、发一次 finished；此后所有回调均落空
void Uploader::finish(bool success)
{
    if (m_finished) {
        return;
    }

    m_finished = true;
    m_active = false;
    m_waitingAck = false;
    if (m_file.isOpen()) {
        m_file.close();
    }
    emit finished(success);
}
