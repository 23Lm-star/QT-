// Client 主窗口实现：连接管理、登录/注册、验证码冷却、心跳与失联重连、
// 以及接收流的 PDU 拆包与按类型分发。
// 所有代码运行在 GUI 线程；PDU 所指向的接收缓冲只在当前槽内有效。
#include "logging.h"
#include "client.h"
#include "index.h"
#include "ui_client.h"
#include "process.h"
#include "file.h"
#include "transfertask.h"
#include <string.h>
#include <QByteArray>
#include <QPixmap>
#include <QBoxLayout>
#include <QFile>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QString>
#include <QDebug>
#include <QMessageBox>
#include <QHostAddress>
#include <QGraphicsOpacityEffect>
#include <QTimer>
#include <QCoreApplication>
#include <QTextStream>
#include <QDateTime>
#include <QInputDialog>
#include <QSettings>
#include <QScreen>
#include <QUuid>
#include <string>

namespace {
// 复制到定长字段并保证以 NUL 结尾：最多拷贝 dstSize-1 字节。
void copyFixedField(char *dst, int dstSize, const QByteArray &src)
{
    memset(dst, 0, dstSize);
    memcpy(dst, src.constData(), qMin(src.size(), dstSize - 1));
}

// 把剩余秒数格式化成 MM:SS，用于获取验证码按钮的倒计时显示
QString formatCooldown(int sec)
{
    sec = qMax(0, sec);
    int m = sec / 60;
    int s = sec % 60;
    return QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
}
}

// 构造：建控件、读配置、恢复/生成客户端 ID、启动验证码与心跳定时器，
// 最后发起 TCP 连接并挂接 connected/readyRead/disconnected 三个槽。
Client::Client(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Client)
{
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    ui->setupUi(this);
    // 基准 600×420 放大 2 倍（最多收敛到屏幕可用区 95%）。
    setFixedSize(scaledWindowSize(600,420));
    this->m_mhptr=new MsgHandler;
    loadconfig();
    // REQ-EXT-SESSION-001：持久化客户端唯一 ID（首启生成 32hex 落盘，之后复用；登录负载附加区携带）
    // REQ-AUTH-CONNECTION-001：读回后严格校验格式，历史大写 UUID/损坏值重新生成（旧值不是合法 clientId）
    {
        QSettings st;
        m_clientId = st.value("client/unique_id").toString();
        if(!isValidClientId(m_clientId.toLatin1())){
            if(!m_clientId.isEmpty())
                qCWarning(lcCli) << "[session] 落盘 clientId 格式非法，重新生成（旧值长度=" << m_clientId.size() << "）";
            m_clientId = QUuid::createUuid().toString(QUuid::Id128);   // Qt5.14 Id128 输出即 32 位小写 hex
            st.setValue("client/unique_id", m_clientId);
        }
    }
    // REQ-AUTH-CONNECTION-001：读回上次会话的恢复凭据（重连后优先 121 恢复）
    loadResumeToken();
    ui->loadingLabel->hide();
    // 验证码冷却：初值 0/未锁定，定时器由 startCaptchaCooldown 按需启动。
    m_captchaCooldownSec=0;
    m_captchaLocked=false;
    m_captchaCooldownTimer=new QTimer(this);
    connect(m_captchaCooldownTimer,&QTimer::timeout,this,&Client::onCaptchaCooldownTick);
    
    // 心跳固定 30s 一次；parent=this 保证随窗口销毁。
    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &Client::sendHeartbeat);
    m_heartbeatTimer->start(30000);

    // 服务端存活检测初始化
    m_lastPongMs = QDateTime::currentMSecsSinceEpoch();
    m_serverLostShown = false;
    m_loggedIn = false;
    m_reconnectDlg = new ReconnectDialog(this);
    connect(m_reconnectDlg, &ReconnectDialog::reconnectClicked,
            this, &Client::onReconnectRequested);

    // 异步连接（立即返回）；成功后进入 connectsort。REQ-EXT-TLS-001：连接发起收敛到 startConnection。
    connect(&sorket,&QTcpSocket::connected,this,&Client::connectsort);
    connect(&sorket,&QTcpSocket::readyRead,this,&Client::recvmsg);
    connect(&sorket,&QTcpSocket::disconnected,this,&Client::onSocketDisconnected);
    connect(&sorket,&QSslSocket::encrypted,this,&Client::connectsort);
    connect(&sorket,QOverload<const QList<QSslError>&>::of(&QSslSocket::sslErrors),
            this,[this](const QList<QSslError> &errors){
        // 业务规则 2：禁止 ignoreSslErrors 放行——证书不符即断连并提示（pinning 语义）。
        qCCritical(lcCli) << "[TLS] 证书校验失败:" << errors.value(0).errorString();
        m_tlsMode = false;   // 防止断连前再走 connectsort 发报文
        m_disconnectCause = tls::DisconnectCause::TlsRejected;   // D-1：断连原因先落定
        sorket.abort();
        QMessageBox::critical(this, QStringLiteral("安全连接失败"),
            QString::fromUtf8("服务器证书校验未通过，已拒绝连接。\n%1")
                .arg(errors.value(0).errorString()));
    });
    startConnection();
}

Client::~Client(){
    TransferManager::instance().saveSnapshotNow();   // 退出前把未完成传输落盘，下次登录恢复
    delete ui;
}

void Client::loadconfig()
{
    // 配置内嵌在 Qt 资源中，格式固定为 3 行 CRLF：IP / 端口 / 存储根。
    QFile f(":/connect.config");
    if(f.open(QIODevice::ReadOnly)){
        QString s=QString(f.readAll());
        QStringList str=s.split("\r\n");
        m_strlp=str[0];
        m_usPort=str[1].toUShort();
        m_strRootPath=str[2];
        f.close();
    }
    else {
        QMessageBox::critical(this,"提示","打开文件失败");
    }
}

