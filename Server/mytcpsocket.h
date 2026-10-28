#ifndef MYTCPSOCKET_H
#define MYTCPSOCKET_H
#include <QSslSocket>
#include <QSslConfiguration>
#include <QObject>
#include <QFile>
#include <QTimer>
#include <QHash>
#include <QList>
#include <protocol.h>
#include "msghandler.h"
// 每个实例对应一条客户端 TCP 连接，归属 MyTcpServer 所在的事件循环线程
// （Qt 父子/信号槽保证槽函数在该线程执行；msghandler 因此无需加锁）。
// REQ-EXT-TLS-001：基类改 QSslSocket。明文端口实例从不发起加密，行为等同 QTcpSocket；
// TLS 端口实例由 setTlsMode(true) 登记，握手完成（encrypted）前 recvmsg 不做 PDU 解析。
// 职责：粘包/半包解析（recvmsg）、PDU 分派（handlMsg）、上传写盘与下载分块流、
// 认证/会话版本校验，以及断线时的在线态与资源清理。
// 所有权：连接由 MyTcpServer::incomingConnection 创建，断开后经 removeSocket 用 deleteLater 销毁。
//进行一个客户端连接的类
class MyTcpSocket : public QSslSocket
{
    Q_OBJECT
public:
    // 构造时连接 readyRead/bytesWritten/disconnected，并创建 MsgHandler 与空闲回收定时器。
    MyTcpSocket();
    // 释放未完成的下载文件句柄与 MsgHandler；不写数据库（下线写由 clientOffLine 负责）。
    ~MyTcpSocket();
    // REQ-EXT-TLS-001：TLS 端口连接登记（须在 setSocketDescriptor 后调用）；
    // tls=true 时发起握手并挂接 encrypted 后处理，握手完成前不解析 PDU。
    void setTlsMode(bool tls);
    // 从 exe 目录 certs/server.{crt,key} 构建服务端 TLS 配置（D4 固定目录约定）。
    // 证书缺失或解析失败返回 isNull 配置，调用方据此跳过 TLS 监听并在日志说明。
    static QSslConfiguration buildTlsConfig();
public:
    //用来处理客户端PDU请求 的对象 指针
    MsgHandler* m_MsgHandler;
    //登录的用户名
    QString m_strLoginName;
    // 服务端分配的连接号（单调递增），用于跨线程回调（如上传写盘完成）精确定位连接。
    quint64 m_connectionId;
    // REQ-EXT-SEC-001：连接认证状态（仅服务端登录成功/注销/断线时写入，客户端不可覆盖）
    bool m_actorAuthenticated = false;
    // 本连接认证代次：登录/注销/断线时自增，供日志区分同一连接的不同身份阶段。
    quint64 m_actorSessionVersion = 0;
    // REQ-EXT-SESSION-001：登录时服务端事务分配的用户域版本（0=未认证），每请求处理前比对登记表
    quint64 m_loginVersion = 0;
    //存储传输的pdu字节流
    // 历史遗留计数（未参与逻辑），保留以兼容旧代码/调试。
    int a;
    // 未解析的入站字节缓存：可能包含多个 PDU 或半个 PDU，recvmsg 循环消费。
    QByteArray m_dpulist;
    uint m_invalidPduCount; // 丢包计数器
    static const uint MAX_INVALID_PDU_THRESHOLD = 5; // 丢包上限
    qint64 m_lastActiveMs;  // 最后收到客户端数据的时间(ms)，用于服务端僵尸连接检测
    //接收用户请求的三个过程： (循环)接收--处理--响应
    // 分派单个完整 PDU；返回的 PDU 由调用方 seedMsg 发送并释放（见下方所有权规则）。
    PDU* handlMsg(PDU* pdu);
    // 发送响应并 memfree（所有权转移给本方法）；respdu 为 nullptr 时无操作。
    void seedMsg(PDU* respdu);

