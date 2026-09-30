#include "logging.h"
#include "msghandler.h"
#include "QDebug"
#include "operatedb.h"
#include "mytcpserver.h"
#include "server.h"
#include "sharelinkservice.h"
#include "fileobjectstore.h"
#include "memalloc.h"
#include <QByteArray>

#include <QFile>
#include <QFileInfo>
#include <QTime>
#include <QVector>
#include <QSet>
#include <cerrno>
#include <cstring>

namespace {
QString fixedString(const char *data, int maxLen)
{
    QByteArray bytes(data, maxLen);
    const int end = bytes.indexOf('\0');
    if (end >= 0) {
        bytes.truncate(end);
    }
    return QString::fromUtf8(bytes);
}
// REQ-EXT-SEC-001：将服务端派生的身份写入 32 字节定长字段（覆盖客户端提交的"自己"字段）
void copySelfField(char *dst, const QString &name)
{
    memset(dst, 0, 32);
    const QByteArray bytes = name.toUtf8();
    memcpy(dst, bytes.constData(), qMin(bytes.size(), 31));
}
}

MsgHandler::MsgHandler(){
    pdu=nullptr;
    m_captchaFailCount = 0;
    static bool seeded=false;
    if(!seeded){
        qsrand(QTime::currentTime().msec());
        seeded=true;
    }
}

QString MsgHandler::generateCaptchaCode(int length)
{
    const QString chars = "RSTnoprsXYZabcdeJK0QtuvwxyzUVWq123456789LMNPfghijklABCDEFGHm";
    QString code;
    for (int i = 0; i < length; ++i) {
        code.append(chars.at(qrand() % chars.length()));
    }
    return code;
}

int MsgHandler::validateCaptcha(const char* caMsg, uint uiMsgLen, int& remainingSec)
{
    remainingSec = 0;
    QDateTime now = QDateTime::currentDateTime();

    // 若之前锁定且已过期，恢复正常（冷却回到 1 分钟逻辑）
    if (m_captchaLockUntil.isValid() && now >= m_captchaLockUntil) {
        m_captchaLockUntil = QDateTime();
        m_captchaFailCount = 0;
    }

    // 锁定中：直接拒绝并回传剩余秒数
    if (m_captchaLockUntil.isValid() && now < m_captchaLockUntil) {
        remainingSec = now.secsTo(m_captchaLockUntil);
        qCDebug(lcAuth) << "验证码校验失败: 账号已锁定，剩余" << remainingSec << "秒";
        return -4;
    }

    if (uiMsgLen < 4) {
        m_captchaFailCount++;
        return -2;
    }
    QString userInput = QString::fromUtf8(caMsg).trimmed();

    // 无有效码或已过期：不累计失败次数，避免误锁
    if (m_captchaCode.isEmpty() || now > m_captchaExpiry) {
        m_captchaCode.clear();
        qCDebug(lcAuth) << "验证码校验失败: 无有效码或已过期";
        return -3;
    }

    if (userInput != m_captchaCode) {
        m_captchaFailCount++;
        // 连续错误达上限 → 触发锁定，冷却升级为锁定时长，作废当前码
        if (m_captchaFailCount >= CAPTCHA_MAX_FAILS) {
            m_captchaLockUntil = now.addSecs(CAPTCHA_LOCKOUT_SEC);
            m_captchaNextAllowed = m_captchaLockUntil;
            m_captchaCode.clear();
            remainingSec = CAPTCHA_LOCKOUT_SEC;
            qCWarning(lcAuth) << "验证码连续错误" << m_captchaFailCount << "次，账号锁定" << CAPTCHA_LOCKOUT_SEC << "秒";
            return -4;
        }
        qCDebug(lcAuth) << "验证码校验失败: 输入不符";
        return -2;
    }

    // 校验成功：一次性使用，作废当前码并重置错误计数
    m_captchaCode.clear();
    m_captchaFailCount = 0;
    qCDebug(lcAuth) << "验证码校验成功";
    return 0;
}

//处理注销账户请求
// REQ-EXT-SEC-001：授权依据为连接级身份（m_actorName），PDU 用户字段只作请求数据；
// 删除目录同样以服务端派生的 actor 生成，密码仅作为对本人账号的二次确认。
PDU *MsgHandler::logoff(bool &revoked)
{
    revoked=false;
    const QString actor=m_actorName;
    if(actor.isEmpty()){
        qCWarning(lcAuth)<<"[logoff] 拒绝：连接无认证上下文 conn="<<m_connId;
        PDU* respdu=mkPDU();
        respdu->uiType=ENUM_MSG_TYPE_LOGOFF_RESPOND;
        bool ret=false;
        memcpy(respdu->caData,&ret,sizeof(ret));
        return respdu;
    }

    QString claimedName = fixedString(pdu->caData, 32);
    QString userPwd  = fixedString(pdu->caData + 32, 32);

    if(claimedName!=actor){
        qCWarning(lcAuth)<<"[logoff] PDU 用户名字段与连接身份不符，按连接身份受理 conn="<<m_connId;
    }
    qCDebug(lcAuth) << "logoff request for:" << actor;

    bool ret = OperateDB::instance().handleLogoff(
        actor.toUtf8().constData(),
        userPwd.toUtf8().constData()
    );

    // 如果数据库删除成功，清理用户的文件目录（目录名 = 服务端 actor，非客户端字段）
    if(ret){
        QString userDir=QString("%1/%2").arg(Server::getinstance().filesPath).arg(actor);
        QDir dir(userDir);
        if(dir.exists()){
            bool removed=dir.removeRecursively();
            qCDebug(lcAuth)<<"logoff: remove user dir"<<userDir<<"result:"<<removed;
        }
        revoked=true;
    }

    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_LOGOFF_RESPOND;
    memcpy(respdu->caData,&ret,sizeof(ret));
    return respdu;
}

// ===== REQ-EXT-SEC-001：actor 用户根与规范路径校验 =====
QString MsgHandler::actorUserRoot() const
{
    if(m_actorName.isEmpty()){
        return QString();
    }
    return QDir(Server::getinstance().filesPath).absoluteFilePath(m_actorName);
}

bool MsgHandler::pathInActorRoot(const QString &path) const
{
    const QString root=actorUserRoot();
    if(root.isEmpty()){
        return false;
    }
    return isPathInSandbox(path, root);
}

// REQ-EXT-FILE-001 D5：对象文件不落用户路径，改校验其所在目录在 actor 根内且存在
bool MsgHandler::parentDirInActorRoot(const QString &filePath) const
{
    const QString parent = QFileInfo(filePath).absolutePath();
    const QString root = actorUserRoot();
    if(root.isEmpty()){
        return false;
    }
    return isPathInSandbox(parent, root);
}

PDU *MsgHandler::captcha()
{
    QDateTime now = QDateTime::currentDateTime();
    int status = 0;
    int remaining = 0;

    // 锁定中：禁止获取新码
    if (m_captchaLockUntil.isValid() && now < m_captchaLockUntil) {
        status = -6;
        remaining = now.secsTo(m_captchaLockUntil);
    }
    // 正常冷却中：拒绝并回传剩余秒数
    else if (now < m_captchaNextAllowed) {
        status = -5;
        remaining = now.secsTo(m_captchaNextAllowed);
    }
    else {
        // 覆盖式单码：直接覆盖，旧码自然失效
        m_captchaCode = generateCaptchaCode(6);
        m_captchaExpiry = now.addSecs(CAPTCHA_EXPIRE_SEC);
        m_captchaNextAllowed = now.addSecs(CAPTCHA_COOLDOWN_SEC);
        remaining = CAPTCHA_COOLDOWN_SEC;   // 成功时回传正常冷却时长
        status = 0;
        qCDebug(lcAuth) << "生成验证码:" << m_captchaCode << "过期时间:" << m_captchaExpiry;
    }

    PDU* respdu;
    if (status == 0) {
        QByteArray codeBytes = m_captchaCode.toUtf8();
        respdu = mkPDU(codeBytes.size() + 1);
        memcpy(respdu->caMsg, codeBytes.constData(), codeBytes.size() + 1);
    } else {
        respdu = mkPDU();
    }
    respdu->uiType = ENUM_MSG_TYPE_CAPTCHA_RESPOND;
    memcpy(respdu->caData, &status, sizeof(status));
    memcpy(respdu->caData + 4, &remaining, sizeof(remaining));
    return respdu;
}

