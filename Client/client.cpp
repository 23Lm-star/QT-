#include "client.h"
#include "ui_client.h"
#include "process.h"
#include "index.h"
#include "file.h"
#include <string.h>
#include <QByteArray>
#include <QFile>
#include <QString>
#include <QDebug>
#include <QMessageBox>
#include <QHostAddress>
#include <QCryptographicHash>
#include <QGraphicsOpacityEffect>
#include <QTimer>
#include <QCoreApplication>
#include <QTextStream>
#include <QDateTime>
#include <QInputDialog>
#include <string>

namespace {
QByteArray passwordHashForProtocol(const QString &password)
{
    return QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Md5).toHex();
}

void copyFixedField(char *dst, int dstSize, const QByteArray &src)
{
    memset(dst, 0, dstSize);
    memcpy(dst, src.constData(), qMin(src.size(), dstSize - 1));
}

void copyExactField(char *dst, int dstSize, const QByteArray &src)
{
    memset(dst, 0, dstSize);
    memcpy(dst, src.constData(), qMin(src.size(), dstSize));
}
}

Client::Client(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Client)
{
    ui->setupUi(this);
    setFixedSize(600,420);
    this->m_mhptr=new MsgHandler;
    loadconfig();
    ui->loadingLabel->hide();
    m_captchaCooldownSec=0;
    m_captchaCooldownTimer=new QTimer(this);
    connect(m_captchaCooldownTimer,&QTimer::timeout,this,&Client::onCaptchaCooldownTick);
    
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

    sorket.connectToHost(QHostAddress(this->m_strlp),this->m_usPort);
    connect(&sorket,&QTcpSocket::connected,this,&Client::connectsort);
    connect(&sorket,&QTcpSocket::readyRead,this,&Client::recvmsg);
    connect(&sorket,&QTcpSocket::disconnected,this,&Client::onSocketDisconnected);
}

Client::~Client(){
    delete ui;
}

void Client::loadconfig()
{
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
    static Client instance;
    return instance;
}

void Client::flus()
{
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
    qDebug() << "[HEARTBEAT] 读取" << flagFile << "值=" << rawVal
             << "| 编译开关=" << s_heartbeatEnabled
             << "| 决策=" << (willSend ? "发PING" : "不发PING");
    if (!willSend) {
        qDebug() << "[HEARTBEAT] PING 跳过（模拟假死）";
        return;
    }
    // 复用心跳检测服务端存活：距上次收到 PONG 已超过阈值，判定服务端失联
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastPongMs > SERVER_ALIVE_TIMEOUT_MS && !m_serverLostShown) {
        onServerLost();
    }

    PDU* pdu = mkPDU();
    pdu->uiType = ENUM_MSG_TYPE_HEARTBEAT_REQUEST;
    qDebug() << "[HEARTBEAT] send PING";
    seedMsg(pdu);
}

