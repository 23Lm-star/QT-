#include "msghandler.h"
#include "QDebug"
#include "operatedb.h"
#include "mytcpserver.h"
#include "server.h"
#include <QByteArray>
#include <QDir>

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
}

//处理注册请求
PDU *MsgHandler::rigist()
{
    //取出用户名和密码
    char strname[32]={'\0'};
    char strpwd[32]={'\0'};
    memcpy(strname,pdu->caData,32);
    memcpy(strpwd,pdu->caData+32,32);
    //利用数据库操作对象进行数据库的注册操作,返回注册结果并输出
    bool ret=OperateDB::instance().handleRegit(strname,strpwd);
    qDebug()<<"regist ret "<<ret<<endl;
    if(ret){
        QDir dir;
        bool ret1=dir.mkdir(QString("%1").arg(Server::getinstance().filesPath));
        bool ret2=dir.mkdir(QString("%1/%2").arg(Server::getinstance().filesPath).arg(strname));
        qDebug()<<"创建用户目录:"<<"ret1 "<<ret1<<" "<<QString("%1").arg(Server::getinstance().filesPath)<<endl;
        qDebug()<<"创建用户目录:"<<"ret2 "<<ret2<<" "<<QString("%1/%2").arg(Server::getinstance().filesPath).arg(strname)<<endl;
    }
    //将 注册结果 打包进响应报文返回
    PDU* respdu=mkPDU();
    memcpy(respdu->caData,&ret,sizeof(ret));
    memcpy(respdu->caData+32,strname,32);
    respdu->uiType=ENUM_MSG_TYPE_REGIST_RESPOND;
    return respdu;
}
//处理登录请求，同时需要更新socket对象的 客户 用户名
PDU *MsgHandler::login(QString &strname)
{
    //取出用户名和密码
    char username[32]={'\0'};
    char userpwd[32]={'\0'};
    memcpy(username,pdu->caData,32);
    memcpy(userpwd,pdu->caData+32,32);
    //在数据库中 检查用户名和密码是否一致或者有问题
    int ret=OperateDB::instance().handleLogin(username,userpwd);
    qDebug()<<"login ret"<<ret<<endl;
    if(ret==1){//记录已登录的用户名
        strname=username;
    }
    //将登录结果打包进响应报文并返回
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_LOGIN_RESPOND;
    memcpy(respdu->caData,&ret,sizeof(ret));
    memcpy(respdu->caData+32,username,sizeof(username));
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
    //在数据库中 检查用户名和密码是否一致或者有问题
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
        MyTcpServer::getinstance().seedAddFriend(seedName,pdu);
    }
    return respdu;
}