Client &Client::getinstance(){
    // C++11 函数内静态变量：首次调用构造（可能失败/阻塞于连接），进程退出时析构。
    static Client instance;
    return instance;
}

// REQ-UX-001 A5：基准尺寸 ×2，并收敛到主屏可用区域的 95%，避免大窗口超出显示器
QSize Client::scaledWindowSize(int baseW, int baseH)
{
    QSize s(baseW * 2, baseH * 2);
    if(QScreen *scr = QApplication::primaryScreen()){
        const QRect avail = scr->availableGeometry();
        s.setWidth(qMin(s.width(), int(avail.width() * 0.95)));
        s.setHeight(qMin(s.height(), int(avail.height() * 0.95)));
    }
    return s;
}

void Client::flus()
{
    // 只清账号/密码；验证码输入框保留占位提示。
    ui->name_LE->clear();
    ui->pwd_LE->clear();
}

bool Client::s_heartbeatEnabled = true;  // 编译期默认开关（想永久关改成 false 再构建）
const qint64 Client::SERVER_ALIVE_TIMEOUT_MS = 90000;  // 与服务端 ZOMBIE 阈值一致

// 调试：心跳模拟假死开关（低耦合，仅作用于 sendHeartbeat，不触碰协议/UI/连接逻辑）
// 运行时切换：在客户端 exe 同目录放置 zombie_sim.txt，内容为 1=暂停发PING(模拟僵尸)，
//             0 或文件不存在=正常发PING。每次心跳都重读文件，因此把 1 改回 0 即可动态恢复。
void Client::sendHeartbeat() {
    // 调试开关：zombie_sim.txt 内容 1=停发(模拟僵尸)，0/不存在=发
    const QString flagFile = QCoreApplication::applicationDirPath() + "/zombie_sim.txt";
    QString rawVal;          // 文件原始内容，用于日志展示
    bool zombie = false;
    QFile f(flagFile);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        rawVal = f.readAll().trimmed();
        f.close();
        zombie = (rawVal == "1");
    } else {
        rawVal = "<文件不存在，默认发PING>";
    }
    // 决策：编译开关关闭 或 文件内容=1 → 不发
    const bool willSend = s_heartbeatEnabled && !zombie;
    qCDebug(lcCli) << "[HEARTBEAT] 读取" << flagFile << "值=" << rawVal
             << "| 编译开关=" << s_heartbeatEnabled
             << "| 决策=" << (willSend ? "发PING" : "不发PING");
    if (!willSend) {
        qCDebug(lcCli) << "[HEARTBEAT] PING 跳过（模拟假死）";
        return;
    }
    // 复用心跳检测服务端存活：距上次收到 PONG 已超过阈值，判定服务端失联
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastPongMs > SERVER_ALIVE_TIMEOUT_MS && !m_serverLostShown) {
        onServerLost();
    }

    PDU* pdu = mkPDU();
    pdu->uiType = ENUM_MSG_TYPE_HEARTBEAT_REQUEST;
    // REQ-AUTH-CONNECTION-001：caData[0,32)=clientId，服务端校验会话归属并续期 1800s/90s
    authFillClientIdField(pdu->caData, m_clientId.toLatin1().constData());
    qCDebug(lcCli) << "[HEARTBEAT] send PING";
    seedMsg(pdu);
}