void Client::handlMsg(PDU *&pdu)
{
    m_mhptr->pdu=pdu;
    switch(pdu->uiType){
    case ENUM_MSG_TYPE_HEARTBEAT_RESPOND: {
        m_lastPongMs = QDateTime::currentMSecsSinceEpoch();
        qDebug() << "[HEARTBEAT] recv PONG";
        break;
    }
    case ENUM_MSG_TYPE_REGIST_RESPOND:{
        m_mhptr->handlrigist();
        break;
    }
    case ENUM_MSG_TYPE_LOGIN_RESPOND:{
        m_mhptr->handllogin();
        m_loggedIn = true;   // 登录过才监控断开，避免登录界面误报
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
    case ENUM_MSG_TYPE_UPLOADFILE_DATA_RESPOND:{
        m_mhptr->uploadfiledata();
        break;
    }
    case ENUM_MSG_TYPE_UPLOADFILE_INIT_RESPOND:{
        m_mhptr->uploadfileinit();
        break;
    }
    case ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND:{
        m_mhptr->downloadfile();
        break;
    }
    case ENUM_MSG_TYPE_DOWNLOADFILE_DATA:{
        m_mhptr->handleDownloadData();
        break;
    }
    case ENUM_MSG_TYPE_DOWNLOADFILE_FINISH:{
        m_mhptr->handleDownloadFinish();
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
    case ENUM_MSG_TYPE_GETUSERINFORMATION_RESPOND:{
        m_mhptr->handlUserInformation();
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
    default:
        break;
    }
}

void Client::seedMsg(PDU *pdu){
    sorket.write((char*)pdu,pdu->uiTotalLen);
    free(pdu);
    pdu=nullptr;
}

void Client::connectsort(){
    qDebug()<<"服务器连接成功"<<endl;
    if (m_reconnectDlg && m_reconnectDlg->isVisible()) {
        qDebug() << "[HEARTBEAT] 重连成功，关闭重连窗口";
        m_reconnectDlg->accept();   // 触发 closeEvent → 停止 spinner
        m_serverLostShown = false;
        m_lastPongMs = QDateTime::currentMSecsSinceEpoch();
    }
}

void Client::recvmsg(){
    m_pdulist.append(sorket.readAll());
    while (m_pdulist.size()>=(int)sizeof(PDU)) {
        PDU*pdu=(PDU*)m_pdulist.data();
        uint pdutotallen=pdu->uiTotalLen;
        if((uint)m_pdulist.size()>=pdutotallen){
            handlMsg(pdu);
            m_pdulist.remove(0,pdu->uiTotalLen);
        }
        else break;
    }
}

void Client::on_register_PB_clicked(){
    QString strname=ui->name_LE->text();
    QString strpwd=ui->pwd_LE->text();
    if(strname.isEmpty()||strpwd.isEmpty()||strname.toUtf8().size()>31){
        QMessageBox::information(this,"提示","用户名或密码长度非法");
        return ;
    }
    QByteArray captchaBytes=ui->randcode_LE->text().toUtf8();
    PDU* pdu=mkPDU(captchaBytes.size()+1);
    pdu->uiType=ENUM_MSG_TYPE_REGIST_REQUEST;
    QByteArray passwordHash=passwordHashForProtocol(strpwd);
    copyFixedField(pdu->caData,32,strname.toUtf8());
    copyExactField(pdu->caData+32,32,passwordHash);
    memcpy(pdu->caMsg,captchaBytes.constData(),captchaBytes.size()+1);
    seedMsg(pdu);
}

void Client::on_login_PB_clicked(){
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
    PDU* pdu=mkPDU(captchaBytes.size()+1);
    pdu->uiType=ENUM_MSG_TYPE_LOGIN_REQUEST;
    QByteArray passwordHash=passwordHashForProtocol(strpwd);
    copyFixedField(pdu->caData,32,strname.toUtf8());
    copyExactField(pdu->caData+32,32,passwordHash);
    memcpy(pdu->caMsg,captchaBytes.constData(),captchaBytes.size()+1);
    seedMsg(pdu);
}

//请求服务端验证码
void Client::on_pushButton_clicked(){
    if(sorket.state()!=QAbstractSocket::ConnectedState){
        QMessageBox::information(this,"提示","未连接服务器，无法获取验证码");
        return;
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_CAPTCHA_REQUEST;
    seedMsg(pdu);
}

void Client::updateCaptchaDisplay(const QString& code){
    ui->randcode_LE->setPlaceholderText(code);
}

void Client::resetCaptchaButton(){
    ui->pushButton->setEnabled(true);
}

void Client::startCaptchaCooldown(){
    m_captchaCooldownSec=60;
    ui->pushButton->setEnabled(false);
    ui->pushButton->setText(QString("%1s").arg(m_captchaCooldownSec));
    m_captchaCooldownTimer->start(1000);
}

void Client::onCaptchaCooldownTick(){
    m_captchaCooldownSec--;
    if(m_captchaCooldownSec<=0){
        m_captchaCooldownTimer->stop();
        resetCaptchaButton();
        ui->pushButton->setText("获取验证码");
        return;
    }
    ui->pushButton->setText(QString("%1s").arg(m_captchaCooldownSec));
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

    QByteArray passwordHash=passwordHashForProtocol(pwdInput);
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_LOGOFF_REQUEST;
    copyFixedField(pdu->caData,32,m_LoginName.toUtf8());
    copyExactField(pdu->caData+32,32,passwordHash);
    seedMsg(pdu);
}

void Client::showLoadingAnimation(bool success){}

// ===== 服务端存活检测 + 重连 =====
void Client::onServerLost() {
    if (m_serverLostShown) return;
    m_serverLostShown = true;
    qDebug() << "[HEARTBEAT] 服务端失联，弹出重连窗口";
    m_reconnectDlg->show();
    m_reconnectDlg->raise();
    m_reconnectDlg->activateWindow();
}

void Client::onSocketDisconnected() {
    qDebug() << "[HEARTBEAT] socket disconnected 信号";
    // 已登录才弹窗（未登录在登录界面，连接失败不应弹重连窗口）
    if (m_loggedIn && !m_serverLostShown) {
        onServerLost();
    }
}

void Client::onReconnectRequested() {
    qDebug() << "[HEARTBEAT] 用户点击重新连接，发起 connectToHost";
    if (sorket.state() == QAbstractSocket::ConnectedState) {
        sorket.disconnectFromHost();
    }
    m_lastPongMs = QDateTime::currentMSecsSinceEpoch(); // 重置，避免重连过程中立即再判失联
    sorket.connectToHost(QHostAddress(m_strlp), m_usPort);
}
