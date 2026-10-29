#include "logging.h"
#include "mytcpserver.h"
#include "mytcpsocket.h"
#include "tlspinning.h"   // Server.pro 跨目录引用 Client/tlspinning.cpp：pinning/TLS 端口规则只有一份
#include "server.h"
#include "protocol.h"
#include "memalloc.h"
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
#include <QSet>
#include <QSslKey>
#include <QSslCertificate>
// 连接管理实现。所有槽函数运行在主线程；对连接表的遍历一律先加锁取快照，
// 出锁后再执行可能触发重入（abort → disconnected → clientOffLine → removeSocket）的操作。
MyTcpServer::MyTcpServer(){
    // 读取僵尸连接阈值（zombie_timeout.cfg，单位秒；默认 90s = 3×30s 心跳周期）
    m_zombieTimeoutMs = 90000;
    const QString cfg = QCoreApplication::applicationDirPath() + "/zombie_timeout.cfg";
    QFile cf(cfg);
    if (cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        bool ok = false;
        const qint64 secs = cf.readAll().trimmed().toLongLong(&ok);
        cf.close();
        if (ok && secs > 0) {
            m_zombieTimeoutMs = secs * 1000;
            qCDebug(lcNet) << "[ZOMBIE] 阈值配置:" << secs << "s (" << m_zombieTimeoutMs << "ms)";
        } else {
            qCDebug(lcNet) << "[ZOMBIE] 阈值配置无效，使用默认 90s";
        }
    } else {
        qCDebug(lcNet) << "[ZOMBIE] 未找到 zombie_timeout.cfg，使用默认阈值 90s";
    }

    // 周期扫描由父对象 this 托管，随单例析构自动停止。
    // REQ-EXT-TLS-001：TLS 监听实例（第二个 MyTcpServer）不重复起清扫定时器——
    // 连接表是全进程共享的，单例每 30s 扫一遍已覆盖两类连接。
    static bool s_firstInstanceDone = false;
    if (!s_firstInstanceDone) {
        m_sweepTimer = new QTimer(this);
        connect(m_sweepTimer, &QTimer::timeout, this, &MyTcpServer::onSweepTimeout);
        m_sweepTimer->start(30000);   // 每 30s 扫描一次僵尸连接
        s_firstInstanceDone = true;
    }
}

// REQ-EXT-TLS-001 D1：TLS 监听器。端口 = 明文主端口 + 1（配置格式不加行，D4 已确认）。
bool MyTcpServer::startTlsListener(const QSslConfiguration &cfg)
{
    if (s_tlsListener) return false;                 // 只允许启动一次
    if (cfg.isNull() || cfg.localCertificate().isNull() || cfg.privateKey().isNull()) {
        qCCritical(lcNet) << "[TLS] 配置无效，TLS 端口不启用（明文端口不受影响）";
        return false;
    }
    // MyTcpServer 的私有构造在静态成员函数内可直接调用（C++ 访问控制按类而非按实例）。
    // 不释放：监听器与进程同寿命，退出时无需有序销毁（与单例同一策略）。
    MyTcpServer *listener = new MyTcpServer();
    listener->m_isTlsListener = true;
    listener->m_tlsCfg = cfg;
    MyTcpServer &master = MyTcpServer::getinstance();
    quint16 tlsPort = 0;
    // S-1：主端口 65535 时 +1 会回绕到 0，listen(0) 会随机挑端口却照样打「启动成功」——直接拒绝
    if (!tls::nextTlsPort(master.serverPort(), &tlsPort)) {
        qCCritical(lcNet) << "[TLS] 主端口" << master.serverPort()
                          << "无法派生 TLS 端口（+1 越界或为 0），TLS 端口不启用（明文端口不受影响）";
        delete listener;
        return false;
    }
    if (!listener->listen(master.serverAddress(), tlsPort)) {
        qCCritical(lcNet) << "[TLS] TLS 端口监听失败:" << listener->errorString()
                          << "端口" << tlsPort << "（明文端口不受影响）";
        delete listener;
        return false;
    }
    s_tlsListener = listener;
    qCDebug(lcNet) << "[TLS] TLS 端口启动成功，监听" << tlsPort;
    return true;
}