void Client::handlMsg(PDU *&pdu)
{
    // 把当前 PDU 交给 MsgHandler；它只借用指针，不负责释放。
    m_mhptr->pdu=pdu;
    // 纯分发，无默认提示：未知类型静默忽略（向前兼容新消息）。
    switch(pdu->uiType){
    case ENUM_MSG_TYPE_HEARTBEAT_RESPOND: {
        m_lastPongMs = QDateTime::currentMSecsSinceEpoch();
        // REQ-AUTH-CONNECTION-001：51 caData[0,4)=标准码（旧服务端零区=0，PONG 语义不变）。
        // 会话已失效(-3017)：清凭据回登录页（服务端随后切断）；其余失败只记日志。
        const int hbCode = authReadI32(pdu->caData);
        if (hbCode != ENUM_PROTOCOL_CODE_OK) {
            qCWarning(lcCli) << "[HEARTBEAT] PONG 带失败码" << hbCode;
            if (hbCode == ENUM_PROTOCOL_CODE_SESSION_INVALID && m_loggedIn) {
                clearResumeToken();
                m_loggedIn = false;
                resetGroupStates();
                TransferManager::instance().saveSnapshotNow();
                TransferManager::instance().onConnectionLost(QStringLiteral("会话已失效，重新登录后可重试"));
                Index::getinstance().hide();
                this->show();
                flus();
                QMessageBox::information(this, "会话已失效",
                    "登录会话已过期或被撤销，请重新登录。");
            }
        } else {
            qCDebug(lcCli) << "[HEARTBEAT] recv PONG";
        }
        break;
    }
    case ENUM_MSG_TYPE_REGIST_RESPOND:{
        m_mhptr->handlrigist();
        break;
    }
    case ENUM_MSG_TYPE_LOGIN_RESPOND:{
        // R-1（REQ-EXT-TRANSFER-003）：登录名在同步路径就位——handllogin() 直接返回名字。
        const QString loginName = m_mhptr->handllogin();
        if (!loginName.isEmpty()) {
            // REQ-AUTH-CONNECTION-001：成功响应 caMsg=一次性恢复凭据（64hex+NUL）；
            // 旧服务端零正文 → 无凭据，清除本地残留（旧 token 已被新登录作废）。
            QString token;
            if (pdu->uiMsgLen > 0) {
                const QByteArray raw(pdu->caMsg, int(qMin(pdu->uiMsgLen, uint(AUTH_RESUME_TOKEN_HEX_LEN))));
                if (isValidResumeToken(raw)) token = QString::fromLatin1(raw);
            }
            if (!token.isEmpty()) saveResumeToken(loginName, token);
            else clearResumeToken();
            // 会话建立统一入口（与 122 恢复成功共用）：切主界面、恢复传输、声明能力、拉申请
            enterLoggedInSession(loginName);
        }
        break;
    }
    case ENUM_MSG_TYPE_SESSION_LOGOUT_RESPOND:{
        // REQ-AUTH-CONNECTION-001：119 退出结果。成功才清凭据回登录页（socket 保持匿名，不断开）；
        // 失败（如 Redis 不可用 -1003）保留当前登录态并提示，用户可重试。
        const int loCode = authReadI32(pdu->caData + SESSION_RESP_CODE_OFFSET);
        if (loCode == ENUM_PROTOCOL_CODE_OK) {
            clearResumeToken();
            m_loggedIn = false;
            resetGroupStates();
            TransferManager::instance().saveSnapshotNow();
            TransferManager::instance().onConnectionLost(QStringLiteral("已退出登录"));
            Index::getinstance().hide();
            this->show();
            this->raise();
            this->activateWindow();
            flus();
            QMessageBox::information(this, "提示", "已退出登录。");
        } else {
            QMessageBox::information(this, "提示",
                QString("退出登录失败：%1").arg(protocolCodeText(loCode)));
        }
        break;
    }
    case ENUM_MSG_TYPE_SESSION_RESUME_RESPOND:{
        // REQ-AUTH-CONNECTION-001：121 恢复结果。成功恢复主界面与任务（沿用原 token）；
        // 任何失败清 token 回登录页，不留半登录状态。
        m_resumePending = false;
        const int rsCode = authReadI32(pdu->caData + SESSION_RESP_CODE_OFFSET);
        if (rsCode == ENUM_PROTOCOL_CODE_OK) {
            char uname[33] = {'\0'};
            memcpy(uname, pdu->caData + SESSION_RESP_USER_OFFSET, 32);
            const QString user = QString::fromUtf8(uname);
            qCDebug(lcCli) << "[session] 会话恢复成功 ver="
                           << authReadI64(pdu->caData + SESSION_RESP_VERSION_OFFSET);
            enterLoggedInSession(user);
        } else {
            qCWarning(lcCli) << "[session] 会话恢复失败 code=" << rsCode;
            clearResumeToken();
            forceReLogin();
        }
        break;
    }
    case ENUM_MSG_TYPE_SESSION_INVALIDATED_EVENT:{
        handleSessionInvalidated(pdu);
        break;
    }
    case ENUM_MSG_TYPE_findUser_RESPOND:{
        m_mhptr->handlfinduser();
        break;
    }
    case ENUM_MSG_TYPE_ONLINEUSER_RESPOND:{
        m_mhptr->handlonlineuser();
        break;
    }
    case ENUM_MSG_TYPE_ADDFRIEND_RESPOND:{
        m_mhptr->handlAddFriend();
        break;
    }
    case ENUM_MSG_TYPE_ADDFRIEND_REQUEST:{
        m_mhptr->handlAddFriendAgree(m_LoginName);
        break;
    }
    case ENUM_MSG_TYPE_FLUSHFRIEND_RESPOND:{
        m_mhptr->handlFlushFriend();
        break;
    }
    case ENUM_MSG_TYPE_DELETEFRIEND_RESPOND:{
        m_mhptr->handlDeleteFriend();
        break;
    }
    case ENUM_MSG_TYPE_CHAT_REQUEST:{
        m_mhptr->chat();
        break;
    }
    case ENUM_MSG_TYPE_GETCHATMSG_RESPOND:{
        m_mhptr->getcharmsg();
        break;
    }
    // REQ-CLI-MSGSTORE-001：16 私聊入库应答，仅决定自发消息是否写进本地存储
    case ENUM_MSG_TYPE_CHAT_RESPOND:{
        m_mhptr->handleChatAck();
        break;
    }
    // ===== REQ-CHAT-ACK-001：桌面端私聊接收确认 =====
    case ENUM_MSG_TYPE_CHAT_DELIVER_EVENT:{
        m_mhptr->chatDeliver();
        break;
    }
    case ENUM_MSG_TYPE_CHAT_PULL_PENDING_RESPOND:{
        m_mhptr->getPendingChat();
        break;
    }
    // REQ-PAGE-FETCH-001：118 聊天历史分页响应（桌面端）
    case ENUM_MSG_TYPE_CHAT_HISTORY_RESPOND:{
        m_mhptr->chatHistoryResp();
        break;
    }
    case ENUM_MSG_TYPE_LIST_PENDING_FRIEND_RESPOND:{
        m_mhptr->handlePendingFriendList();
        break;
    }
    case ENUM_MSG_TYPE_MKDIR_RESPOND:{
        m_mhptr->mkdir();
        break;
    }
    case ENUM_MSG_TYPE_CHANGEDIR_RESPOND:{
        m_mhptr->changedir();
        break;
    }
    case ENUM_MSG_TYPE_DELFILE_RESPOND:{
        m_mhptr->deldir();
        break;
    }
    case ENUM_MSG_TYPE_RENAMEFILE_RESPOND:{
        m_mhptr->renamedir();
        break;
    }
    case ENUM_MSG_TYPE_FLUSHFILE_RESPOND:{
        m_mhptr->flushfile();
        break;
    }
    case ENUM_MSG_TYPE_SHAREFILE_RESPOND:{
        QMessageBox::information(&Index::getinstance(),"提示","文件分享完成");
        break;
    }
    case ENUM_MSG_TYPE_SHAREFILE_AGREE_REQUEST:{
        m_mhptr->sharefileagree();
        break;
    }
    case ENUM_MSG_TYPE_SHAREFILE_FINISH_RESPOND:{
        m_mhptr->sharefilefinsh();
        break;
    }
    // REQ-EXT-TRANSFER-001：32 确认按 transfer_id 路由到该上传任务
    case ENUM_MSG_TYPE_UPLOADFILE_DATA_RESPOND:{
        TransferManager::instance().onUploadDataRespond(pdu);
        break;
    }
    // REQ-EXT-TRANSFER-001：30 初始化响应按 id 路由（个人上传握手 = 97 → 29 → 30）
    case ENUM_MSG_TYPE_UPLOADFILE_INIT_RESPOND:{
        TransferManager::instance().onUploadInitRespond(pdu);
        break;
    }
    // REQ-EXT-TRANSFER-001：98 秒传预检查响应（无 transfer_id，按连接级单飞槽归属）
    case ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_RESPOND:{
        TransferManager::instance().onFastcheckRespond(pdu);
        break;
    }
    // REQ-EXT-TRANSFER-001：107/108 上传任务释放响应
    case ENUM_MSG_TYPE_UPLOADFILE_ABORT_RESPOND:{
        TransferManager::instance().onAbortRespond(pdu);
        break;
    }
    // REQ-EXT-TRANSFER-001：41/42/43 按 id 路由到该下载任务的接收槽（§5.4）
    case ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND:{
        TransferManager::instance().onDownloadRespond(pdu);
        break;
    }
    case ENUM_MSG_TYPE_DOWNLOADFILE_DATA:{
        TransferManager::instance().onDownloadData(pdu);
        break;
    }
    case ENUM_MSG_TYPE_DOWNLOADFILE_FINISH:{
        TransferManager::instance().onDownloadFinish(pdu);
        break;
    }
    case ENUM_MSG_TYPE_CAPTCHA_RESPOND:{
        m_mhptr->handleCaptcha();
        break;
    }
    case ENUM_MSG_TYPE_MOVEFILE_RESPOND:{
        m_mhptr->handlMoveFile();
        break;
    }
    case ENUM_MSG_TYPE_LISTDIR_RESPOND:{
        Index::getinstance().getFile()->handleListDirRespond(pdu);
        break;
    }
    case ENUM_MSG_TYPE_LOGOFF_RESPOND:{
        m_mhptr->handlLogoff();
        break;
    }
    case ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_RESPOND:
        m_mhptr->handleQueryFilePublishLink(pdu);
        break;
    case ENUM_MSG_TYPE_CREATE_PUBLINK_RESPOND:
        m_mhptr->handleCreatePublishLink(pdu);
        break;
    case ENUM_MSG_TYPE_RESOLVE_PUBLINK_RESPOND:
        m_mhptr->handleResolvePublishLink(pdu);
        break;
    case ENUM_MSG_TYPE_CREATE_GROUP_RESPOND:
    case ENUM_MSG_TYPE_INVITE_GROUP_MEMBER_RESPOND:
    case ENUM_MSG_TYPE_ACCEPT_GROUP_INVITATION_RESPOND:
    case ENUM_MSG_TYPE_REJECT_GROUP_INVITATION_RESPOND:
    case ENUM_MSG_TYPE_LEAVE_GROUP_RESPOND:
    case ENUM_MSG_TYPE_REMOVE_GROUP_MEMBER_RESPOND:
    case ENUM_MSG_TYPE_SET_GROUP_ROLE_RESPOND:
    case ENUM_MSG_TYPE_TRANSFER_GROUP_OWNER_RESPOND:
    case ENUM_MSG_TYPE_DISSOLVE_GROUP_RESPOND:
    case ENUM_MSG_TYPE_LIST_GROUPS_RESPOND:
    case ENUM_MSG_TYPE_LIST_GROUP_MEMBERS_RESPOND:
    case ENUM_MSG_TYPE_QUERY_GROUP_INVITATIONS_RESPOND:
    case ENUM_MSG_TYPE_QUERY_GROUP_SUMMARY_RESPOND:
    case ENUM_MSG_TYPE_GROUP_MEMBER_EVENT:
    case ENUM_MSG_TYPE_GROUP_INVITATION_EVENT:
    case ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_RESPOND:
        Index::getinstance().getGroup()->handleResponse(pdu);
        break;
    // REQ-EXT-GROUP-002：消息类响应按信封 group_id 路由到对应群窗口
    case ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_RESPOND:
    case ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_RESPOND:
    case ENUM_MSG_TYPE_GROUP_READ_CONFIRM_RESPOND:
        Index::getinstance().getGroup()->handleMessageResponse(pdu);
        break;
    case ENUM_MSG_TYPE_GROUP_MESSAGE_EVENT:
        Index::getinstance().getGroup()->handleMessageEvent(pdu);
        break;
    // REQ-EXT-GROUP-003：群文件响应。100/104 按 group_id 路由到群文件窗口。
    case ENUM_MSG_TYPE_GROUP_FILE_LIST_RESPOND:
    case ENUM_MSG_TYPE_GROUP_FILE_DELETE_RESPOND:
        Index::getinstance().getGroup()->handleFileResponse(pdu);
        break;
    // REQ-EXT-TRANSFER-001：102/106 按 transfer_id 路由到群上传/群下载任务
    case ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_RESPOND:
        TransferManager::instance().onGroupUploadInitRespond(pdu);
        break;
    case ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_RESPOND:
        TransferManager::instance().onGroupDownloadRespond(pdu);
        break;
    default:
        break;
    }
}

