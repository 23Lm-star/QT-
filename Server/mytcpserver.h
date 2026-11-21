#ifndef MYTCPSERVER_H
#define MYTCPSERVER_H

#include <QObject>
#include <QTcpServer>
#include <QSslConfiguration>
#include <QMutex>
#include <QHash>
#include <QStringList>
#include "mytcpsocket.h"
// 连接管理器（单例）：维护全部在线连接、登录版本登记表、分享握手登记与僵尸清扫。
// 线程归属：随 main 的事件循环运行在主线程；tcpsocketlist 由其互斥量保护，
// 任何涉及 socket 的写操作都必须在锁外或锁内短临界区完成（避免回环死锁）。
// 锁序约定：DB → m_registryMutex → m_tcpsocketlistMutex，禁止反向获取。
//管理所有客户端连接的一个类
class MyTcpServer : public QTcpServer
{
    Q_OBJECT
public:
    //单例模式，一个实例管理所有的客户端连接
    static MyTcpServer& getinstance();
    // 禁止拷贝：内部持有互斥量与连接表，复制无意义且危险。
    MyTcpServer(const MyTcpServer&)=delete;
    MyTcpServer& operator=(const MyTcpServer&)=delete;
public:
    //进行信号监听
    // 新连接入口：创建 MyTcpSocket、分配连接号并登记到连接表（锁内）。
    void incomingConnection(qintptr handle) override;
    // REQ-EXT-TLS-001 D1：在 主端口+1 上启动 TLS 监听（双端口共存，5000 明文语义不变）。
    // 配置无效（certs 缺失/解析失败）时不监听并返回 false；成功返回 true。
    // 由 Server::initialize 在明文监听成功后调用；僵尸清扫定时器只在单例上运行，
    // TLS 监听实例复用同一连接表与广播路由。
    bool startTlsListener(const QSslConfiguration &cfg);
    //移除已经和客户端断开连接的socket对象
    // 先从表内摘除，再 deleteLater（不在槽函数栈内直接析构自己）。
    void removeSocket(MyTcpSocket* m);
    //添加好友
    // 向指定在线用户转发 PDU；不接管 pdu 所有权（调用方负责释放）。
    void seedAddFriend(char* tarname,PDU*pdu);
    //单次拥有型转发（供 addfriend() 在线路径使用，写后 memfree）
    void sendAddFriendAndRelease(char* tarname, PDU* pdu);
    //信息转发
    // 与 seedAddFriend 相同但用于聊天消息；同样不接管所有权。
    // REQ-AUDIT-001 R1：返回"找到目标连接且 write() 未报错"，调用方据此决定是否标记已送达。
    bool reseed(char*tarname,PDU*pdu);
    // REQ-CHAT-ACK-001：按目标连接能力投递私聊。
    // 目标声明过 116（桌面端）→ 只发 109 并等客户端 110；否则发 15 并按旧语义处理。
    // 返回：0=目标不在线；1=已按 ACK 模式投递（调用方不得标记）；2=已按旧模式投递（可标记）
    int deliverChat(char* tarname, PDU* ackPdu, PDU* legacyPdu);
    // REQ-EXT-GROUP-001：向当前在线群成员广播已提交的成员/邀请事件。
    // 事件 PDU 为一次性所有权：内部广播后 memfree，调用方不得再访问。
    void sendGroupEvent(const QStringList& users, PDU* pdu);
    // REQ-EXT-GROUP-002 T3：群消息在线广播——锁内仅收集目标连接快照，锁外逐连接入
    // 每连接有界发送队列；队列满只丢该连接实时投递（拉取兜底），不阻塞事务与其他成员。
    void broadcastGroupMessage(const QStringList& users, PDU* pdu);
    // REQ-EXT-SEC-001 M-1: 分享握手登记表——分享人发出邀请时登记 (接收者, 服务端 canonical 源路径)，
    // 接收者同意移动时仅放行有登记的条目并在成功后消费。仅事件循环主线程（socket 处理器）访问，无需加锁。
    void addPendingShare(const QString& receiver, const QString& canonicalSource);
    bool hasPendingShare(const QString& receiver, const QString& canonicalSource) const;
    void consumePendingShare(const QString& receiver, const QString& canonicalSource);
    // REQ-EXT-SESSION-001 D3/D5：用户域登录版本内存登记表（权威持久源=库；进程重启
    // 由重连强制登录自然重建）。登录成功先 set 再踢旧；校验=内存比对，热路径零 DB。
    void setLoginVersion(const QString& user, quint64 ver);
    quint64 currentLoginVersion(const QString& user);   // 无登记返回 0
    void removeLoginVersion(const QString& user);       // 权威会话下线、或账号被注销（删除 user_info 行、计数器归零）时移除对应项
    // 关闭该用户名下已认证且版本≠登记表现值的全部存量连接（锁内快照、出锁 abort，仿僵尸扫描纪律；
    // 现值取单调登记表，并发下自含式正确：低版本登录者也会把自己关掉）
    void closeStaleSockets(const QString& user);
    // REQ-AUTH-CONNECTION-001：121 恢复接管——关闭该用户除 keep 外的全部已认证连接
    // （含同版本的旧物理传输）。被关连接先置 m_loginVersion=0 走 stale 分支，
    // 不得触发下线写/在线键删除/登记表移除，保护刚恢复的逻辑会话。
    void closeUserSockets(const QString& user, MyTcpSocket* keep);
public slots:
    void handleUploadWriteFinished(quint64 connectionId, quint64 transferId,
                                   qint64 offset, qint64 length, bool success);
    // REQ-EXT-PERF-002：复算任务回主线程的入口——同 handleUploadWriteFinished 按连接号定位，
    // 连接已断开（指针不在表内）时直接丢弃结果。
    void handleStagingHashFinished(quint64 connectionId, quint64 transferId,
                                   bool ok, const QString& shaHex, qint64 sizeBytes);
private:
    MyTcpServer();
    // REQ-EXT-TLS-001：TLS 监听实例（非单例；仅 its incomingConnection 走 TLS 模式）。
    // 单例的清扫定时器负责僵尸回收，监听实例不重复起定时器。
    static MyTcpServer *s_tlsListener;
    // 本实例是否以 TLS 模式接受连接（仅 s_tlsListener 为 true）。
    bool m_isTlsListener = false;
    // TLS 监听器共享的服务端证书配置（含本地证书与私钥）。
    QSslConfiguration m_tlsCfg;
    //记录所有已经建立连接的 socket对象 链表
    // 仅由主线程修改；跨函数遍历/快照时须持有下面的互斥量。
    QList<MyTcpSocket*>tcpsocketlist;
    QMutex m_tcpsocketlistMutex;
    // REQ-EXT-SESSION-001：用户域登录版本登记表（独立短临界区锁；锁序 DB→registry→socketlist）
    QHash<QString,quint64> m_loginVersionRegistry;
    QMutex m_registryMutex;
    // 连接号分配器：只增不减，0 保留给未登记连接。
    quint64 m_nextConnectionId = 0;

    //僵尸连接检测：每隔一段时间扫描，静默超过阈值的连接直接 abort 清理
    qint64 m_zombieTimeoutMs;   // 阈值(ms)，由 zombie_timeout.cfg 读取秒数，默认 90000 (90s)
    QTimer* m_sweepTimer;
    void onSweepTimeout();

    // 分享握手记录：接收者 + 服务端规范化后的源路径 + 登记时刻（用于 TTL 惰性清理）。
    struct PendingShare { QString receiver; QString canonicalSource; qint64 addedMs; };
    QList<PendingShare> m_pendingShares;    // 30 分钟过期、上限 1024，添加时惰性清理
    static const qint64 PENDING_SHARE_TTL_MS = 30*60*1000;
    static const int PENDING_SHARE_MAX = 1024;
};

#endif // MYTCPSERVER_H