MyTcpServer *MyTcpServer::s_tlsListener = nullptr;
//连接新客户端，并且创建新的tcpsocket用来连接新客户端
void MyTcpServer::incomingConnection(qintptr handle)
{
    qCDebug(lcNet)<<"新客户端连接"<<endl;
    // handle 是系统 socket 描述符，交给 QAbstractSocket 接管后由 Qt 负责关闭。
    // 新对象无父对象：生命周期由连接表 + removeSocket(deleteLater) 管理。
    MyTcpSocket * m_tcpSocket=new MyTcpSocket;
    m_tcpSocket->setSocketDescriptor(handle);
    // REQ-EXT-TLS-001：TLS 监听实例接受的连接先登记 TLS 模式再进入连接表，
    // 保证握手完成前该连接不会被当作 PDU 流解析（见 MyTcpSocket::recvmsg 守卫）。
    if (m_isTlsListener) m_tcpSocket->setTlsMode(true);
    {
        QMutexLocker locker(&m_tcpsocketlistMutex);
        m_tcpSocket->m_connectionId=++m_nextConnectionId;
        tcpsocketlist.append(m_tcpSocket);
        for(int i=0;i<tcpsocketlist.size();i++){
            qCDebug(lcNet)<<tcpsocketlist[i]<<endl;
        }
    }
    qCDebug(lcNet)<<"新客户端连接完成"<<endl;
}

// 线程池写盘完成后的主线程回调：按连接号找到存活连接再转交；
// 连接可能已断开（指针不在表内），此时直接丢弃结果。
void MyTcpServer::handleUploadWriteFinished(quint64 connectionId, quint64 transferId,
                                            qint64 offset, qint64 length, bool success)
{
    QMutexLocker locker(&m_tcpsocketlistMutex);
    for(MyTcpSocket* socket : tcpsocketlist){
        if(socket->m_connectionId == connectionId){
            socket->handleUploadWriteFinished(transferId,offset,length,success);
            return;
        }
    }
}

// REQ-EXT-PERF-002：复算结果路由——连接可能已断开（指针不在表内），此时直接丢弃结果。
void MyTcpServer::handleStagingHashFinished(quint64 connectionId, quint64 transferId,
                                            bool ok, const QString& shaHex, qint64 sizeBytes)
{
    QMutexLocker locker(&m_tcpsocketlistMutex);
    for(MyTcpSocket* socket : tcpsocketlist){
        if(socket->m_connectionId == connectionId){
            socket->handleStagingHashFinished(transferId,ok,shaHex,sizeBytes);
            return;
        }
    }
}

MyTcpServer &MyTcpServer::getinstance()
{
    static MyTcpServer instance;
    return instance;
}
//移除已经与客户端断连的socket对象
void MyTcpServer::removeSocket(MyTcpSocket *m)
{
    {
        QMutexLocker locker(&m_tcpsocketlistMutex);
        this->tcpsocketlist.removeOne(m);
        for(int i=0;i<tcpsocketlist.size();i++){
            qCDebug(lcNet)<<tcpsocketlist[i]<<endl;
        }
    }
    m->deleteLater();
    m=nullptr;
}

//发送指定的客户端发送添加好友的消息
void MyTcpServer::seedAddFriend(char *tarname, PDU *pdu)
{
    if(tarname==nullptr||pdu==nullptr){
        return;
    }
    QMutexLocker locker(&m_tcpsocketlistMutex);
    for(int i=0;i<tcpsocketlist.size();i++){
        if(strcmp(tcpsocketlist[i]->m_strLoginName.toStdString().c_str(),tarname)==0){
            tcpsocketlist[i]->write((char*)pdu,pdu->uiTotalLen);
            break;
        }
    }
}