void Client::seedMsg(PDU *pdu){
    if (tlsPending()) {                                 // REQ-EXT-TLS-001 业务规则 5（同一谓词）
        free(pdu);
        return;
    }
    // QTcpSocket::write 会立即拷贝数据到内部写缓冲，因此可以马上 free。
    logPduFrame("C->S", pdu);      // REQ-AUDIT-FULL-001：发包日志
    sorket.write((char*)pdu,pdu->uiTotalLen);
    free(pdu);
    // 仅清空局部副本，调用方的指针不受影响
    pdu=nullptr;
}

// REQ-EXT-TLS-001：certs/server.crt 在位则走 TLS（端口=配置端口+1，pinning 该证书，
// 校验名 tlsserver 与证书 CN 一致）；不在位则保持明文原语义。
void Client::startConnection()
{
    m_tlsMode = false;
    m_disconnectCause = tls::DisconnectCause::Other;   // D-1：新一轮连接重新认定原因
    quint16 tlsPort = 0;
    if (!tls::nextTlsPort(m_usPort, &tlsPort)) {
        qCCritical(lcCli) << "[TLS] 配置端口" << m_usPort << "无法派生 TLS 端口（+1 越界或为 0），回退明文";
        sorket.connectToHost(QHostAddress(this->m_strlp), this->m_usPort);
        return;
    }
    const QSslCertificate crt = tls::loadPinnedCert();   // D4：certs/server.crt 固定目录
    if (!crt.isNull()) {
        sorket.setSslConfiguration(tls::buildPinnedConfig(crt));   // pinning + TLS1.2
        // Qt 5.14 的校验名是 QSslSocket 属性（不在 QSslConfiguration 上）
        sorket.setPeerVerifyName(tls::kVerifyName);                // 与证书 CN/SAN 一致
        m_tlsMode = true;
        qCInfo(lcCli) << "[TLS] 以 TLS 连接" << m_strlp << ":" << tlsPort;
        sorket.connectToHostEncrypted(m_strlp, tlsPort);
        return;
    }
    if (QFile::exists(tls::certDirPath() + "/server.crt")) {
        qCWarning(lcCli) << "[TLS] server.crt 无法解析，回退明文连接";
    }
    sorket.connectToHost(QHostAddress(this->m_strlp), this->m_usPort);
}

