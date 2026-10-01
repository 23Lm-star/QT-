#ifndef MYTCPSOCKET_H
#define MYTCPSOCKET_H
#include <QTcpSocket>
#include <QObject>
#include <QFile>
#include <QTimer>
#include <protocol.h>
#include "msghandler.h"
//进行一个客户端连接的类
class MyTcpSocket : public QTcpSocket
{
    Q_OBJECT
public:
    MyTcpSocket();
    ~MyTcpSocket();
public:
    //用来处理客户端PDU请求 的对象 指针
    MsgHandler* m_MsgHandler;
    //登录的用户名
    QString m_strLoginName;
    quint64 m_connectionId;
    // REQ-EXT-SEC-001：连接认证状态（仅服务端登录成功/注销/断线时写入，客户端不可覆盖）
    bool m_actorAuthenticated = false;
    quint64 m_actorSessionVersion = 0;
    // REQ-EXT-SESSION-001：登录时服务端事务分配的用户域版本（0=未认证），每请求处理前比对登记表
    quint64 m_loginVersion = 0;
    //存储传输的pdu字节流
    int a;
    QByteArray m_dpulist;
    uint m_invalidPduCount; // 丢包计数器
    static const uint MAX_INVALID_PDU_THRESHOLD = 5; // 丢包上限
    qint64 m_lastActiveMs;  // 最后收到客户端数据的时间(ms)，用于服务端僵尸连接检测
    //接收用户请求的三个过程： (循环)接收--处理--响应
    PDU* handlMsg(PDU* pdu);
    void seedMsg(PDU* respdu);

    //流式分块下载状态
    static const int DOWNLOAD_CHUNK_SIZE = 65536; // 64KB
    QFile* m_downloadFile;
    qint64 m_downloadTotalSize;
    qint64 m_downloadSentSize;
    bool m_downloadActive;
    bool m_downloadNextChunkScheduled;
    bool m_uploadWriteInFlight;
    static constexpr qint64 DOWNLOAD_WRITE_HIGH_WATER = 256 * 1024;
    void startDownloadStream(const QString& filePath);
    void scheduleNextDownloadChunk();
    void handleUploadData(PDU* pdu);
    void sendUploadAck(bool success, qint64 nextOffset, bool complete);
    // REQ-EXT-SEC-001：统一入口门——公开类型白名单 + 未认证拒绝响应
    static bool isPublicRequestType(uint uiType);
    PDU* authRejectPdu(uint uiType);
    // 所有权规则：
    // - 返回非空 PDU → 由 recvmsg() 调用 seedMsg() 写入并释放
    // - 返回 nullptr   → PDU 已由 handlMsg() 内部发送并释放（如下载流），或无需响应
    // 禁止同一 PDU 既返回又通过 sendAndRelease/seedMsg 发送
    // REQ-EXT-GROUP-002 T3：每连接有界发送队列（群消息广播用）。返回 false=队列满、
    // 该连接实时投递被丢弃（由拉取兜底），不影响其他成员。
    bool enqueueOutbound(const QByteArray &pdu);
    static const int OUTBOUND_QUEUE_MAX_MSGS = 256;
    static constexpr qint64 OUTBOUND_QUEUE_MAX_BYTES = 256 * 1024;
    static constexpr qint64 OUTBOUND_FLUSH_HIGH_WATER = 256 * 1024;
private:
    /// 发送 PDU 并释放其内存（所有权转移到此方法）
    /// pdu 为 nullptr 时安全空操作
    void sendAndRelease(PDU* pdu);
    // REQ-EXT-GROUP-002 T3：队列冲刷（事件循环内一次性调度）
    QList<QByteArray> m_outboundQueue;
    qint64 m_outboundQueueBytes = 0;
    bool m_outboundPumpScheduled = false;
    quint64 m_outboundDropCount = 0;
private slots:
    void sendNextChunk();
    void onBytesWritten(qint64 bytes);
    void pumpOutboundQueue();
public slots:
    void handleUploadWriteFinished(qint64 offset, qint64 length, bool success);
    void recvmsg();//接收信息引起的槽函数
    void clientOffLine();//客户端登录下线断开连接的槽函数

};

#endif // MYTCPSOCKET_H