//单次拥有型转发：写出后无条件 memfree，调用方不得再访问 PDU
void MyTcpServer::sendAddFriendAndRelease(char *tarname, PDU *pdu)
{
    if(pdu==nullptr){
        return;
    }
    {
        QMutexLocker locker(&m_tcpsocketlistMutex);
        for(int i=0;i<tcpsocketlist.size();i++){
            if(tarname!=nullptr && strcmp(tcpsocketlist[i]->m_strLoginName.toStdString().c_str(),tarname)==0){
                tcpsocketlist[i]->write((char*)pdu,pdu->uiTotalLen);
                break;
            }
        }
    }
    memfree(pdu);
}
//给客户端转发pdu消息
void MyTcpServer::reseed(char *tarname, PDU *pdu){
    if(tarname==nullptr||pdu==nullptr){
        return;
    }
    QMutexLocker locker(&m_tcpsocketlistMutex);
    for(int i=0;i<tcpsocketlist.size();i++){
        if(strcmp(tcpsocketlist[i]->m_strLoginName.toStdString().c_str(),tarname)==0){
            tcpsocketlist[i]->write((char*)pdu,pdu->uiTotalLen);
            break;
        }
    }
}

void MyTcpServer::sendGroupEvent(const QStringList& users, PDU *pdu)
{
    if (pdu == nullptr || users.isEmpty()) {
        if (pdu != nullptr) memfree(pdu);
        return;
    }
    QSet<QString> targets;
    for (const QString& user : users) {
        if (!user.isEmpty()) targets.insert(user);
    }
    QMutexLocker locker(&m_tcpsocketlistMutex);
    QSet<QString> delivered;
    for (MyTcpSocket* socket : tcpsocketlist) {
        if (socket == nullptr || !socket->m_actorAuthenticated || delivered.contains(socket->m_strLoginName)
                || !targets.contains(socket->m_strLoginName)) {
            continue;
        }
        socket->write(reinterpret_cast<const char*>(pdu), pdu->uiTotalLen);
        delivered.insert(socket->m_strLoginName);
    }
    memfree(pdu);
}

// REQ-EXT-GROUP-002 T3：群消息在线广播——持 m_tcpsocketlistMutex 仅收集目标连接
// 指针快照，随即释放锁；锁外逐连接写入其每连接有界发送队列（绝不持全局锁写 socket）。
void MyTcpServer::broadcastGroupMessage(const QStringList& users, PDU *pdu)
{
    if (pdu == nullptr) return;
    if (users.isEmpty()) { memfree(pdu); return; }
    QSet<QString> targets;
    for (const QString& user : users) {
        if (!user.isEmpty()) targets.insert(user);
    }
    QList<MyTcpSocket*> snapshot;
    {
        QMutexLocker locker(&m_tcpsocketlistMutex);
        snapshot = tcpsocketlist;
    }
    const QByteArray payload(reinterpret_cast<const char*>(pdu), int(pdu->uiTotalLen));
    memfree(pdu);
    int dropped = 0;
    for (MyTcpSocket* socket : snapshot) {
        if (socket == nullptr || !socket->m_actorAuthenticated
                || !targets.contains(socket->m_strLoginName)) {
            continue;
        }
        if (!socket->enqueueOutbound(payload)) ++dropped;
    }
    if (dropped > 0) {
        qCWarning(lcNet) << "[GROUP] 群消息广播队列满：丢弃实时投递连接数=" << dropped
                         << "目标成员数=" << users.size();
    }
}