void Client::connectsort(){
    // TLS 连接：TCP 刚连通时只开始握手，业务动作等 encrypted 二次进入本槽。
    if (m_tlsMode && !sorket.isEncrypted()) {
        qCDebug(lcCli) << "[TLS] TCP 已连通，等待握手完成";
        return;
    }
    qCDebug(lcCli)<<"服务器连接成功"<<endl;
    // 区分"首次连接"与"断线后重连"：重连弹窗可见过说明这是一次重连
    bool wasReconnect = (m_reconnectDlg && m_reconnectDlg->isVisible());
    if (wasReconnect) {
        qCDebug(lcCli) << "[HEARTBEAT] 重连成功，关闭重连窗口";
        m_reconnectDlg->accept();   // 触发 closeEvent → 停止 spinner
        m_serverLostShown = false;
        m_lastPongMs = QDateTime::currentMSecsSinceEpoch();
        // REQ-AUTH-CONNECTION-001：优先 121 恢复逻辑会话；无凭据/恢复失败由响应路径回登录页
        tryResumeSession();
    }
}

// 服务端重启/连接重建后，本连接的服务端身份已丢失，必须重新登录。
// 这里直接回到登录界面，要求用户重新输入账号密码；服务端会为新连接重建
// m_strLoginName，从而修复"重连后 listdir/movefile 退化到 ./filesys 根目录"
// 以及"未重新鉴权就继续收发文件/聊天"的隐患。
void Client::forceReLogin() {
    m_loggedIn = false;
    // REQ-EXT-GROUP-004：旧连接的服务端身份已丢失，群读状态随之作废；重新登录后由新会话重建
    resetGroupStates();
    // 回到登录界面（Client 在登录成功后曾被 hide，这里重新显示）
    Index::getinstance().hide();
    this->show();
    this->raise();
    this->activateWindow();
    flus();                         // 清空账号/密码输入框
    QMessageBox::information(this, "提示",
        "与服务器的连接已恢复。\n由于服务端已重启，请重新登录。");
}

// ===== REQ-AUTH-CONNECTION-001：恢复凭据持久化与会话生命周期 =====
// 凭据只存 QSettings（用户级配置），不进日志、不进弹窗文案；覆盖前显式清旧键。
void Client::saveResumeToken(const QString& user, const QString& token)
{
    if (user.isEmpty() || !isValidResumeToken(token.toLatin1())) {
        qCWarning(lcCli) << "[session] 拒绝保存非法恢复凭据（长度=" << token.size() << "）";
        return;
    }
    QSettings st;
    st.remove(QStringLiteral("session/resume_user"));
    st.remove(QStringLiteral("session/resume_token"));
    st.setValue(QStringLiteral("session/resume_user"), user);
    st.setValue(QStringLiteral("session/resume_token"), token);
    m_resumeToken = token;
    m_tokenUser = user;
}

void Client::clearResumeToken()
{
    m_resumeToken.clear();
    m_tokenUser.clear();
    QSettings st;
    st.remove(QStringLiteral("session/resume_user"));
    st.remove(QStringLiteral("session/resume_token"));
}

void Client::loadResumeToken()
{
    QSettings st;
    const QString user = st.value(QStringLiteral("session/resume_user")).toString();
    const QString token = st.value(QStringLiteral("session/resume_token")).toString();
    if (user.isEmpty() && token.isEmpty()) return;
    if (user.isEmpty() || !isValidResumeToken(token.toLatin1())) {
        qCWarning(lcCli) << "[session] 落盘恢复凭据不完整/格式非法，丢弃";
        clearResumeToken();
        return;
    }
    m_resumeToken = token;
    m_tokenUser = user;
}

// 119 退出登录：请求服务端撤销会话；本地状态清理在 120 成功响应里完成（不用本地断线替代撤销）。
void Client::sendSessionLogout()
{
    if (!m_loggedIn) return;
    PDU* pdu = mkPDU();
    pdu->uiType = ENUM_MSG_TYPE_SESSION_LOGOUT_REQUEST;
    authFillClientIdField(pdu->caData, m_clientId.toLatin1().constData());
    seedMsg(pdu);
}