//处理注册请求
PDU *MsgHandler::rigist()
{
    int remaining = 0;
    int captchaRet = validateCaptcha(pdu->caMsg, pdu->uiMsgLen, remaining);
    if (captchaRet != 0) {
        PDU* respdu = mkPDU();
        respdu->uiType = ENUM_MSG_TYPE_REGIST_RESPOND;
        memcpy(respdu->caData, &captchaRet, sizeof(captchaRet));
        if (captchaRet == -4) {
            memcpy(respdu->caData + 4, &remaining, sizeof(remaining));
        }
        return respdu;
    }
    //取出用户名和密码
    QString strname=fixedString(pdu->caData,32);
    QString strpwd=fixedString(pdu->caData+32,32);
    //利用数据库操作对象进行数据库的注册操作，返回注册结果并输出
    bool ret=OperateDB::instance().handleRegit(strname.toUtf8().data(),strpwd.toUtf8().data());
    qCDebug(lcAuth)<<"regist ret "<<ret<<endl;
    if(ret){
        QDir dir;
        bool ret1=dir.mkdir(QString("%1").arg(Server::getinstance().filesPath));
        bool ret2=dir.mkdir(QString("%1/%2").arg(Server::getinstance().filesPath).arg(strname));
        qCDebug(lcAuth)<<"创建用户目录:"<<"ret1 "<<ret1<<" "<<QString("%1").arg(Server::getinstance().filesPath)<<endl;
        qCDebug(lcAuth)<<"创建用户目录:"<<"ret2 "<<ret2<<" "<<QString("%1/%2").arg(Server::getinstance().filesPath).arg(strname)<<endl;
    }
    //将注册结果打包进响应报文返回
    PDU* respdu=mkPDU();
    memcpy(respdu->caData,&ret,sizeof(ret));
    QByteArray strnameBytes=strname.toUtf8();
    memcpy(respdu->caData+32,strnameBytes.constData(),qMin(strnameBytes.size(),31));
    respdu->uiType=ENUM_MSG_TYPE_REGIST_RESPOND;
    return respdu;
}
//处理登录请求，同时需要更新socket对象的客户用户名
// REQ-EXT-SEC-001：authOk 仅在服务端凭据校验通过时为 true，由连接层据此绑定 actor_context
PDU *MsgHandler::login(QString &strname, bool &authOk)
{
    authOk = false;
    int remaining = 0;
    int captchaRet = validateCaptcha(pdu->caMsg, pdu->uiMsgLen, remaining);
    if (captchaRet != 0) {
        PDU* respdu = mkPDU();
        respdu->uiType = ENUM_MSG_TYPE_LOGIN_RESPOND;
        memcpy(respdu->caData, &captchaRet, sizeof(captchaRet));
        if (captchaRet == -4) {
            memcpy(respdu->caData + 4, &remaining, sizeof(remaining));
        }
        return respdu;
    }
    //取出用户名和密码
    QString username=fixedString(pdu->caData,32);
    QString userpwd=fixedString(pdu->caData+32,32);
    //在数据库中检查用户名和密码是否一致或有问题
    int ret=OperateDB::instance().handleLogin(username.toUtf8().data(),userpwd.toUtf8().data());
    qCDebug(lcAuth)<<"login ret"<<ret<<endl;
    if(ret==1){//记录已登录的用户名
        strname=username;
        authOk=true;
    }
    //将登录结果打包进响应报文并返回
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_LOGIN_RESPOND;
    memcpy(respdu->caData,&ret,sizeof(ret));
    QByteArray usernameBytes=username.toUtf8();
    memcpy(respdu->caData+32,usernameBytes.constData(),qMin(usernameBytes.size(),31));
    return respdu;
}
//查找用户
PDU *MsgHandler::finduser(){
    //将需要查找的用户取出
    char userName[32]={'\0'};
    memcpy(userName,pdu->caData,32);
    //在数据库中查找所需要查找的用户，并返回其状态
    int ret=OperateDB::instance().handleFindUser(userName);
    qCDebug(lcNet)<<"findUser ret"<<ret<<endl;
    //将查找用户的状态打包进响应报文中并返回
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_findUser_RESPOND;
    memcpy(respdu->caData,&ret,sizeof(ret));
    return respdu;
}
//查找所有用户
PDU *MsgHandler::onlineuser(const QString &myname)
{
    //在数据库中查找所有在线用户
    QStringList userNames=OperateDB::instance().handleOnlineUsers(myname);
    //打印查找信息
    qCDebug(lcNet)<<"所有在线用户个数"<<userNames.size()<<" ";
    for(int i=0;i<userNames.size();i++){
        qCDebug(lcNet)<<userNames[i]<<" ";
    } qCDebug(lcNet)<<endl;
    //将查找的在线用户的信息打包进响应报文中并返回
    PDU* respdu=mkPDU(userNames.size()*32);
    respdu->uiType=ENUM_MSG_TYPE_ONLINEUSER_RESPOND;
    for(int i=0;i<userNames.size();i++){
        memcpy(respdu->caMsg+i*32,userNames[i].toStdString().c_str(),32);
    }
    return respdu;
}
//添加好友
PDU *MsgHandler::addfriend()
{
    //取出添加人和目标人
    // REQ-EXT-SEC-001：添加人（自己）以连接级 actor 覆盖
    const QString claimedSelf = fixedString(pdu->caData, 32);
    if(claimedSelf != m_actorName){
        qCWarning(lcNet)<<"[addfriend] 添加人字段与连接身份不符，按连接身份改写 conn="<<m_connId;
    }
    copySelfField(pdu->caData, m_actorName);
    char userName[32]={'\0'};
    char seedName[32]={'\0'};
    memcpy(userName,pdu->caData,32);
    memcpy(seedName,pdu->caData+32,32);
    //在数据库中检查用户名和密码是否一致或有问题
    int ret=OperateDB::instance().handleAddFriend(userName,seedName);
    qCDebug(lcNet)<<"addfriend ret"<<ret<<endl;
    PDU* respdu=nullptr;
    //将若是添加的好友不在线打包进响应报文并返回
    if(ret!=1){
        respdu=mkPDU();
        respdu->uiType=ENUM_MSG_TYPE_ADDFRIEND_RESPOND;
        memcpy(respdu->caData,&ret,sizeof(ret));
    }
    else {//在线给目标添加的好友发送请求
        PDU*pdu=mkPDU();
        pdu->uiType=ENUM_MSG_TYPE_ADDFRIEND_REQUEST;
        memcpy(pdu->caData,userName,32);
        memcpy(pdu->caData+32,seedName,32);
        MyTcpServer::getinstance().sendAddFriendAndRelease(seedName,pdu);
    }
    return respdu;
}

PDU *MsgHandler::addfriendFin()
{
    // REQ-EXT-SEC-001：同意人（自己）以连接级 actor 覆盖
    const QString claimedSelf = fixedString(pdu->caData, 32);
    if(claimedSelf != m_actorName){
        qCWarning(lcNet)<<"[addfriendFin] 同意人字段与连接身份不符，按连接身份改写 conn="<<m_connId;
    }
    copySelfField(pdu->caData, m_actorName);
    char agreeName[32]={'\0'};
    char seedName[32]={'\0'};
    memcpy(agreeName,pdu->caData,32);
    memcpy(seedName,pdu->caData+32,32);
    //在数据库中检查用户名和密码是否一致或有问题
    int ret=OperateDB::instance().handleAddFriendFin(seedName,agreeName);
    qCDebug(lcNet)<<" addfriendFin ret"<<ret<<endl;
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_ADDFRIEND_RESPOND;
    memcpy(pdu->caData,&ret,sizeof(ret));
    MyTcpServer::getinstance().seedAddFriend(seedName,pdu);
    MyTcpServer::getinstance().seedAddFriend(agreeName,pdu);
    memfree(pdu);
    return nullptr;
}

PDU *MsgHandler::flushfriend(const QString &username)
{
    QStringList ret=OperateDB::instance().handleFlushFriend(username);
    qCDebug(lcNet)<<"ret.size()="<<ret.size();
    for(int i=0;i<ret.size();i++){
        qCDebug(lcNet)<<ret[i]<<" ";
    }qCDebug(lcNet)<<endl;
    PDU* respdu=mkPDU(ret.size()*32);
    respdu->uiType=ENUM_MSG_TYPE_FLUSHFRIEND_RESPOND;
    for(int i=0;i<ret.size();i++){
        memcpy(respdu->caMsg+i*32,ret[i].toStdString().c_str(),32);
    }
    return respdu;
}

