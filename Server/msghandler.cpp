#include "msghandler.h"
#include "QDebug"
#include "operatedb.h"
#include "mytcpserver.h"
#include "server.h"
#include "sharelinkservice.h"
#include "memalloc.h"
#include <QByteArray>

#include <QFile>
#include <QFileInfo>
#include <QTime>
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
        qDebug() << "验证码校验失败: 账号已锁定，剩余" << remainingSec << "秒";
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
        qDebug() << "验证码校验失败: 无有效码或已过期";
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
            qDebug() << "验证码连续错误" << m_captchaFailCount << "次，账号锁定" << CAPTCHA_LOCKOUT_SEC << "秒";
            return -4;
        }
        qDebug() << "验证码校验失败: 输入不符";
        return -2;
    }

    // 校验成功：一次性使用，作废当前码并重置错误计数
    m_captchaCode.clear();
    m_captchaFailCount = 0;
    qDebug() << "验证码校验成功";
    return 0;
}

//处理注销账户请求
PDU *MsgHandler::logoff()
{
    QString userName = fixedString(pdu->caData, 32);
    QString userPwd  = fixedString(pdu->caData + 32, 32);

    qDebug() << "logoff request for:" << userName;

    bool ret = OperateDB::instance().handleLogoff(
        userName.toUtf8().constData(),
        userPwd.toUtf8().constData()
    );

    // 如果数据库删除成功，清理用户的文件目录
    if(ret){
        QString userDir=QString("%1/%2").arg(Server::getinstance().filesPath).arg(userName);
        QDir dir(userDir);
        if(dir.exists()){
            bool removed=dir.removeRecursively();
            qDebug()<<"logoff: remove user dir"<<userDir<<"result:"<<removed;
        }
    }

    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_LOGOFF_RESPOND;
    memcpy(respdu->caData,&ret,sizeof(ret));
    return respdu;
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
        qDebug() << "生成验证码:" << m_captchaCode << "过期时间:" << m_captchaExpiry;
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
    qDebug()<<"regist ret "<<ret<<endl;
    if(ret){
        QDir dir;
        bool ret1=dir.mkdir(QString("%1").arg(Server::getinstance().filesPath));
        bool ret2=dir.mkdir(QString("%1/%2").arg(Server::getinstance().filesPath).arg(strname));
        qDebug()<<"创建用户目录:"<<"ret1 "<<ret1<<" "<<QString("%1").arg(Server::getinstance().filesPath)<<endl;
        qDebug()<<"创建用户目录:"<<"ret2 "<<ret2<<" "<<QString("%1/%2").arg(Server::getinstance().filesPath).arg(strname)<<endl;
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
PDU *MsgHandler::login(QString &strname)
{
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
    qDebug()<<"login ret"<<ret<<endl;
    if(ret==1){//记录已登录的用户名
        strname=username;
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
    qDebug()<<"findUser ret"<<ret<<endl;
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
    qDebug()<<"所有在线用户个数"<<userNames.size()<<" ";
    for(int i=0;i<userNames.size();i++){
        qDebug()<<userNames[i]<<" ";
    } qDebug()<<endl;
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
    char userName[32]={'\0'};
    char seedName[32]={'\0'};
    memcpy(userName,pdu->caData,32);
    memcpy(seedName,pdu->caData+32,32);
    //在数据库中检查用户名和密码是否一致或有问题
    int ret=OperateDB::instance().handleAddFriend(userName,seedName);
    qDebug()<<"addfriend ret"<<ret<<endl;
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
    char agreeName[32]={'\0'};
    char seedName[32]={'\0'};
    memcpy(agreeName,pdu->caData,32);
    memcpy(seedName,pdu->caData+32,32);
    //在数据库中检查用户名和密码是否一致或有问题
    int ret=OperateDB::instance().handleAddFriendFin(seedName,agreeName);
    qDebug()<<" addfriendFin ret"<<ret<<endl;
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
    qDebug()<<"ret.size()="<<ret.size();
    for(int i=0;i<ret.size();i++){
        qDebug()<<ret[i]<<" ";
    }qDebug()<<endl;
    PDU* respdu=mkPDU(ret.size()*32);
    respdu->uiType=ENUM_MSG_TYPE_FLUSHFRIEND_RESPOND;
    for(int i=0;i<ret.size();i++){
        memcpy(respdu->caMsg+i*32,ret[i].toStdString().c_str(),32);
    }
    return respdu;
}

PDU *MsgHandler::deletefriend()
{
    char strName[32]={'\0'};
    char tarName[32]={'\0'};
    memcpy(strName,pdu->caData,32);
    memcpy(tarName,pdu->caData+32,32);
    bool ret=OperateDB::instance().handleDeleteFriend(strName,tarName);
    qDebug()<<"deletefriend ret"<<ret<<endl;

    // 跨功能联动：好友删除成功后，清理两人互发的私聊记录
    if(ret){
        OperateDB::instance().deleteFriendMessages(QString::fromUtf8(strName), QString::fromUtf8(tarName));
        qDebug() << "[deletefriend] 已联动清理双方私聊记录: " << strName << " - " << tarName;
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_DELETEFRIEND_RESPOND;
    memcpy(pdu->caData,&ret,sizeof(ret));
    return pdu;
}

PDU *MsgHandler::chat()
{
    char seedName[32]={'\0'};//发送人
    char tarName[32]={'\0'};//接收人
    memcpy(seedName,pdu->caData,32);
    memcpy(tarName,pdu->caData+32,32);
    char msg[120]={'\0'};//发送信息
    char currTime[30]={'\0'};//发送时间
    Msg* pdu_msg=(Msg*)pdu->caMsg;
    memcpy(msg,pdu_msg->msg,pdu_msg->msgLen);
    memcpy(currTime,pdu_msg->Time,30);
    qDebug()<<"接收到的msg"<<QString(msg)<<" 时间为"<<QString(currTime)<<endl;
    bool ret= OperateDB::instance().seedchatMsg(seedName,tarName,QString(msg),QString(currTime));
    qDebug()<<"char ret"<<ret<<endl;
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
    if(isPathInSandbox(path) && !dirName.contains('/') && !dirName.contains('\\')){
        QDir dir;
        ret1=dir.mkdir(QString("%1").arg(path));
        ret2=dir.mkdir(QString("%1/%2").arg(path).arg(dirName));
    } else {
        qWarning()<<"[MKDIR] 拒绝越权建目录:"<<path<<dirName;
    }
    qDebug()<<"mkdir" <<QString("%1").arg(path)<<"ret1"<<ret1<<" ";
    qDebug()<<"mkdir" <<QString("%1/%2").arg(path).arg(fileName)<<"ret2"<<ret2<<" ";
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
    qDebug()<<"删除文件:"<<delFilePath<<"文件类型"<<delFileType<<endl;
    bool ans=0;//记录删除结果，默认删除失败
    // REQ-FS-003②: 先校验后删除，顺序不可颠倒（removeRecursively 不可逆）
    if(!isPathInSandbox(delFilePath)){
        qWarning()<<"[deldir] 拒绝越权删除:"<<delFilePath;
    }
    else if(delFileType==1){//如果文件类型为普通文件
        QFile file(delFilePath);
        ans=file.remove();//删除文件
    }
    else if(delFileType==0){//如果文件类型为目录文件
        QDir dir(delFilePath);
        ans=dir.removeRecursively();//删除目录
    }

    // 跨功能联动：删除成功后，清理该文件（或目录下所有文件）对应的分享链接记录
    if(ans){
        // owner 直接使用登录名；relPath 以用户根目录为基准取相对路径，
        // 避免相对 filesPath 反推时把 filesys 误判为 owner 段
        QString userRoot = QDir(Server::getinstance().filesPath).absoluteFilePath(loginName);
        QString relPath = QDir(userRoot).relativeFilePath(QFileInfo(delFilePath).absoluteFilePath());
        if(!relPath.startsWith("..") && !relPath.startsWith("/") && !relPath.isEmpty()){
            OperateDB::instance().deleteShareLinkByPath(loginName, relPath);
            qDebug() << "[deldir] 已联动清理分享链接: owner=" << loginName << " relPath=" << relPath;
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
    qDebug()<<"tarPath"<<tarPath<<" ";
    QFileInfo FileInfoi(tarPath.toStdString().c_str());
    qDebug()<<tarPath.toStdString().c_str()<<" ";
    PDU* respdu=nullptr;
    bool isExist=FileInfoi.exists();
    // REQ-FS-003⑤: 沙箱外路径按"不存在"处理，不回显任意路径的存在性与名称
    if(isExist && !isPathInSandbox(tarPath)){
        isExist=false;
        qWarning()<<"[changedir] 拒绝越权路径探测:"<<tarPath;
    }
    qDebug()<<" isExist "<<isExist<<" ";
    //如果文件存在
    if(isExist){
        if(FileInfoi.isDir()){//如果是目录
            //目标目录长度
            int PathLen=tarPath.toStdString().size();
            respdu=mkPDU(PathLen+1);//创建pdu，长度为目标目录长度+1
            memcpy(respdu->caMsg,tarPath.toStdString().c_str(),PathLen);
            qDebug()<<" isdir "<<"tarPath"<<tarPath<<endl;
        }
        else if(FileInfoi.isFile()){//如果是文件
            int index=tarPath.lastIndexOf('/');
            tarPath=tarPath.remove(index,tarPath.size()-index);
            //目标目录长度
            int PathLen=tarPath.toStdString().size();
            respdu=mkPDU(PathLen+1);//创建pdu，长度为目标目录长度+1
            memcpy(respdu->caMsg,tarPath.toStdString().c_str(),PathLen);
            qDebug()<<" isFile "<<"tarPath"<<tarPath<<endl;
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
    if(!isPathInSandbox(path)){
        qWarning()<<"[flushfile] 拒绝越权列目录:"<<path;
        PDU* respdu=mkPDU();
        respdu->uiType=ENUM_MSG_TYPE_FLUSHFILE_RESPOND;
        return respdu;
    }
    QDir dir(path);
    QFileInfoList FileInfolist =dir.entryInfoList();
    PDU* respdu=mkPDU(sizeof(FileInfo)*(FileInfolist.size()-2));
    respdu->uiType=ENUM_MSG_TYPE_FLUSHFILE_RESPOND;
    for(int i=2;i<FileInfolist.size();i++){
        FileInfo* pFileInfo=(FileInfo*)respdu->caMsg+(i-2);
        memcpy(pFileInfo->caName,FileInfolist[i].fileName().toStdString().c_str(),32);
        if(FileInfolist[i].isDir()){//若是目录
            pFileInfo->uiType=0;
        }
        else {//若不是目录
            pFileInfo->uiType=1;
        }
        qDebug()<<"caName"<<pFileInfo->caName<<" uiType"<<pFileInfo->uiType<<" ";
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
    qDebug()<<"旧文件名:"<<oldFileName<<" "<<"新文件名:"<<newFileName<<" ";
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
    qDebug()<<"文件类型:"<<fileType<<" "<<"旧文件路径:"<<oldFilePath<<" "<<"新文件路径:"<<newFilePath<<" ";
    //进行文件或目录的重命名
    bool ret=1;
    // REQ-FS-003③: 先校验后重命名；未知 fileType 维持原状（ret=1 不执行改名）
    if(!isPathInSandbox(dirPath)){
        ret=0;
        qWarning()<<"[renamefile] 拒绝越权重命名目录:"<<dirPath;
    }
    else if(oldName.contains('/') || oldName.contains('\\')
            || newName.contains('/') || newName.contains('\\')){
        // F-1: 名称段含分隔符时 dirPath 校验可被 "dirPath/../x" 拼接绕过，一律拒绝
        ret=0;
        qWarning()<<"[renamefile] 拒绝越权重命名名称段:"<<oldName<<newName;
    }
    else if(fileType==0){//目录
        QDir dir;
        ret=dir.rename(oldFilePath,newFilePath);
    }
    else if(fileType==1){//文件
        QFile file;
        ret=file.rename(oldFilePath,newFilePath);
    }
    qDebug()<<"rename ret"<<ret<<endl;

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
            qDebug() << "[renamefile] 已联动更新分享链接: owner=" << loginName
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
    char seedName[32]={'\0'};
    memcpy(seedName,pdu->caData,32);
    int revUserCnt=0;
    memcpy(&revUserCnt,pdu->caData+32,sizeof (revUserCnt));
    qDebug()<<"seedName "<<seedName<<" revUserCnt "<<revUserCnt<<" ";
    PDU* resendpdu=mkPDU(pdu->uiMsgLen-revUserCnt*32);
    resendpdu->uiType=ENUM_MSG_TYPE_SHAREFILE_AGREE_REQUEST;
    memcpy(resendpdu->caData,seedName,32);
    memcpy(resendpdu->caMsg,pdu->caMsg+32*revUserCnt,resendpdu->uiMsgLen);
    qDebug()<<"resendpdu->caMsg"<<resendpdu->caMsg<<endl;
    for(int i=0;i<revUserCnt;i++){
        char revName[32]={'\0'};
        memcpy(revName,pdu->caMsg+32*i,32);
        qDebug()<<"revName"<<revName<<" ";
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
    QString sourcePath=QString::fromUtf8(pdu->caMsg);
    int indexi=sourcePath.lastIndexOf('/');
    QString fileName=sourcePath.right(sourcePath.size()-indexi-1);
    QString recvName=QString::fromUtf8(pdu->caData);
    // 目标接收者目录段同样客户端可控（../ 可逃逸），拼接方式维持原状
    QString recvDir=QString("%1/%2").arg(Server::getinstance().filesPath).arg(recvName);
    QString tarPath=QString("%1/%2/%3").arg(Server::getinstance().filesPath).arg(pdu->caData).arg(fileName);
    qDebug()<<"sourcePath:"<<sourcePath<<"tarPath："<<tarPath<<endl;
    // REQ-FS-003⑤: 源与目标目录都必须在沙箱内；目标文件名不得含反斜杠（Windows 下 "\.." 逃逸）
    bool ret=false;
    if(!isPathInSandbox(sourcePath)){
        qWarning()<<"[sharefilemove] 拒绝越权源路径:"<<sourcePath;
    } else if(!isPathInSandbox(recvDir)){
        qWarning()<<"[sharefilemove] 拒绝越权目标目录:"<<recvDir;
    } else if(fileName.contains('\\')){
        qWarning()<<"[sharefilemove] 拒绝含路径分隔符的目标名:"<<fileName;
    } else {
        ret=QFile::copy(sourcePath,tarPath);
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
                qDebug()<<"movefile 拒绝：不能把目录移进自身或其子目录";
            }
            // 原地移动（目标目录就是源所在目录）：无需操作
            else if(canonicalDstDir == srcInfo.dir().canonicalPath()){
                ret=true;
                qDebug()<<"movefile 原地移动，无需操作";
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
                    qDebug() << "文件移动重名，自动重命名为:" << finalDstPath;
                }

                //先尝试 rename；若跨文件系统失败则 copy+remove
                QFile srcFile(srcPath);
                ret = srcFile.rename(finalDstPath);
                if (!ret) {
                    qDebug() << "rename 失败, errorString:" << srcFile.errorString()
                             << "src:" << srcPath << "dst:" << finalDstPath;
                    // 降级策略：copy + remove
                    ret = QFile::copy(srcPath, finalDstPath);
                    if (!ret) {
                        qDebug() << "copy 降级也失败, src:" << srcPath << "dst:" << finalDstPath
                                 << "errno:" << errno << strerror(errno);
                    } else {
                        if (QFile::remove(srcPath)) {
                            qDebug() << "copy+remove 降级成功:" << finalDstPath;
                        } else {
                            qDebug() << "copy+remove 降级: copy 成功但 remove 失败, src:" << srcPath;
                            QFile::remove(finalDstPath);  // 清理不完整的目标文件
                            ret = false;
                        }
                    }
                }
            }
        } else {
            qDebug() << "movefile 目标路径校验失败: canonicalDstDir=" << canonicalDstDir;
        }
    } else {
        qDebug() << "movefile 源路径校验失败: canonicalSrc=" << canonicalSrc;
    }

    qDebug()<<"movefile src:"<<srcPath<<"dst:"<<dstPath<<"ret:"<<ret;

    // 跨功能联动：移动成功后，平移该文件的分享链接 relative_path
    if(ret){
        QString newRel = dstRel.isEmpty() ? fileName : (dstRel + "/" + fileName);
        OperateDB::instance().updateShareLinkPath(loginName, srcRel, newRel);
        qDebug() << "[movefile] 已联动更新分享链接: owner=" << loginName
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
        qDebug()<<"listdir rel="<<relPath<<" 子目录数="<<list.size();
    } else {
        qDebug()<<"listdir 越权或目录不存在: rel="<<relPath;
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
    // REQ-FS-003①: 校验 dirPath（所在目录，必定存在）；不对最终文件路径做 canonical（新文件尚不存在）。
    // begin() 内部的 fileName 校验保留，作为纵深防御第二层；不修改会话类接口。
    ResumableUploadSession::BeginResult result = {false, 0, false};
    if (isPathInSandbox(dirPath)) {
        result = m_uploadSession.begin(dirPath, fileName, fileSize, resumeEnabled);
    } else {
        qWarning() << "uploadfileinit 拒绝越权目录:" << dirPath;
    }

    bool ret=result.ok;
    qint64 resumeOffset=result.offset;
    qDebug()<<"uploadfileinit file"<<fileName
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
    qDebug()<<"uploadfiledata len"<<pdu->uiMsgLen
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

    if(isPathInSandbox(filePath)
            && fileInfo.isFile()
            && !fileName.isEmpty()){
        fileSize=fileInfo.size();
        ret=true;
        m_downloadCanonicalPath=canonicalFilePath;
    }

    //仅返回元数据 PDU（不含文件内容），实际数据由 MyTcpSocket 流式发送
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND;
    memcpy(respdu->caData,&ret,sizeof(ret));
    memcpy(respdu->caData+sizeof(bool),&fileSize,sizeof(fileSize));
    QByteArray fileNameBytes=fileName.toUtf8();
    memcpy(respdu->caData+32,fileNameBytes.constData(),qMin(fileNameBytes.size(),31));
    qDebug()<<"downloadfile validate path"<<filePath<<"ret"<<ret<<"size"<<fileSize;
    return respdu;
}

//获取聊天信息
PDU *MsgHandler::chatmsg()
{
    char seedName[32]={'\0'};
    char revName[32]={'\0'};
    memcpy(revName,pdu->caData,32);
    memcpy(seedName,pdu->caData+32,32);
    QStringList Msglist= OperateDB::instance().getchatMsg(seedName,revName);
    qDebug()<<"chatmsg msglist.size"<<Msglist.size()<<" ";
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
        qDebug()<<"filei->msg "<<filei->msg<<" filei->Time"<<filei->Time<<",";
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
    qDebug()<<"getuserinformation userName:"<<userName<<"found:"<<found<<"online:"<<outOnline;
    return respdu;
}

PDU* MsgHandler::createPublishLink(const QString& loginName) {
    QString relativePath = fixedString(pdu->caMsg, pdu->uiMsgLen);
    QString ownerName = loginName;
    qDebug() << "[createPublishLink] 收到生成请求: owner=" << ownerName << " relativePath=" << relativePath;

    QString token = ShareLinkService::generateToken();
    QString hash = ShareLinkService::hashToken(token);
    QDateTime expire = ShareLinkService::defaultExpireTime();

    bool ok = OperateDB::instance().createShareLink(hash, ownerName, relativePath, expire);
    if(!ok){
        qDebug() << "[createPublishLink] 写入数据库失败，生成分享链接未完成";
    }

    PDU* respdu = mkPDU(128);
    respdu->uiType = ENUM_MSG_TYPE_CREATE_PUBLINK_RESPOND;
    memcpy(respdu->caData, &ok, sizeof(ok));
    if(ok) {
        QString link = QString("cloud-disk://share/%1").arg(token);
        QByteArray linkBytes = link.toUtf8();
        memcpy(respdu->caMsg, linkBytes.constData(), linkBytes.size());
    }
    return respdu;
}



// 新增：安全外链分享（BUG-ARC-PUBLINK-001）
PDU* MsgHandler::resolvePublishLink(const QString& loginName) {
    QString token = fixedString(pdu->caMsg, pdu->uiMsgLen);
    qDebug() << "[resolvePublishLink] 收到解析请求: token_len=" << token.size()
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
    qDebug() << "[resolvePublishLink] hash 来源:" << (isHex64 ? "直接输入(64hex)" : "hashToken(输入)");

    OperateDB::ShareLinkInfo info;
    PDU* respdu = mkPDU(1024); // 预留 JSON 空间
    respdu->uiType = ENUM_MSG_TYPE_RESOLVE_PUBLINK_RESPOND;

    bool ok = false;
    QString savedRelPath; // 形如 "分享链接/2.pdf"，返回给客户端展示
    if (OperateDB::instance().getShareLinkByHash(hash, info)) {
        qDebug() << "[resolvePublishLink] 命中记录: owner=" << info.ownerName
                 << " relativePath=" << info.relativePath
                 << " revoked=" << info.revoked
                 << " expireTime=" << info.expireTime.toString("yyyy-MM-dd HH:mm:ss");
        if (info.revoked) {
            qDebug() << "[resolvePublishLink] 链接已撤销，拒绝";
        } else if (QDateTime::currentDateTime() >= info.expireTime) {
            qDebug() << "[resolvePublishLink] 链接已过期，拒绝";
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
                        qDebug() << "[resolvePublishLink] 已复制分享文件到:" << finalDstPath;
                    } else {
                        qDebug() << "[resolvePublishLink] 复制失败:" << srcFile.errorString()
                                 << " src=" << srcPath << " dst=" << finalDstPath;
                    }
                } else {
                    qDebug() << "[resolvePublishLink] 同名文件过多，放弃复制";
                }
            } else {
                qDebug() << "[resolvePublishLink] 源文件不存在或越权: " << srcPath;
            }
        }
    } else {
        qDebug() << "[resolvePublishLink] 未找到匹配记录 (hash_head="
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
    QString relativePath = fixedString(pdu->caMsg, pdu->uiMsgLen);
    QString ownerName = fixedString(pdu->caData, 32); 
    
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