// 重连打开后优先尝试恢复：有合法凭据即发 121；否则回登录页重新鉴权。
void Client::tryResumeSession()
{
    if (m_resumeToken.isEmpty() || m_tokenUser.isEmpty()
            || !isValidResumeToken(m_resumeToken.toLatin1())) {
        clearResumeToken();
        forceReLogin();
        return;
    }
    PDU* pdu = mkPDU(uint(SESSION_RESUME_MSG_LEN));
    pdu->uiType = ENUM_MSG_TYPE_SESSION_RESUME_REQUEST;
    authFillClientIdField(pdu->caData, m_clientId.toLatin1().constData());
    authWriteResumeTokenMsg(pdu->caMsg, SESSION_RESUME_MSG_LEN, m_resumeToken.toLatin1().constData());
    m_resumePending = true;
    qCDebug(lcCli) << "[session] 重连后发送会话恢复请求（凭据不回显）";
    seedMsg(pdu);
}

// 123 失效事件：新登录互踢/注销/管理员撤销。停业务、清凭据、存快照、回登录页。
void Client::handleSessionInvalidated(PDU* pdu)
{
    const int reason = authReadI32(pdu->caData + SESSION_RESP_CODE_OFFSET);
    const qint64 staleVer = authReadI64(pdu->caData + SESSION_RESP_VERSION_OFFSET);
    QString reasonText;
    if (pdu->uiMsgLen > 0)
        reasonText = QString::fromUtf8(pdu->caMsg, int(pdu->uiMsgLen)).trimmed();
    qCWarning(lcCli) << "[session] 收到会话失效事件 reason=" << reason << " ver=" << staleVer;
    clearResumeToken();                     // 被踢/注销后旧凭据不可再用
    m_resumePending = false;
    m_loggedIn = false;
    resetGroupStates();
    TransferManager::instance().saveSnapshotNow();   // 在途任务落快照（重登后按键合并恢复）
    TransferManager::instance().onConnectionLost(QStringLiteral("会话已失效，重新登录后可重试"));
    Index::getinstance().hide();
    this->show();
    this->raise();
    this->activateWindow();
    flus();
    const QString headline = reasonText.isEmpty() ? protocolCodeText(reason) : reasonText;
    QMessageBox::information(this, "会话已失效",
        QString("%1\n请重新登录。").arg(headline));
    // 服务端随后按版本关闭连接；onSocketDisconnected 因 m_loggedIn=false 不再弹重连窗
}

// 会话建立统一入口：登录成功（4 响应）与恢复成功（122 响应）共用。
void Client::enterLoggedInSession(const QString& user)
{
    m_LoginName = user;
    TransferManager::instance().setCurrentUser(user);
    if (!m_msgStore.open(user))
        qCWarning(lcCli) << "[msgstore] 本地消息库打开失败，本轮消息仅内存展示:" << m_msgStore.lastError();
    TransferManager::instance().onConnectionRestored();
    m_loggedIn = true;
    resetGroupStates();
    Index::getinstance().getFile()->resetUserPath();
    Index::getinstance().show();
    this->hide();
    // 主界面构造完成后下一轮事件循环做初始刷新与能力声明
    QTimer::singleShot(0, [this](){
        Index::getinstance().getFriend()->flushfriend();
        Index::getinstance().getGroup()->initializeSession();
        PDU* cap = mkPDU();
        cap->uiType = ENUM_MSG_TYPE_CHAT_ACK_CAPABILITY;
        seedMsg(cap);
        if (Index::getinstance().getFriend())
            Index::getinstance().getFriend()->requestPendingFriendRequests();
    });
}

void Client::recvmsg(){
    if (tls::pduWriteBlocked(m_tlsMode, sorket.isEncrypted())) return;   // REQ-EXT-TLS-001 业务规则 5
    // 追加本次读到的字节；循环尝试切出完整 PDU。
    m_pdulist.append(sorket.readAll());
    // 重入闸：handlMsg 里的模态弹窗会开嵌套事件循环，期间新包触发 readyRead 会重入本函数。
    // 重入只追加字节立即返回，剩余字节由外层循环继续；否则嵌套 remove 会让外层指向
    // m_pdulist 的裸指针失效——0xC0000374 堆损坏（2026-11-14 GUI 三次崩溃，事件日志实锤）
    if (m_dispatchingPdu) {
        return;
    }
    m_dispatchingPdu = true;
    // 条件保证头部 76 字节齐全后才读取 uiTotalLen（避免半包越界）。
    while (m_pdulist.size()>=(int)sizeof(PDU)) {
        PDU*pdu=(PDU*)m_pdulist.data();
        uint pdutotallen=pdu->uiTotalLen;
        if(pdutotallen<(uint)sizeof(PDU)){
            // 畸形帧（totalLen<76）无法恢复帧边界，清空重同步，防 remove(0,0) 死循环
            qCWarning(lcCli)<<"[recvmsg] 畸形帧 uiTotalLen="<<pdutotallen<<"，清空缓冲重同步";
            m_pdulist.clear();
            break;
        }
        if((uint)m_pdulist.size()>=pdutotallen){
            // 先取整帧稳定副本并出队，再分发：处理期间的嵌套事件循环可能追加字节导致
            // 缓冲区重分配，任何指向 m_pdulist 内部的指针都不能跨 handlMsg 存活
            const QByteArray frame = m_pdulist.left(int(pdutotallen));
            m_pdulist.remove(0, int(pdutotallen));
            logPduFrame("C<-S", (const PDU*)frame.constData());  // REQ-AUDIT-FULL-001：收包日志
            PDU* framePdu = (PDU*)frame.constData();
            handlMsg(framePdu);
        }
        else break;
    }
    m_dispatchingPdu = false;
}