    // ===== REQ-EXT-TRANSFER-001：per-transfer 流式分块下载（单调度环 + 单定时器，§4.1） =====
    static const int DOWNLOAD_CHUNK_SIZE = 65536; // 64KB
    // 单条下载流的状态。file 由本连接独占并负责 close/delete（断线、完成、abort 三条路径）。
    struct DownloadStream {
        // 传输 id（0=旧单流路径）
        quint64 id = 0;
        // 已按续传起点 seek 的只读句柄
        QFile*  file = nullptr;
        // 文件总字节数
        qint64  totalSize = 0;
        // 已发出的字节数（含续传起点前已存在的部分）
        qint64  sentSize = 0;
        qint64  remaining() const { return totalSize - sentSize; }
    };
    QHash<quint64, DownloadStream> m_downloads;   // key 0 = 旧单流路径
    QList<quint64> m_downloadRing;                // round-robin 环（活跃顺序）
    // 下一块轮到的环下标
    int m_rrCursor = 0;
    // 防止重复投递 singleShot（每 tick 只调度一次）
    bool m_downloadNextChunkScheduled;
    static constexpr qint64 DOWNLOAD_WRITE_HIGH_WATER = 256 * 1024;
    // REQ-EXT-TRANSFER-002 S3：startOffset = 采纳的续传起点（0 = 从头；调用方已按 §2.3 先行拒绝非法起点）
    // 打开文件并按起点 seek，加入轮转环；失败时回 43(success=false) 且不建流。
    void startDownloadStream(const QString& filePath, quint64 transferId, qint64 startOffset);
    // 若环非空且尚无挂起的 tick，投递一次 sendNextChunk（事件循环合并写）。
    void scheduleNextDownloadChunk();
    // 从环与哈希中移除一条流并关闭/释放文件；同时修正轮转游标。
    void removeDownloadStream(quint64 id);
    // 发送 43 下载完成 PDU（带 id），随后由 seedMsg 释放。
    void seedDownloadFinish(quint64 id, bool success);
    // 31 上传数据块：校验偏移/魔数/在途写，然后投递线程池写盘。
    void handleUploadData(PDU* pdu);
    // 发送 32 上传应答：success、服务端期望的下一个偏移、是否完成，均回带 transfer_id。
    void sendUploadAck(quint64 transferId, bool success, qint64 nextOffset, bool complete);
    // 107/108：传输任务释放（暂停/取消共用；REQ-EXT-TRANSFER-002 S1 起上传/下载同义）
    PDU* handleTransferAbort(PDU* pdu);
    // D5：上传上下文空闲回收（连接级定时器，间隔 15s）
    static const int UPLOAD_IDLE_REAP_SEC = 60;
    // 父对象为 this，随连接销毁
    QTimer* m_reapTimer;
    // 定时回调：让 MsgHandler 回收长时间无进展的上传上下文（写盘在途的不回收）。
    void onReapTimeout();
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
    // 单连接待发条数上限
    static const int OUTBOUND_QUEUE_MAX_MSGS = 256;
    // 单连接待发字节上限
    static constexpr qint64 OUTBOUND_QUEUE_MAX_BYTES = 256 * 1024;
    // Qt 写缓冲高水位：低于它才继续冲刷/入队
    static constexpr qint64 OUTBOUND_FLUSH_HIGH_WATER = 256 * 1024;
private:
    /// 发送 PDU 并释放其内存（所有权转移到此方法）
    /// pdu 为 nullptr 时安全空操作
    void sendAndRelease(PDU* pdu);
    // REQ-EXT-GROUP-002 T3：队列冲刷（事件循环内一次性调度）
    // 已序列化的待发 PDU（值语义，生命周期独立于源 PDU）
    QList<QByteArray> m_outboundQueue;
    // 队列当前字节数（与条数共同限流）
    qint64 m_outboundQueueBytes = 0;
    // 已投递 singleShot，避免重复调度
    bool m_outboundPumpScheduled = false;
    // 因队列满被丢弃的实时投递计数（审计用）
    quint64 m_outboundDropCount = 0;
    // REQ-EXT-TLS-001：本连接是否来自 TLS 端口（握手完成前禁止 PDU 解析）
    bool m_tlsMode = false;
private slots:
    // 下载调度 tick：背压允许时只发一块，然后重新调度。
    void sendNextChunk();
    // socket 写缓冲回落：触发下一块下载与出站队列冲刷。
    void onBytesWritten(qint64 bytes);
    // 把出站队列中可写部分写入 socket（保持总排队量有界）。
    void pumpOutboundQueue();
public slots:
    // 线程池写盘完成后回到主线程：更新上传进度、必要时收尾或释放上下文。
    void handleUploadWriteFinished(quint64 transferId, qint64 offset, qint64 length, bool success);
    // REQ-EXT-PERF-002：线程池复算完成后回到主线程：转正落定并按需迟到补发 32/98/102。
    void handleStagingHashFinished(quint64 transferId, bool ok,
                                   const QString& shaHex, qint64 sizeBytes);
    void recvmsg();//接收信息引起的槽函数
    void clientOffLine();//客户端登录下线断开连接的槽函数
    // REQ-EXT-TLS-001：TLS 握手完成入口——把等待期已到达的字节转入正常 PDU 解析。
    void onTlsEncrypted();

};

#endif // MYTCPSOCKET_H