// REQ-EXT-SEC-001 M-1：分享握手登记（仅主线程访问）
void MyTcpServer::addPendingShare(const QString& receiver, const QString& canonicalSource)
{
    if(receiver.isEmpty() || canonicalSource.isEmpty()){
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for(int i=m_pendingShares.size()-1;i>=0;--i){
        if(now - m_pendingShares[i].addedMs > PENDING_SHARE_TTL_MS){
            m_pendingShares.removeAt(i);
        }
    }
    while(m_pendingShares.size() >= PENDING_SHARE_MAX){
        m_pendingShares.removeFirst();
    }
    m_pendingShares.append(PendingShare{receiver, canonicalSource, now});
}

bool MyTcpServer::hasPendingShare(const QString& receiver, const QString& canonicalSource) const
{
    for(const PendingShare& ps : m_pendingShares){
        if(ps.receiver==receiver && ps.canonicalSource==canonicalSource){
            return true;
        }
    }
    return false;
}

void MyTcpServer::consumePendingShare(const QString& receiver, const QString& canonicalSource)
{
    for(int i=0;i<m_pendingShares.size();++i){
        if(m_pendingShares[i].receiver==receiver && m_pendingShares[i].canonicalSource==canonicalSource){
            m_pendingShares.removeAt(i);
            return;
        }
    }
}

// ===== REQ-EXT-SESSION-001：登录版本登记表与踢旧（D3/D5） =====
// 单调 CAS：只接受不低于现值的版本——并发双登录时后提交的"低版本"登记不得
// 覆盖高版本，否则新会话被误踢（E3 并发对打覆盖此路径）
void MyTcpServer::setLoginVersion(const QString& user, quint64 ver)
{
    QMutexLocker locker(&m_registryMutex);
    auto it = m_loginVersionRegistry.find(user);
    if(it == m_loginVersionRegistry.end() || ver >= it.value()){
        m_loginVersionRegistry.insert(user, ver);
    }
}

quint64 MyTcpServer::currentLoginVersion(const QString& user)
{
    QMutexLocker locker(&m_registryMutex);
    return m_loginVersionRegistry.value(user, 0);   // 无登记=0（登录版本恒≥1，视为过期）
}

void MyTcpServer::removeLoginVersion(const QString& user)
{
    QMutexLocker locker(&m_registryMutex);
    m_loginVersionRegistry.remove(user);
}

void MyTcpServer::closeStaleSockets(const QString& user)
{
    const quint64 cur = currentLoginVersion(user);   // 先读单调登记现值（registry 锁先于 socketlist 锁）
    QList<MyTcpSocket*> stale;
    {
        QMutexLocker locker(&m_tcpsocketlistMutex);
        for (MyTcpSocket* s : tcpsocketlist) {
            if (s->m_actorAuthenticated && s->m_strLoginName == user
                    && s->m_loginVersion != cur) {
                stale.append(s);
            }
        }
    }
    // 出锁再 abort：abort→disconnected→clientOffLine→removeSocket 会重入同一把锁（同僵尸扫描注释）
    for (MyTcpSocket* s : stale) {
        qCWarning(lcAuth) << "[KICK] 登录版本过期，关闭旧连接 conn=" << s->m_connectionId
                          << " user=" << user << " ver=" << s->m_loginVersion << " latest=" << cur;
        s->abort();
    }
}

//僵尸连接扫描：静默超过 m_zombieTimeoutMs 的连接直接 abort，触发既有下线清理
void MyTcpServer::onSweepTimeout(){
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<MyTcpSocket*> stale;
    {
        QMutexLocker locker(&m_tcpsocketlistMutex);
        for (MyTcpSocket* s : tcpsocketlist) {
            if (now - s->m_lastActiveMs > m_zombieTimeoutMs) {
                stale.append(s);
            }
        }
    }
    // 退出锁后再 abort：避免 abort()→disconnected→clientOffLine→removeSocket 重入同一把锁导致死锁
    for (MyTcpSocket* s : stale) {
        qCDebug(lcNet) << "僵尸连接清理:" << s->m_strLoginName
                 << "静默" << (now - s->m_lastActiveMs) << "ms";
        s->abort();   // 立即关闭 → 触发 disconnected → clientOffLine() 走既有清理
    }
}