void Client::on_register_PB_clicked(){
    if (m_captchaLocked) {
        QMessageBox::information(this,"提示","账号已锁定，请等待倒计时结束后重试");
        return;
    }
    QString strname=ui->name_LE->text();
    QString strpwd=ui->pwd_LE->text();
    if(strname.isEmpty()||strpwd.isEmpty()||strname.toUtf8().size()>31){
        QMessageBox::information(this,"提示","用户名或密码长度非法");
        return ;
    }
    QByteArray captchaBytes=ui->randcode_LE->text().toUtf8();
    // caData: [0..31]=用户名，[32..63]=UTF-8 明文密码。
    // REQ-AUTH-CONNECTION-001：caMsg = 验证码 + NUL + 32B clientId（与登录同布局，服务端严格校验）
    const QByteArray clientIdBytes=m_clientId.toLatin1();
    PDU* pdu=mkPDU(uint(captchaBytes.size()+1+clientIdBytes.size()));
    pdu->uiType=ENUM_MSG_TYPE_REGIST_REQUEST;
    copyFixedField(pdu->caData,32,strname.toUtf8());
    copyFixedField(pdu->caData+32,32,strpwd.toUtf8());
    memcpy(pdu->caMsg,captchaBytes.constData(),captchaBytes.size()+1);
    memcpy(pdu->caMsg+captchaBytes.size()+1,clientIdBytes.constData(),clientIdBytes.size());
    seedMsg(pdu);
}

void Client::on_login_PB_clicked(){
    if (m_captchaLocked) {
        QMessageBox::information(this,"提示","账号已锁定，请等待倒计时结束后重试");
        return;
    }
    QString captchaInput=ui->randcode_LE->text();
    if(captchaInput.isEmpty()){
        QMessageBox::information(this,"提示","请输入验证码");
        return;
    }
    QString strname=ui->name_LE->text();
    QString strpwd=ui->pwd_LE->text();
    if(strname.isEmpty()||strpwd.isEmpty()||strname.toUtf8().size()>31){
        QMessageBox::information(this,"提示","用户名或密码长度非法");
        return;
    }
    QByteArray captchaBytes=captchaInput.toUtf8();
    // caData: [0..31]=用户名，[32..63]=UTF-8 明文密码。
    // REQ-EXT-SESSION-001 D1：验证码 NUL 之后追加 32 字节客户端 ID（零协议改动，老服务端按 uiMsgLen 忽略）
    QByteArray clientIdBytes=m_clientId.toUtf8();
    PDU* pdu=mkPDU(captchaBytes.size()+1+clientIdBytes.size());
    pdu->uiType=ENUM_MSG_TYPE_LOGIN_REQUEST;
    copyFixedField(pdu->caData,32,strname.toUtf8());
    copyFixedField(pdu->caData+32,32,strpwd.toUtf8());
    memcpy(pdu->caMsg,captchaBytes.constData(),captchaBytes.size()+1);
    memcpy(pdu->caMsg+captchaBytes.size()+1,clientIdBytes.constData(),clientIdBytes.size());
    seedMsg(pdu);
}