PDU *MsgHandler::deletefriend()
{
    // REQ-EXT-SEC-001：删除发起人（自己）在 caData+32，以连接级 actor 覆盖
    const QString claimedSelf = fixedString(pdu->caData + 32, 32);
    if(claimedSelf != m_actorName){
        qCWarning(lcNet)<<"[deletefriend] 发起人字段与连接身份不符，按连接身份改写 conn="<<m_connId;
    }
    copySelfField(pdu->caData + 32, m_actorName);
    char strName[32]={'\0'};
    char tarName[32]={'\0'};
    memcpy(strName,pdu->caData,32);
    memcpy(tarName,pdu->caData+32,32);
    bool ret=OperateDB::instance().handleDeleteFriend(strName,tarName);
    qCDebug(lcNet)<<"deletefriend ret"<<ret<<endl;

    // 跨功能联动：好友删除成功后，清理两人互发的私聊记录
    if(ret){
        OperateDB::instance().deleteFriendMessages(QString::fromUtf8(strName), QString::fromUtf8(tarName));
        qCDebug(lcNet) << "[deletefriend] 已联动清理双方私聊记录: " << strName << " - " << tarName;
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_DELETEFRIEND_RESPOND;
    memcpy(pdu->caData,&ret,sizeof(ret));
    return pdu;
}

PDU *MsgHandler::chat()
{
    // REQ-EXT-SEC-001：发送人字段以连接级 actor 强制覆盖（入库与转发报文同步改写），客户端提交只作请求数据
    const QString claimedSender = fixedString(pdu->caData, 32);
    if(claimedSender != m_actorName){
        qCWarning(lcNet)<<"[chat] 发送人字段与连接身份不符，按连接身份改写 conn="<<m_connId<<" claimed="<<claimedSender;
    }
    copySelfField(pdu->caData, m_actorName);
    char seedName[32]={'\0'};//发送人
    char tarName[32]={'\0'};//接收人
    memcpy(seedName,pdu->caData,32);
    memcpy(tarName,pdu->caData+32,32);
    char msg[120]={'\0'};//发送信息
    char currTime[30]={'\0'};//发送时间
    Msg* pdu_msg=(Msg*)pdu->caMsg;
    memcpy(msg,pdu_msg->msg,pdu_msg->msgLen);
    memcpy(currTime,pdu_msg->Time,30);
    qCDebug(lcNet)<<"接收到的msg"<<QString(msg)<<" 时间为"<<QString(currTime)<<endl;
    bool ret= OperateDB::instance().seedchatMsg(seedName,tarName,QString(msg),QString(currTime));
    qCDebug(lcNet)<<"char ret"<<ret<<endl;
    if(ret){
        MyTcpServer::getinstance().reseed(tarName,pdu);//转发给接收人
    }
    return nullptr;
}
//创建目录
// REQ-FS-003: 统一路径沙箱校验实现（唯一实现，所有入口复用）
bool MsgHandler::isPathInSandbox(const QString &path, const QString &root)
{
    if (path.isEmpty()) return false;

    const QString rootRaw = root.isEmpty() ? Server::getinstance().filesPath : root;
    // connect.config 实测 filesPath 为相对路径 "./filesys"，先绝对化归一再 canonical
    QFileInfo rootInfo(QDir(rootRaw).absolutePath());
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty()) return false;      // 沙箱根不存在 → 一律拒绝

    QFileInfo targetInfo(path);
    const QString canonicalTarget = targetInfo.canonicalFilePath();
    if (canonicalTarget.isEmpty()) return false;    // 路径不存在（含非法路径）→ 拒绝

    QString prefix = canonicalRoot;
    if (!prefix.endsWith('/')) prefix.append('/');

    return canonicalTarget == canonicalRoot || canonicalTarget.startsWith(prefix);
}