PDU *MsgHandler::addfriendFin()
{
    char agreeName[32]={'\0'};
    char seedName[32]={'\0'};
    memcpy(agreeName,pdu->caData,32);
    memcpy(seedName,pdu->caData+32,32);
    //在数据库中 检查用户名和密码是否一致或者有问题
    int ret=OperateDB::instance().handleAddFriendFin(seedName,agreeName);
    qDebug()<<" addfriendFin ret"<<ret<<endl;
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_ADDFRIEND_RESPOND;
    memcpy(pdu->caData,&ret,sizeof(ret));
    MyTcpServer::getinstance().seedAddFriend(seedName,pdu);
    MyTcpServer::getinstance().seedAddFriend(agreeName,pdu);
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
    qDebug()<<"接收到的msg"<<QString(msg)<<" 时间为:"<<QString(currTime)<<endl;
    bool ret= OperateDB::instance().seedchatMsg(seedName,tarName,QString(msg),QString(currTime));
    qDebug()<<"char ret"<<ret<<endl;
    if(ret){
        MyTcpServer::getinstance().reseed(tarName,pdu);//转发给接收人
    }
    return nullptr;
}
//创建目录
PDU *MsgHandler::MKDIR(){
    //获取目录名和路径
    char fileName[32]={'\0'};
    QString path(QString::fromUtf8(pdu->caMsg));
    memcpy(fileName,pdu->caData,32);
    //构建QDir对象，创建目录，分两步
    QDir dir;
    bool ret1=dir.mkdir(QString("%1").arg(path));
    bool ret2=dir.mkdir(QString("%1/%2").arg(path).arg(fileName));
    qDebug()<<"mkdir" <<QString("%1").arg(path)<<"ret1"<<ret1<<" ";
    qDebug()<<"mkdir" <<QString("%1/%2").arg(path).arg(fileName)<<"ret2"<<ret2<<" ";
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_MKDIR_RESPOND;
    memcpy(respdu->caData,&ret2,sizeof(ret2));
    return respdu;
}
//删除目录
PDU *MsgHandler::deldir()
{
    //获取删除文件路径和文件类型
    QString delFilePath(QString::fromUtf8(pdu->caMsg));
    uint delFileType=0;
    memcpy(&delFileType,pdu->caData,sizeof(uint));
    qDebug()<<"删除文件:"<<delFilePath<<"文件类型"<<delFileType<<endl;
    bool ans=0;//记录删除结果，默认删除失败
    if(delFileType==1){//如果文件类型为普通文件
        QFile file(delFilePath);
        ans=file.remove();//删除文件
    }
    else if(delFileType==0){//如果文件类型为目录文件
        QDir dir(delFilePath);
        ans=dir.removeRecursively();//删除目录
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
    QDir dir(QString::fromUtf8(pdu->caMsg));
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
PDU *MsgHandler::renamefile()
{
    //拼接旧文件路径和新文件路径
    char oldFileName[32]={'\0'};
    char newFileName[32]={'\0'};
    memcpy(oldFileName,pdu->caData,32);
    memcpy(newFileName,pdu->caData+32,32);
    qDebug()<<"旧文件名:"<<oldFileName<<" "<<"新文件名:"<<newFileName<<" ";
    QString oldFilePath(QString::fromUtf8(pdu->caMsg+sizeof(uint))+"/"+QString::fromUtf8(oldFileName));
    QString newFilePath(QString::fromUtf8(pdu->caMsg+sizeof(uint))+"/"+QString::fromUtf8(newFileName));
    uint fileType=0;
    memcpy(&fileType,pdu->caMsg,sizeof(fileType));
    qDebug()<<"文件类型:"<<fileType<<" "<<"旧文件路径"<<oldFilePath<<" "<<"新文件路径"<<newFilePath<<" ";
    //进行文件或目录的重命名
    bool ret=1;
    if(fileType==0){//目录
        QDir dir;
        ret=dir.rename(oldFilePath,newFilePath);
    }
    else if(fileType==1){//文件
        QFile file;
        ret=file.rename(oldFilePath,newFilePath);
    }
    qDebug()<<"rename ret"<<ret<<endl;
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
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_SHAREFILE_RESPOND;
    return respdu;
}
//分享文件（移动）
PDU *MsgHandler::sharefilemove()
{
    QString sourcePath=pdu->caMsg;
    int indexi=sourcePath.lastIndexOf('/');
    QString fileName=sourcePath.right(sourcePath.size()-indexi-1);
    QString tarPath=QString("%1/%2/%3").arg(Server::getinstance().filesPath).arg(pdu->caData).arg(fileName);
    qDebug()<<"sourcePath:"<<sourcePath<<"tarPath："<<tarPath<<endl;
    bool ret=QFile::copy(sourcePath,tarPath);
    PDU* respdu=mkPDU();
    memcpy(respdu->caData,&ret,sizeof (ret));
    respdu->uiType=ENUM_MSG_TYPE_SHAREFILE_FINSH_RESPOND;
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
    ResumableUploadSession::BeginResult result =
            m_uploadSession.begin(dirPath, fileName, fileSize, resumeEnabled);

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

    bool ret=m_uploadSession.append(pdu->caMsg,pdu->uiMsgLen);
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
        filei->totoalLen=sizeof(Msg)+Msglist[i].toUtf8().size()+1;
        filei->msgLen=Msglist[i].toUtf8().size()+1;
        memcpy(filei->Time,Msglist[i+1].toUtf8().toStdString().c_str(),30);
        memcpy(filei->msg,Msglist[i].toUtf8().toStdString().c_str(),filei->msgLen);
        qDebug()<<"filei->msg "<<filei->msg<<" filei->Time"<<filei->Time<<",";
        dis+=filei->totoalLen;
    }
    return respdu;
}