//请求服务端验证码
void Client::on_pushButton_clicked(){
    // 客户端拦截：冷却/锁定中直接拒绝发送"获取验证码"请求（按钮本就禁用，这里兜底）
    if (m_captchaLocked || m_captchaCooldownSec > 0) {
        return;
    }
    if(sorket.state()!=QAbstractSocket::ConnectedState){
        QMessageBox::information(this,"提示","未连接服务器，无法获取验证码");
        return;
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_CAPTCHA_REQUEST;
    // REQ-AUTH-CONNECTION-001：caData[0,32)=clientId，冷却/锁定状态跨连接按设备一致
    authFillClientIdField(pdu->caData, m_clientId.toLatin1().constData());
    seedMsg(pdu);
}

// REQ-AUDIT-001 R8：服务端只下发 PNG。图片放在"验证码"文字之后的独立标签里，
// 不改动原标签文字（用户要求：不要盖住"验证码"这三个字）。
void Client::updateCaptchaImage(const QByteArray& png){
    QPixmap pixmap;
    if(!pixmap.loadFromData(png, "PNG") || pixmap.isNull()){
        if(m_captchaImageLabel != nullptr){
            m_captchaImageLabel->clear();
        }
        ui->randcode->setToolTip("验证码图片解析失败，请重新获取");
        return;
    }
    if(m_captchaImageLabel == nullptr){
        m_captchaImageLabel = new QLabel(ui->captchaLayout->parentWidget());
        m_captchaImageLabel->setObjectName("captchaImageLabel");
        m_captchaImageLabel->setToolTip("请按图片输入验证码");
        m_captchaImageLabel->setAlignment(Qt::AlignCenter);
        // 图片插在 captchaLayout 中"验证码"标签之后（grid 里的第 2 行横向布局）
        QBoxLayout* lay = qobject_cast<QBoxLayout*>(ui->captchaLayout);
        if(lay != nullptr){
            const int idx = lay->indexOf(ui->randcode);
            lay->insertWidget(idx >= 0 ? idx + 1 : 0, m_captchaImageLabel);
        }
    }
    // 高度收敛到 36px：行内还要放输入框与按钮，避免把布局撑出窗口
    m_captchaImageLabel->setPixmap(pixmap.scaledToHeight(36, Qt::SmoothTransformation));
    m_captchaImageLabel->setVisible(true);
}

void Client::resetCaptchaButton(){
    ui->pushButton->setEnabled(true);
}

// 验证码冷却状态机入口：sec 来自服务端；锁定与普通冷却共用同一倒计时。
void Client::startCaptchaCooldown(int sec, bool locked){
    // 服务端未给出有效秒数时使用本地默认值。
    if (sec <= 0) sec = locked ? 600 : 60;
    m_captchaCooldownSec=sec;
    m_captchaLocked=locked;
    ui->pushButton->setEnabled(false);
    ui->pushButton->setText(formatCooldown(m_captchaCooldownSec));
    if (locked) {
        // 锁定期间：注册/登录按钮也禁用，点击将被客户端直接拦截
        ui->register_PB->setEnabled(false);
        ui->login_PB->setEnabled(false);
    }
    m_captchaCooldownTimer->start(1000);
}

void Client::onCaptchaCooldownTick(){
    // 每秒递减；到 0 停止定时器并恢复交互。
    m_captchaCooldownSec--;
    if(m_captchaCooldownSec<=0){
        m_captchaCooldownTimer->stop();
        resetCaptchaButton();
        ui->pushButton->setText("获取验证码");
        if (m_captchaLocked) {
            // 10 分钟锁定结束：恢复注册/登录，后续冷却回到 1 分钟（由服务端下发时长驱动）
            m_captchaLocked = false;
            ui->register_PB->setEnabled(true);
            ui->login_PB->setEnabled(true);
        }
        if (m_captchaAutoRefresh && !m_captchaLocked) {
            // 登录验证码失败后的自动补拉：冷却归零立即取新码，用户免手点
            m_captchaAutoRefresh = false;
            on_pushButton_clicked();
        }
        return;
    }
    ui->pushButton->setText(formatCooldown(m_captchaCooldownSec));
}

// 登录失败（验证码错/过期）后自动换新码：验证码一次性，失败后复用旧图必再失败
//（-3012，2026-11-14 GUI 实测）。立即请求一次；若服务端冷却中，-5 响应接管倒计时，
// 归零时 onCaptchaCooldownTick 依据 m_captchaAutoRefresh 自动补拉。
void Client::requestCaptchaAutoRefresh(){
    m_captchaAutoRefresh = true;
    on_pushButton_clicked();
}

//发送注销账户请求
void Client::sendLogoffRequest()
{
    //弹出密码确认框
    bool ok=false;
    QString pwdInput=QInputDialog::getText(this,"注销确认",
        "输入密码以确认注销账户：\n（此操作不可逆，将永久删除您的账户、好友关系和所有文件）",
        QLineEdit::Password,"",&ok);
    if(!ok||pwdInput.isEmpty()){
        return;
    }

    //二次确认
    int confirm=QMessageBox::warning(this,"最后确认",
        "确定要永久删除您的账户吗？\n此操作不可撤销！",
        QMessageBox::Yes|QMessageBox::No,QMessageBox::No);
    if(confirm!=QMessageBox::Yes){
        return;
    }

    // caData: [0..31]=当前登录名，[32..63]=UTF-8 明文密码；服务端会再次校验密码。
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_LOGOFF_REQUEST;
    copyFixedField(pdu->caData,32,m_LoginName.toUtf8());
    copyFixedField(pdu->caData+32,32,pwdInput.toUtf8());
    seedMsg(pdu);
}

void Client::showLoadingAnimation(bool success){}

// ===== 服务端存活检测 + 重连 =====
void Client::onServerLost() {
    // 去重：定时器与 disconnected 信号可能先后触发，只处理第一次。
    if (m_serverLostShown) return;
    // 直到重连成功（connectsort）才复位
    m_serverLostShown = true;
    // REQ-EXT-TRANSFER-003 G7/C8②：心跳丢失（服务端假死）同样触发在途任务收敛，任务是"失败可重试"
    TransferManager::instance().onConnectionLost(QStringLiteral("与服务端断开连接，可重试"));
    qCDebug(lcCli) << "[HEARTBEAT] 服务端失联，弹出重连窗口";
    m_reconnectDlg->show();
    m_reconnectDlg->raise();
    m_reconnectDlg->activateWindow();
}

void Client::onSocketDisconnected() {
    qCDebug(lcCli) << "[HEARTBEAT] socket disconnected 信号";
    // REQ-EXT-TRANSFER-003 G7/C8①：连接断开即在途任务收敛（服务端被杀/网络断），
    // 不再让任务永久停在"传输中"占着名额（本地分片与服务端暂存保留，重试即续传）
    TransferManager::instance().onConnectionLost(QStringLiteral("与服务端断开连接，可重试"));
    // REQ-AUTH-CONNECTION-001：121 已发出但连接被关闭（未等到 122，如服务端拒绝后切断）：
    // 复位等待标记并给出重连入口，重试仍走恢复优先；凭据留给 122 失败路径统一清除。
    if (m_resumePending) {
        m_resumePending = false;
        m_loggedIn = true;   // 借用既有重连窗路径（会话尚未确认失效，不清凭据）
    }
    // 已登录才弹窗（未登录在登录界面，连接失败不应弹重连窗口）
    if (m_loggedIn && !m_serverLostShown) {
        // D-1：证书被拒已有「安全连接失败」对话框，这里不再叠加互踢文案；重连窗照常给出
        if (tls::showKickNotice(m_loggedIn, m_disconnectCause)) {
            // REQ-EXT-SESSION-001：被服务端版本校验切断时的提示（零协议，仅文案）
            QMessageBox::information(this,"连接已关闭","连接被服务端关闭，账号可能已在其他设备登录。重新连接后请重新登录。");
        }
        onServerLost();
    }
}

void Client::onReconnectRequested() {
    qCDebug(lcCli) << "[HEARTBEAT] 用户点击重新连接，发起 connectToHost";
    if (sorket.state() == QAbstractSocket::ConnectedState) {
        sorket.disconnectFromHost();
    }
    m_lastPongMs = QDateTime::currentMSecsSinceEpoch(); // 重置，避免重连过程中立即再判失联
    startConnection();
}