PDU *MsgHandler::MKDIR(){
    //获取目录名和路径
    char fileName[32]={'\0'};
    QString path(QString::fromUtf8(pdu->caMsg));
    memcpy(fileName,pdu->caData,32);
    //构建QDir对象，创建目录，分两步
    bool ret1=false;
    bool ret2=false;
    const QString dirName = fixedString(fileName, 32);
    // REQ-FS-003⑤: path 为待建目录的所在父目录（须存在且在沙箱内）；新目录名不得含路径分隔符
    // REQ-EXT-SEC-001: 收敛为 actor 本人用户根（filesPath/<actor>），跨用户目录一律拒绝
    if(pathInActorRoot(path) && !dirName.contains('/') && !dirName.contains('\\')){
        QDir dir;
        ret1=dir.mkdir(QString("%1").arg(path));
        ret2=dir.mkdir(QString("%1/%2").arg(path).arg(dirName));
    } else {
        qCWarning(lcFs)<<"[MKDIR] 拒绝越权建目录 conn="<<m_connId<<" 路径:"<<path<<" 名:"<<dirName;
    }
    qCDebug(lcFs)<<"mkdir" <<QString("%1").arg(path)<<"ret1"<<ret1<<" ";
    qCDebug(lcFs)<<"mkdir" <<QString("%1/%2").arg(path).arg(fileName)<<"ret2"<<ret2<<" ";
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_MKDIR_RESPOND;
    memcpy(respdu->caData,&ret2,sizeof(ret2));
    return respdu;
}
//删除目录
PDU *MsgHandler::deldir(const QString& loginName)
{
    //获取删除文件路径和文件类型
    QString delFilePath(QString::fromUtf8(pdu->caMsg));
    uint delFileType=0;
    memcpy(&delFileType,pdu->caData,sizeof(uint));
    qCDebug(lcFs)<<"删除文件:"<<delFilePath<<"文件类型"<<delFileType<<endl;
    bool ans=0;//记录删除结果，默认删除失败
    // REQ-FS-003②: 先校验后删除，顺序不可颠倒（removeRecursively 不可逆）
    // REQ-EXT-SEC-001: 只能删除 actor 本人用户根内的路径（loginName 参数即连接级身份）
    // REQ-EXT-FILE-001 D5：对象文件不物理落用户路径，改为校验所在目录在 actor 根内
    if(!parentDirInActorRoot(delFilePath)){
        qCWarning(lcFs)<<"[deldir] 拒绝越权删除 conn="<<m_connId<<" 路径:"<<delFilePath;
    }
    else {
        // REQ-EXT-FILE-001 D5：先按逻辑条目解引用（验收3：减引用，归零→待回收，不物理删对象）；
        // 无条目则回退物理删除（未迁移存量文件）。
        const QFileInfo delInfo(delFilePath);
        const QString delParent = FileObjectStore::normalizeParent(delInfo.absolutePath());
        const QString delName = delInfo.fileName();
        const FileObjectStore::EntryInfo e =
                FileObjectStore::instance().entryByPath(loginName, delParent, delName);
        if(e.valid()){
            if(e.isDir){
                ans = FileObjectStore::instance().releaseSubtree(e.fileId, loginName);
            } else {
                ans = FileObjectStore::instance().releaseEntry(e.fileId, loginName);
            }
        } else if(delFileType==1){
            QFile file(delFilePath);
            ans=file.remove();
        } else if(delFileType==0){
            QDir dir(delFilePath);
            ans=dir.removeRecursively();
        }
    }

    // 跨功能联动：删除成功后，清理该文件（或目录下所有文件）对应的分享链接记录
    if(ans){
        // owner 直接使用登录名；relPath 以用户根目录为基准取相对路径，
        // 避免相对 filesPath 反推时把 filesys 误判为 owner 段
        QString userRoot = QDir(Server::getinstance().filesPath).absoluteFilePath(loginName);
        QString relPath = QDir(userRoot).relativeFilePath(QFileInfo(delFilePath).absoluteFilePath());
        if(!relPath.startsWith("..") && !relPath.startsWith("/") && !relPath.isEmpty()){
            OperateDB::instance().deleteShareLinkByPath(loginName, relPath);
            qCDebug(lcFs) << "[deldir] 已联动清理分享链接: owner=" << loginName << " relPath=" << relPath;
        }
    }

    //返回报文
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_DELFILE_RESPOND;
    memcpy(respdu->caData,&ans,sizeof(ans));
    return respdu;
}
//切换目录
PDU *MsgHandler::changedir(){
    QString tarPath=QString(pdu->caMsg).toUtf8();
    qCDebug(lcFs)<<"tarPath"<<tarPath<<" ";
    QFileInfo FileInfoi(tarPath.toStdString().c_str());
    qCDebug(lcFs)<<tarPath.toStdString().c_str()<<" ";
    PDU* respdu=nullptr;
    bool isExist=FileInfoi.exists();
    // REQ-FS-003⑤: 沙箱外路径按"不存在"处理，不回显任意路径的存在性与名称
    // REQ-EXT-SEC-001: 探测范围收敛为 actor 本人用户根，他人目录按"不存在"处理
    if(isExist && !pathInActorRoot(tarPath)){
        isExist=false;
        qCWarning(lcFs)<<"[changedir] 拒绝越权路径探测 conn="<<m_connId<<":"<<tarPath;
    }
    qCDebug(lcFs)<<" isExist "<<isExist<<" ";
    //如果文件存在
    if(isExist){
        if(FileInfoi.isDir()){//如果是目录
            //目标目录长度
            int PathLen=tarPath.toStdString().size();
            respdu=mkPDU(PathLen+1);//创建pdu，长度为目标目录长度+1
            memcpy(respdu->caMsg,tarPath.toStdString().c_str(),PathLen);
            qCDebug(lcFs)<<" isdir "<<"tarPath"<<tarPath<<endl;
        }
        else if(FileInfoi.isFile()){//如果是文件
            int index=tarPath.lastIndexOf('/');
            tarPath=tarPath.remove(index,tarPath.size()-index);
            //目标目录长度
            int PathLen=tarPath.toStdString().size();
            respdu=mkPDU(PathLen+1);//创建pdu，长度为目标目录长度+1
            memcpy(respdu->caMsg,tarPath.toStdString().c_str(),PathLen);
            qCDebug(lcFs)<<" isFile "<<"tarPath"<<tarPath<<endl;
        }
    }
    //如果文件不存在
    else {
        respdu=mkPDU();
    }
    respdu->uiType=ENUM_MSG_TYPE_CHANGEDIR_RESPOND;
    memcpy(respdu->caData,&isExist,sizeof (isExist));
    return respdu;
}
//刷新目录
PDU *MsgHandler::flushfile()
{
    QString path(QString::fromUtf8(pdu->caMsg));
    // REQ-FS-003⑤: 沙箱外目录一律不回显条目
    // REQ-EXT-SEC-001: 列目录范围收敛为 actor 本人用户根
    if(!pathInActorRoot(path)){
        qCWarning(lcFs)<<"[flushfile] 拒绝越权列目录 conn="<<m_connId<<":"<<path;
        PDU* respdu=mkPDU();
        respdu->uiType=ENUM_MSG_TYPE_FLUSHFILE_RESPOND;
        return respdu;
    }
    // REQ-EXT-FILE-001 D5：逻辑条目为权威；未迁移物理文件按名并入（不与 active 条目重名）。
    struct Row { QString name; bool isDir; };
    QVector<Row> rows;
    QSet<QString> seen;
    const QString parent = FileObjectStore::normalizeParent(path);
    const QList<FileObjectStore::EntryInfo> entries =
            FileObjectStore::instance().listEntries(m_actorName, parent);
    for(const FileObjectStore::EntryInfo& e : entries){
        Row r; r.name=e.displayName; r.isDir=e.isDir; rows.append(r); seen.insert(e.displayName);
    }
    QDir dir(path);
    if(dir.exists()){
        const QFileInfoList phys = dir.entryInfoList();
        for(int i=0;i<phys.size();++i){
            const QString nm = phys[i].fileName();
            if(nm=="."||nm==".."||nm==".staging"||nm=="objects") continue;
            if(seen.contains(nm)) continue;
            Row r; r.name=nm; r.isDir=phys[i].isDir(); rows.append(r);
        }
    }
    PDU* respdu=mkPDU(sizeof(FileInfo)*rows.size());
    respdu->uiType=ENUM_MSG_TYPE_FLUSHFILE_RESPOND;
    for(int i=0;i<rows.size();++i){
        FileInfo* pFileInfo=(FileInfo*)respdu->caMsg+i;
        memcpy(pFileInfo->caName,rows[i].name.toUtf8().constData(),32);
        pFileInfo->uiType=rows[i].isDir?0:1;
    }
    return respdu;
}
//文件/目录重命名
PDU *MsgHandler::renamefile(const QString& loginName)
{
    //拼接旧文件路径和新文件路径
    char oldFileName[32]={'\0'};
    char newFileName[32]={'\0'};
    memcpy(oldFileName,pdu->caData,32);
    memcpy(newFileName,pdu->caData+32,32);
    qCDebug(lcFs)<<"旧文件名:"<<oldFileName<<" "<<"新文件名:"<<newFileName<<" ";
    // 旧名/新名共用同一所在目录 dirPath（现状布局），校验目录一处即可；
    // 不校验 newFilePath：新文件尚不存在，canonical 为空会误拒所有正常重命名
    QString dirPath(QString::fromUtf8(pdu->caMsg+sizeof(uint)));
    // F-1 修复：名称段同 MKDIR 模式拒绝 '/'、'\\'，堵 "dirPath/../x" 拼接逃逸
    const QString oldName = fixedString(oldFileName, 32);
    const QString newName = fixedString(newFileName, 32);
    QString oldFilePath(dirPath+"/"+oldName);
    QString newFilePath(dirPath+"/"+newName);
    uint fileType=0;
    memcpy(&fileType,pdu->caMsg,sizeof(fileType));
    qCDebug(lcFs)<<"文件类型:"<<fileType<<" "<<"旧文件路径:"<<oldFilePath<<" "<<"新文件路径:"<<newFilePath<<" ";
    //进行文件或目录的重命名
    bool ret=1;
    // REQ-FS-003③: 先校验后重命名；未知 fileType 维持原状（ret=1 不执行改名）
    // REQ-EXT-SEC-001: 目录收敛为 actor 本人用户根（loginName 参数即连接级身份）
    if(!pathInActorRoot(dirPath)){
        ret=0;
        qCWarning(lcFs)<<"[renamefile] 拒绝越权重命名目录 conn="<<m_connId<<":"<<dirPath;
    }
    else if(oldName.contains('/') || oldName.contains('\\')
            || newName.contains('/') || newName.contains('\\')){
        // F-1: 名称段含分隔符时 dirPath 校验可被 "dirPath/../x" 拼接绕过，一律拒绝
        ret=0;
        qCWarning(lcFs)<<"[renamefile] 拒绝越权重命名名称段 conn="<<m_connId;
    }
    else if(fileType==0){//目录
        QDir dir;
        ret=dir.rename(oldFilePath,newFilePath);
    }
    else if(fileType==1){//文件
        QFile file;
        ret=file.rename(oldFilePath,newFilePath);
    }
    qCDebug(lcFs)<<"rename ret"<<ret<<endl;

    // 跨功能联动：重命名成功后，平移该文件的分享链接 relative_path
    if(ret){
        // owner 直接使用登录名；relDir 以用户根目录为基准取相对路径，
        // 避免相对 filesPath 反推时把 filesys 误判为 owner 段
        // dirPath 已在重命名前取得（校验对象即此目录）
        // relDir：去掉"用户根目录"前缀得到相对路径。
        // 直接字符串剥离，避免 relativeFilePath 在 CWD 不一致时算出 ".." 开头，
        // 从而被下方守卫跳过整段联动（即重命名未关联 share_link 的根因）。
        QString userRootRel = QDir(Server::getinstance().filesPath).filePath(loginName); // 如 ./filesys/wcy
        QString relDir = dirPath;
        if (relDir.startsWith(userRootRel)) {
            relDir = relDir.mid(userRootRel.length());
            if (relDir.startsWith('/')) relDir = relDir.mid(1);
        } else {
            // 兜底：dirPath 非预期格式时退回 relativeFilePath 计算
            relDir = QDir(QDir(Server::getinstance().filesPath).absoluteFilePath(loginName))
                         .relativeFilePath(QFileInfo(dirPath).absoluteFilePath());
        }
        if(!relDir.startsWith("..") && !relDir.startsWith("/")){
            QString oldRel = relDir.isEmpty() ? oldName
                                             : relDir + "/" + oldName;
            QString newRel = relDir.isEmpty() ? newName
                                             : relDir + "/" + newName;
            OperateDB::instance().updateShareLinkPath(loginName, oldRel, newRel);
            qCDebug(lcFs) << "[renamefile] 已联动更新分享链接: owner=" << loginName
                     << " oldRel=" << oldRel << " newRel=" << newRel;
        }
    }

    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_RENAMEFILE_RESPOND;
    memcpy(respdu->caData,&ret,sizeof(ret));
    return respdu;
}
//分享文件（询问）
PDU *MsgHandler::sharefile()
{
    //发送用户名
    // REQ-EXT-SEC-001：分享人（自己）以连接级 actor 覆盖，转发给他人的署名即服务端身份
    const QString claimedSharer = fixedString(pdu->caData, 32);
    if(claimedSharer != m_actorName){
        qCWarning(lcFs)<<"[sharefile] 分享人字段与连接身份不符，按连接身份改写 conn="<<m_connId;
    }
    copySelfField(pdu->caData, m_actorName);
    char seedName[32]={'\0'};
    memcpy(seedName,pdu->caData,32);
    int revUserCnt=0;
    memcpy(&revUserCnt,pdu->caData+32,sizeof (revUserCnt));
    qCDebug(lcFs)<<"seedName "<<seedName<<" revUserCnt "<<revUserCnt<<" ";
    PDU* resendpdu=mkPDU(pdu->uiMsgLen-revUserCnt*32);
    resendpdu->uiType=ENUM_MSG_TYPE_SHAREFILE_AGREE_REQUEST;
    memcpy(resendpdu->caData,seedName,32);
    memcpy(resendpdu->caMsg,pdu->caMsg+32*revUserCnt,resendpdu->uiMsgLen);
    qCDebug(lcFs)<<"resendpdu->caMsg"<<resendpdu->caMsg<<endl;
    // REQ-EXT-SEC-001 M-1：分享源以服务端 canonical 登记为对每位接收者的握手授权；
    // 接收者同意移动时仅放行有登记的源，伪造 AGREE_RESPOND 拿不到未分享给自己的文件
    int sharePathLen = int(pdu->uiMsgLen) - 32*revUserCnt;
    if(sharePathLen < 0){
        sharePathLen = 0;
    }
    const QByteArray sharePathBytes(pdu->caMsg+32*revUserCnt, sharePathLen);
    const QString shareSrcRaw = QString::fromUtf8(sharePathBytes).trimmed();
    const QString shareCanonicalSrc = QFileInfo(shareSrcRaw).canonicalFilePath();
    // B-1 修复（增量复审）：分享源必须落在分享人本人用户根内——只能分享自己的文件；
    // 源不存在或属他人时不登记握手也不转发邀请，杜绝"自我登记+自我同意"复制他人文件
    const bool sourceOwned = !shareCanonicalSrc.isEmpty() && pathInActorRoot(shareSrcRaw);
    if(!sourceOwned){
        qCWarning(lcFs)<<"[sharefile] 拒绝：分享源不属于分享人，不登记不转发 conn="<<m_connId<<" 源:"<<shareSrcRaw;
    }
    for(int i=0;i<revUserCnt && sourceOwned;i++){
        char revName[32]={'\0'};
        memcpy(revName,pdu->caMsg+32*i,32);
        qCDebug(lcFs)<<"revName"<<revName<<" ";
        MyTcpServer::getinstance().addPendingShare(QString::fromUtf8(revName).trimmed(), shareCanonicalSrc);
        MyTcpServer::getinstance().reseed(revName,resendpdu);
    }
    memfree(resendpdu);
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_SHAREFILE_RESPOND;
    return respdu;
}
//分享文件（移动）
PDU *MsgHandler::sharefilemove()
{
    // REQ-EXT-SEC-001：接收者目录以连接级 actor 生成（PDU caData 用户字段仅作请求数据）；
    // 源路径必须持有分享人对本接收者的握手登记（M-1 修复），未授权的任意沙箱源被拒绝
    QString sourcePath=QString::fromUtf8(pdu->caMsg);
    int indexi=sourcePath.lastIndexOf('/');
    QString fileName=sourcePath.right(sourcePath.size()-indexi-1);
    QString recvName=m_actorName;
    if(recvName.isEmpty()){
        qCWarning(lcFs)<<"[sharefilemove] 拒绝：连接无认证上下文 conn="<<m_connId;
        PDU* respdu=mkPDU();
        bool fail=false;
        memcpy(respdu->caData,&fail,sizeof(fail));
        respdu->uiType=ENUM_MSG_TYPE_SHAREFILE_FINISH_RESPOND;
        return respdu;
    }
    const QString claimedRecv=QString::fromUtf8(pdu->caData);
    if(claimedRecv!=recvName){
        qCWarning(lcFs)<<"[sharefilemove] 接收者字段与连接身份不符，按连接身份生成目标 conn="<<m_connId;
    }
    QString recvDir=QString("%1/%2").arg(Server::getinstance().filesPath).arg(recvName);
    QString tarPath=QString("%1/%2").arg(recvDir).arg(fileName);
    qCDebug(lcFs)<<"sourcePath:"<<sourcePath<<"tarPath："<<tarPath<<endl;
    // REQ-FS-003⑤: 源与目标目录都必须在沙箱内；目标文件名不得含路径分隔符（Windows 下 "\.." 逃逸）
    const QString canonicalSrc=QFileInfo(sourcePath).canonicalFilePath();
    bool ret=false;
    if(!isPathInSandbox(sourcePath)){
        qCWarning(lcFs)<<"[sharefilemove] 拒绝越权源路径 conn="<<m_connId<<":"<<sourcePath;
    } else if(!pathInActorRoot(recvDir)){
        qCWarning(lcFs)<<"[sharefilemove] 拒绝越权目标目录 conn="<<m_connId<<":"<<recvDir;
    } else if(fileName.contains('\\') || fileName.contains('/')){
        qCWarning(lcFs)<<"[sharefilemove] 拒绝含路径分隔符的目标名 conn="<<m_connId;
    } else if(canonicalSrc.isEmpty()
              || !MyTcpServer::getinstance().hasPendingShare(recvName, canonicalSrc)){
        // REQ-EXT-SEC-001 M-1: 无握手登记的源一律拒绝；复制执行改用服务端 canonical
        qCWarning(lcFs)<<"[sharefilemove] 拒绝：源无分享握手登记 conn="<<m_connId<<":"<<sourcePath;
    } else {
        ret=QFile::copy(canonicalSrc,tarPath);
        if(ret){
            MyTcpServer::getinstance().consumePendingShare(recvName, canonicalSrc);
        }
    }
    PDU* respdu=mkPDU();
    memcpy(respdu->caData,&ret,sizeof (ret));
    respdu->uiType=ENUM_MSG_TYPE_SHAREFILE_FINISH_RESPOND;
    return respdu;

}

//移动文件（任意目录间移动）
PDU *MsgHandler::movefile(const QString& loginName)
{
    char srcFileName[32]={'\0'};
    memcpy(srcFileName,pdu->caData,32);
    QString fileName=QString::fromUtf8(srcFileName).trimmed();

    // caMsg 格式：srcRel + '\0' + dstRel（两个以\0结尾的C串，srcRel 含文件名）
    QByteArray caMsgBytes(pdu->caMsg, pdu->uiMsgLen);
    int sep = caMsgBytes.indexOf('\0');
    QString srcRel = QString::fromUtf8(caMsgBytes.left(sep)).trimmed();
    QString dstRel = QString::fromUtf8(caMsgBytes.mid(sep+1)).trimmed();

    QString userRoot=QDir(Server::getinstance().filesPath).filePath(loginName);
    QString srcPath=QDir(userRoot).filePath(srcRel);   // srcRel 已含文件名
    QString dstDir =QDir(userRoot).filePath(dstRel);    // 目标目录
    QString dstPath=QDir(dstDir).filePath(fileName);
    QString finalDstPath = dstPath;

    //路径合法性校验：源路径必须在用户根目录内
    // REQ-FS-003④: 统一走 isPathInSandbox，根传用户目录（原校验根即 userRoot，隔离级别不变；
    // canonicalSrc/canonicalDstDir 仍保留，供下方"移入自身子目录"与原地移动判断使用）
    QFileInfo srcInfo(srcPath);
    QString canonicalSrc=srcInfo.canonicalFilePath();
    QFileInfo dstInfo(dstDir);
    QString canonicalDstDir = dstInfo.canonicalFilePath(); // 目标目录必定存在

    bool ret=false;
    if(isPathInSandbox(srcPath, userRoot) && srcInfo.exists()){
        //确保目标目录也在用户根目录内
        if(dstInfo.isDir() && isPathInSandbox(dstDir, userRoot)){
            // 防止把目录移进自身或其子目录
            if(srcInfo.isDir() && (canonicalDstDir==canonicalSrc || canonicalDstDir.startsWith(canonicalSrc+"/"))){
                qCWarning(lcFs)<<"movefile 拒绝：不能把目录移进自身或其子目录";
            }
            // 原地移动（目标目录就是源所在目录）：无需操作
            else if(canonicalDstDir == srcInfo.dir().canonicalPath()){
                ret=true;
                qCDebug(lcFs)<<"movefile 原地移动，无需操作";
            }
            else {
                // 自动重命名处理逻辑
                finalDstPath = dstPath;
                if (QFileInfo::exists(finalDstPath)) {
                    QFileInfo checkInfo(finalDstPath);
                    QDir parentDir = checkInfo.dir(); // 获取父目录 QDir 对象
                    QString baseName = checkInfo.completeBaseName();
                    QString suffix = checkInfo.suffix();
                    if (!suffix.isEmpty()) suffix = "." + suffix;

                    int count = 1;
                    const int MAX_RETRIES = 1000;
                    // 使用 QDir::filePath() 构建路径，确保跨平台安全
                    do {
                        QString numberedName = QString("%1(%2)%3").arg(baseName).arg(count++).arg(suffix);
                        finalDstPath = parentDir.filePath(numberedName);
                        if (count > MAX_RETRIES) { ret = false; break; }
                    } while (QFileInfo::exists(finalDstPath));
                    qCDebug(lcFs) << "文件移动重名，自动重命名为:" << finalDstPath;
                }

                //先尝试 rename；若跨文件系统失败则 copy+remove
                QFile srcFile(srcPath);
                ret = srcFile.rename(finalDstPath);
                if (!ret) {
                    qCWarning(lcFs) << "rename 失败, errorString:" << srcFile.errorString()
                             << "src:" << srcPath << "dst:" << finalDstPath;
                    // 降级策略：copy + remove
                    ret = QFile::copy(srcPath, finalDstPath);
                    if (!ret) {
                        qCWarning(lcFs) << "copy 降级也失败, src:" << srcPath << "dst:" << finalDstPath
                                 << "errno:" << errno << strerror(errno);
                    } else {
                        if (QFile::remove(srcPath)) {
                            qCDebug(lcFs) << "copy+remove 降级成功:" << finalDstPath;
                        } else {
                            qCWarning(lcFs) << "copy+remove 降级: copy 成功但 remove 失败, src:" << srcPath;
                            QFile::remove(finalDstPath);  // 清理不完整的目标文件
                            ret = false;
                        }
                    }
                }
            }
        } else {
            qCWarning(lcFs) << "movefile 目标路径校验失败: canonicalDstDir=" << canonicalDstDir;
        }
    } else {
        qCWarning(lcFs) << "movefile 源路径校验失败: canonicalSrc=" << canonicalSrc;
    }

    qCDebug(lcFs)<<"movefile src:"<<srcPath<<"dst:"<<dstPath<<"ret:"<<ret;

    // 跨功能联动：移动成功后，平移该文件的分享链接 relative_path
    if(ret){
        QString newRel = dstRel.isEmpty() ? fileName : (dstRel + "/" + fileName);
        OperateDB::instance().updateShareLinkPath(loginName, srcRel, newRel);
        qCDebug(lcFs) << "[movefile] 已联动更新分享链接: owner=" << loginName
                 << " oldRel=" << srcRel << " newRel=" << newRel;
    }

    PDU* respdu=mkPDU();
    memcpy(respdu->caData,&ret,sizeof(ret));
    QByteArray fileNameBytes=fileName.toUtf8();
    memcpy(respdu->caData+32,fileNameBytes.constData(),qMin(fileNameBytes.size(),31));
    respdu->uiType=ENUM_MSG_TYPE_MOVEFILE_RESPOND;
    return respdu;
}
//列出某相对目录下的子目录（移动文件选择目标用）
PDU *MsgHandler::listdir(const QString& loginName)
{
    QString relPath=QString::fromUtf8(pdu->caMsg).trimmed(); // 相对根目录的浏览路径（空=根）
    QString userRoot=QDir(Server::getinstance().filesPath).filePath(loginName);
    QString absDir=QDir(userRoot).filePath(relPath);

    // 越权校验：浏览目录必须在用户根目录内
    // REQ-FS-003④: 统一走 isPathInSandbox，根传用户目录（原校验根即 userRoot，隔离级别不变）
    QFileInfo dirInfo(absDir);

    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_LISTDIR_RESPOND;
    if(isPathInSandbox(absDir, userRoot) && dirInfo.isDir()){
        QFileInfoList list=QDir(absDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        respdu=mkPDU(sizeof(FileInfo)*list.size());
        respdu->uiType=ENUM_MSG_TYPE_LISTDIR_RESPOND;
        for(int i=0;i<list.size();i++){
            FileInfo* p=(FileInfo*)respdu->caMsg+i;
            memcpy(p->caName,list[i].fileName().toStdString().c_str(),32);
            p->uiType=0;
        }
        qCDebug(lcFs)<<"listdir rel="<<relPath<<" 子目录数="<<list.size();
    } else {
        qCWarning(lcFs)<<"listdir 越权或目录不存在: rel="<<relPath;
    }
    return respdu;
}

//上传文件初始化
PDU *MsgHandler::uploadfileinit()
{
    char strname[32]={'\0'};
    qint64 fileSize=0;
    uint resumeMagic=0;
    memcpy(strname,pdu->caData,32);
    memcpy(&fileSize,pdu->caData+UPLOAD_FILE_SIZE_OFFSET,sizeof(qint64));
    memcpy(&resumeMagic,pdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,sizeof(uint));

    const bool resumeEnabled = (resumeMagic == UPLOAD_RESUME_MAGIC);
    const QString dirPath = QString::fromUtf8(pdu->caMsg);
    const QString fileName = fixedString(strname, 32);
    // REQ-EXT-FILE-001 T4：内容寻址暂存——写进 .staging 而非最终目录，完成时 commitUpload 转正。
    // REQ-FS-003①/SEC-001：所在目录须在 actor 本人用户根内；父路径规范化后作逻辑条目键。
    ResumableUploadSession::BeginResult result = {false, 0, false};
    m_uploadSessionId = -1;
    const QString parentPath = FileObjectStore::normalizeParent(dirPath);
    if (pathInActorRoot(dirPath) && !m_actorName.isEmpty()) {
        result = m_uploadSession.beginStaged(m_actorName, parentPath, fileName, fileSize, resumeEnabled);
        if (result.ok) {
            // 仅预检元数据（客户端不提交摘要，服务端复算为准）；登记会话行
            m_uploadSessionId = OperateDB::instance().createUploadSession(
                m_actorName, parentPath, fileName, fileSize,
                QString(), m_uploadSession.stagingFilePath(), result.offset, 3600);
            if (m_uploadSessionId < 0) {
                qCWarning(lcFs) << "uploadfileinit 会话登记失败 conn=" << m_connId;
            }
            if (result.complete) {
                // 0 字节或断点已到尾部：立即落定（仍走服务端复算，验收4）
                finalizeUploadOnComplete();
            }
        }
    } else {
        qCWarning(lcFs) << "uploadfileinit 拒绝越权目录 conn=" << m_connId << ":" << dirPath;
    }

    bool ret=result.ok;
    qint64 resumeOffset=result.offset;
    qCDebug(lcFs)<<"uploadfileinit file"<<fileName
            <<"size"<<fileSize
            <<"resume"<<resumeEnabled
            <<"offset"<<resumeOffset
            <<"complete"<<result.complete
            <<"ret"<<ret<<endl;
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_UPLOADFILE_INIT_RESPOND;
    memcpy(respdu->caData,&ret,sizeof (ret));
    memcpy(respdu->caData+UPLOAD_RESUME_OFFSET,&resumeOffset,sizeof(resumeOffset));
    if(resumeEnabled){
        memcpy(respdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,&resumeMagic,sizeof(resumeMagic));
    }
    return respdu;
}
//上传文件数据
PDU *MsgHandler::uploadfiledata()
{
    uint resumeMagic=0;
    qint64 chunkOffset=0;
    memcpy(&resumeMagic,pdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,sizeof(uint));
    if(resumeMagic == UPLOAD_RESUME_MAGIC){
        memcpy(&chunkOffset,pdu->caData+UPLOAD_CHUNK_OFFSET,sizeof(qint64));
        if(chunkOffset != m_uploadSession.receivedSize()){
            bool ret=false;
            PDU* respdu=mkPDU();
            respdu->uiType=ENUM_MSG_TYPE_UPLOADFILE_DATA_RESPOND;
            memcpy(respdu->caData,&ret,sizeof(ret));
            memcpy(respdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,&resumeMagic,sizeof(resumeMagic));
            return respdu;
        }
    }

    bool ret=false; // Upload writes are dispatched by MyTcpSocket to UploadWriteTask.
    qCDebug(lcFs)<<"uploadfiledata len"<<pdu->uiMsgLen
            <<"received"<<m_uploadSession.receivedSize()
            <<"total"<<m_uploadSession.totalSize()
            <<"ret"<<ret<<endl;
    if(ret && !m_uploadSession.isComplete()){
        return nullptr;
    }
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_UPLOADFILE_DATA_RESPOND;
    memcpy(respdu->caData,&ret,sizeof(ret));
    resumeMagic=UPLOAD_RESUME_MAGIC;
    memcpy(respdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,&resumeMagic,sizeof(resumeMagic));
    return respdu;
}
// REQ-EXT-FILE-001 T4：上传完成→服务端复算转正 + 建逻辑条目 + 引用计数（同事务）。
// commitUpload 失败即拒绝转正（验收4）；staging 由其转正搬移或丢弃（验收1）。
void MsgHandler::finalizeUploadOnComplete()
{
    const QString owner = m_actorName;
    const QString parent = m_uploadSession.parentPath();
    const QString display = m_uploadSession.displayName();
    const qint64 expected = m_uploadSession.totalSize();
    const QString staging = m_uploadSession.stagingFilePath();
    if (owner.isEmpty() || parent.isEmpty() || display.isEmpty() || staging.isEmpty()) {
        qCWarning(lcFs) << "[finalizeUpload] 会话上下文缺失，拒绝转正 conn=" << m_connId;
        return;
    }
    qCDebug(lcFs) << "[finalizeUpload] verifying conn=" << m_connId << " owner=" << owner << " display=" << display;
    OperateDB::instance().finishUploadSession(m_uploadSessionId, "verifying");
    const FileObjectStore::CommitResult r =
        FileObjectStore::instance().commitUpload(owner, parent, display, expected, staging);
    if (r.ok) {
        OperateDB::instance().finishUploadSession(m_uploadSessionId, "done");
        qCDebug(lcFs) << "[finalizeUpload] 落定 fileId=" << r.fileId << " objId=" << r.objectId
                      << " dedup=" << r.dedupHit << " 定名=" << r.displayName;
    } else {
        OperateDB::instance().finishUploadSession(m_uploadSessionId, "failed");
        qCWarning(lcFs) << "[finalizeUpload] 拒绝转正 conn=" << m_connId << " 原因=" << r.error;
    }
    m_uploadSession.reset();
    m_uploadSessionId = -1;
}

//下载文件（仅做校验，流式分块由 MyTcpSocket 完成）
PDU *MsgHandler::downloadfile()
{
    QString filePath=QString::fromUtf8(pdu->caMsg);
    QFileInfo fileInfo(filePath);
    // REQ-FS-003④: 统一走 isPathInSandbox（原校验根即整个 filesPath，与默认根一致；
    // canonicalFilePath 仍保留，供 MyTcpSocket 流式下载使用）
    QString canonicalFilePath=fileInfo.canonicalFilePath();
    QString fileName=fixedString(pdu->caData,32);
    bool ret=false;
    qint64 fileSize=0;
    m_downloadCanonicalPath.clear();

    // REQ-EXT-SEC-001: 下载范围收敛为 actor 本人用户根，他人文件不可下载
    // REQ-EXT-FILE-001 D5：对象文件不物理落用户路径，改为校验所在目录在 actor 根内
    if(parentDirInActorRoot(filePath) && !fileName.isEmpty()){
        // REQ-EXT-FILE-001 D5：优先解析逻辑条目→对象绝对路径；对象缺失回退物理文件（未迁移）。
        const QString dlParent = FileObjectStore::normalizeParent(fileInfo.absolutePath());
        const FileObjectStore::EntryInfo e =
                FileObjectStore::instance().entryByPath(m_actorName, dlParent, fileName);
        const QString objAbs = (e.valid() && !e.isDir)
                ? FileObjectStore::instance().objectAbsPath(e.objectId) : QString();
        if(!objAbs.isEmpty() && QFileInfo(objAbs).isFile()){
            fileSize = e.sizeBytes > 0 ? e.sizeBytes : QFileInfo(objAbs).size();
            m_downloadCanonicalPath = objAbs;
            ret = true;
        } else if(fileInfo.isFile()){
            fileSize=fileInfo.size();
            m_downloadCanonicalPath=canonicalFilePath;
            ret=true;
        }
    }
    if(!ret){
        qCWarning(lcFs)<<"[downloadfile] 拒绝越权/不存在下载 conn="<<m_connId<<" 路径:"<<filePath;
    }

    //仅返回元数据 PDU（不含文件内容），实际数据由 MyTcpSocket 流式发送
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND;
    memcpy(respdu->caData,&ret,sizeof(ret));
    memcpy(respdu->caData+sizeof(bool),&fileSize,sizeof(fileSize));
    QByteArray fileNameBytes=fileName.toUtf8();
    memcpy(respdu->caData+32,fileNameBytes.constData(),qMin(fileNameBytes.size(),31));
    qCDebug(lcFs)<<"downloadfile validate path"<<filePath<<"ret"<<ret<<"size"<<fileSize;
    return respdu;
}

//获取聊天信息
PDU *MsgHandler::chatmsg()
{
    // REQ-EXT-SEC-001：查询者（自己）在 caData 首段，以连接级 actor 覆盖，防止读取他人聊天历史
    const QString claimedSelf = fixedString(pdu->caData, 32);
    if(claimedSelf != m_actorName){
        qCWarning(lcNet)<<"[chatmsg] 查询者字段与连接身份不符，按连接身份改写 conn="<<m_connId;
    }
    copySelfField(pdu->caData, m_actorName);
    char seedName[32]={'\0'};
    char revName[32]={'\0'};
    memcpy(revName,pdu->caData,32);
    memcpy(seedName,pdu->caData+32,32);
    QStringList Msglist= OperateDB::instance().getchatMsg(seedName,revName);
    qCDebug(lcNet)<<"chatmsg msglist.size"<<Msglist.size()<<" ";
    int Msgtotallen=0;
    for(int i=0;i<Msglist.size();i+=2){
        Msgtotallen+=Msglist[i].toUtf8().size()+1+sizeof(Msg);
    }
    PDU* respdu=mkPDU(Msgtotallen);
    respdu->uiType=ENUM_MSG_TYPE_GETCHATMSG_RESPOND;
    for(int i=0,dis=0;i<Msglist.size();i+=2){
        Msg* filei=(Msg*)(respdu->caMsg+dis);
        filei->totalLen=sizeof(Msg)+Msglist[i].toUtf8().size()+1;
        filei->msgLen=Msglist[i].toUtf8().size()+1;
        memcpy(filei->Time,Msglist[i+1].toUtf8().toStdString().c_str(),30);
        memcpy(filei->msg,Msglist[i].toUtf8().toStdString().c_str(),filei->msgLen);
        qCDebug(lcNet)<<"filei->msg "<<filei->msg<<" filei->Time"<<filei->Time<<",";
        dis+=filei->totalLen;
    }
    return respdu;
}

//获取用户信息
PDU *MsgHandler::getuserinformation()
{
    char userName[32]={'\0'};
    memcpy(userName,pdu->caData,32);

    QString outName;
    int outOnline=0;
    bool found=OperateDB::instance().handleGetUserInfo(userName,outName,outOnline);

    int resultCode=found ? 0 : -1;
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_GETUSERINFORMATION_RESPOND;
    memcpy(respdu->caData,&resultCode,sizeof(resultCode));
    memcpy(respdu->caData+sizeof(int),&outOnline,sizeof(outOnline));
    QByteArray nameBytes=outName.toUtf8();
    memcpy(respdu->caData+32,nameBytes.constData(),qMin(nameBytes.size(),31));
    qCDebug(lcNet)<<"getuserinformation userName:"<<userName<<"found:"<<found<<"online:"<<outOnline;
    return respdu;
}

PDU* MsgHandler::createPublishLink(const QString& loginName) {
    QString relativePath = fixedString(pdu->caMsg, pdu->uiMsgLen);
    QString ownerName = loginName;
    qCDebug(lcFs) << "[createPublishLink] 收到生成请求: owner=" << ownerName << " relativePath=" << relativePath;

    QDateTime expire = ShareLinkService::defaultExpireTime();

    // F-1 修复：同一 owner+path 已有链接时，原实现仍返回"新生成但未入库"的 token，
    // 该链接永远无法解析（库里只有旧记录的 hash）。原始 token 不可反推，但
    // resolvePublishLink 支持 64 位十六进制 hash 直接查库，故去重复用时返回已存 hash 形式链接。
    QString linkValue;
    bool ok = false;
    const QString existingHash = OperateDB::instance().getShareLinkByPath(ownerName, relativePath);
    if(!existingHash.isEmpty()){
        linkValue = existingHash;
        ok = true;
        qCDebug(lcFs) << "[createPublishLink] 复用已有链接，返回 hash 形式链接（可直接解析）owner=" << ownerName;
    } else {
        const QString token = ShareLinkService::generateToken();
        ok = OperateDB::instance().createShareLink(ShareLinkService::hashToken(token), ownerName, relativePath, expire);
        linkValue = token;
        if(!ok){
            qCWarning(lcFs) << "[createPublishLink] 写入数据库失败，生成分享链接未完成";
        }
    }

    PDU* respdu = mkPDU(128);
    respdu->uiType = ENUM_MSG_TYPE_CREATE_PUBLINK_RESPOND;
    memcpy(respdu->caData, &ok, sizeof(ok));
    if(ok) {
        QString link = QString("cloud-disk://share/%1").arg(linkValue);
        QByteArray linkBytes = link.toUtf8();
        memcpy(respdu->caMsg, linkBytes.constData(), linkBytes.size());
    }
    return respdu;
}



// 新增：安全外链分享（BUG-ARC-PUBLINK-001）
PDU* MsgHandler::resolvePublishLink(const QString& loginName) {
    QString token = fixedString(pdu->caMsg, pdu->uiMsgLen);
    qCDebug(lcFs) << "[resolvePublishLink] 收到解析请求: token_len=" << token.size()
             << " token_head=" << token.left(16);

    // 兼容两种输入：
    //  - 原始 token（22 位 base64url，来自「生成分享链接」弹窗）
    //  - token_hash 本身（64 位十六进制，来自「获取分享链接」弹窗 / 模拟脚本输出）
    // 若输入正好是 64 位十六进制，则直接当作 hash 查库；否则先 hash(token) 再查。
    bool isHex64 = (token.size() == 64);
    if (isHex64) {
        for (const QChar &c : token) {
            if (!c.isDigit() && !((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                isHex64 = false;
                break;
            }
        }
    }
    QString hash = isHex64 ? token : ShareLinkService::hashToken(token);
    qCDebug(lcFs) << "[resolvePublishLink] hash 来源:" << (isHex64 ? "直接输入(64hex)" : "hashToken(输入)");

    OperateDB::ShareLinkInfo info;
    PDU* respdu = mkPDU(1024); // 预留 JSON 空间
    respdu->uiType = ENUM_MSG_TYPE_RESOLVE_PUBLINK_RESPOND;

    bool ok = false;
    QString savedRelPath; // 形如 "分享链接/2.pdf"，返回给客户端展示
    if (OperateDB::instance().getShareLinkByHash(hash, info)) {
        qCDebug(lcFs) << "[resolvePublishLink] 命中记录: owner=" << info.ownerName
                 << " relativePath=" << info.relativePath
                 << " revoked=" << info.revoked
                 << " expireTime=" << info.expireTime.toString("yyyy-MM-dd HH:mm:ss");
        if (info.revoked) {
            qCWarning(lcFs) << "[resolvePublishLink] 链接已撤销，拒绝";
        } else if (QDateTime::currentDateTime() >= info.expireTime) {
            qCWarning(lcFs) << "[resolvePublishLink] 链接已过期，拒绝";
        } else {
            QString ownerRoot = QDir(Server::getinstance().filesPath).filePath(info.ownerName);
            QString srcPath = QDir(ownerRoot).filePath(info.relativePath);
            QFileInfo srcFi(srcPath);
            if (ShareLinkService::validatePath(ownerRoot, info.relativePath)
                    && srcFi.exists() && srcFi.isFile()) {
                // 目标目录：服务端【请求者】用户目录下的“分享链接”文件夹
                QString userRoot = QDir(Server::getinstance().filesPath).filePath(loginName);
                QString shareDir = QDir(userRoot).filePath("分享链接");
                QDir().mkpath(shareDir);

                QString baseName = srcFi.completeBaseName();
                QString suffix = srcFi.suffix();
                if (!suffix.isEmpty()) suffix = QString(".") + suffix;

                // 同名去重，复用 movefile 的 (1)(2) 规则
                QString finalDstPath = QDir(shareDir).filePath(srcFi.fileName());
                if (QFileInfo::exists(finalDstPath)) {
                    int count = 1;
                    const int MAX_RETRIES = 1000;
                    do {
                        QString numberedName = QString("%1(%2)%3").arg(baseName).arg(count++).arg(suffix);
                        finalDstPath = QDir(shareDir).filePath(numberedName);
                        if (count > MAX_RETRIES) { finalDstPath.clear(); break; }
                    } while (QFileInfo::exists(finalDstPath));
                }

                if (!finalDstPath.isEmpty()) {
                    QFile srcFile(srcPath);
                    if (srcFile.copy(finalDstPath)) {
                        ok = true;
                        savedRelPath = "分享链接/" + QFileInfo(finalDstPath).fileName();
                        qCDebug(lcFs) << "[resolvePublishLink] 已复制分享文件到:" << finalDstPath;
                    } else {
                        qCWarning(lcFs) << "[resolvePublishLink] 复制失败:" << srcFile.errorString()
                                 << " src=" << srcPath << " dst=" << finalDstPath;
                    }
                } else {
                    qCWarning(lcFs) << "[resolvePublishLink] 同名文件过多，放弃复制";
                }
            } else {
                qCWarning(lcFs) << "[resolvePublishLink] 源文件不存在或越权: " << srcPath;
            }
        }
    } else {
        qCDebug(lcFs) << "[resolvePublishLink] 未找到匹配记录 (hash_head="
                 << hash.left(16) << "...)";
    }

    memcpy(respdu->caData, &ok, sizeof(ok));
    if (ok) {
        QString json = QString("{\"saved_path\":\"%1\"}").arg(savedRelPath);
        QByteArray jsonBytes = json.toUtf8();
        memcpy(respdu->caMsg, jsonBytes.constData(), jsonBytes.size());
    }
    return respdu;
}

PDU* MsgHandler::queryFilePublishLink() {
    // REQ-EXT-SEC-001：owner 以连接级 actor 生成，PDU 用户字段仅作请求数据
    QString relativePath = fixedString(pdu->caMsg, pdu->uiMsgLen);
    QString ownerName = m_actorName;
    if(fixedString(pdu->caData, 32) != ownerName){
        qCWarning(lcFs)<<"[queryFilePublishLink] owner 字段与连接身份不符，按连接身份查询 conn="<<m_connId;
    }
    
    QString token = OperateDB::instance().getShareLinkByPath(ownerName, relativePath);
    
    PDU* respdu = mkPDU(128);
    respdu->uiType = ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_RESPOND;
    bool exists = !token.isEmpty();
    memcpy(respdu->caData, &exists, sizeof(exists));
    if (exists) {
        QString link = QString("cloud-disk://share/%1").arg(token);
        QByteArray linkBytes = link.toUtf8();
        memcpy(respdu->caMsg, linkBytes.constData(), linkBytes.size());
    }
    return respdu;
}
