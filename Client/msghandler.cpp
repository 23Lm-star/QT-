#include "client.h"
#include "msghandler.h"
#include <QString>
#include <QMessageBox>
#include <qmessagebox.h>
#include <qdir.h>
#include <QDate>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include "index.h"
#include "userinformation.h"
MsgHandler::MsgHandler()
{
    m_recvFile=nullptr;
}

void MsgHandler::handlrigist(){
    int ret;
    memcpy(&ret,this->pdu->caData,sizeof(ret));
    qDebug()<<"rigist ret"<<ret<<endl;
    char rigistName[32]={'\0'};
    memcpy(rigistName,this->pdu->caData+32,32);
    qDebug()<<QString(rigistName)<<endl;
    if(ret==-2){
        QMessageBox::information(&Client::getinstance(),"提示","验证码错误，请重新输入");
        return;
    }
    if(ret==-3){
        QMessageBox::information(&Client::getinstance(),"提示","验证码已过期，请重新获取");
        return;
    }
    if(ret){
        Client::getinstance().showLoadingAnimation(true);
        QString name=QString(rigistName);
        QTimer::singleShot(1500,[name](){
            QMessageBox::information(&Client::getinstance(),"提示","注册成功");
            Client::getinstance().flus();
            QDir dir;
            dir.mkdir(QString("%1").arg(Client::getinstance().m_strRootPath));
            dir.mkdir(QString("%1/%2").arg(Client::getinstance().m_strRootPath).arg(name));
        });
    }
    else {
        Client::getinstance().showLoadingAnimation(false);
    }
}

void MsgHandler::handllogin()
{
    int ret;
    memcpy(&ret,pdu->caData,sizeof(ret));
    if(ret==-2){
        QMessageBox::information(&Client::getinstance(),"提示","验证码错误，请重新输入");
        return;
    }
    if(ret==-3){
        QMessageBox::information(&Client::getinstance(),"提示","验证码已过期，请重新获取");
        return;
    }
    if(ret==1){
        Client::getinstance().showLoadingAnimation(true);
        char lName[32]={'\0'};
        memcpy(lName,pdu->caData+32,sizeof(lName));
        QString loginName=QString(lName);
        QTimer::singleShot(1500,[loginName](){
            Client::getinstance().m_LoginName=loginName;
            qDebug()<<"登录账号成功，用户名为:"<<loginName;
            Index::getinstance().show();
            Client::getinstance().hide();
            // 主界面完全构造并显示后，在事件循环下一轮触发初始刷新（仅好友列表）
            QTimer::singleShot(0, [](){
                Index::getinstance().getFriend()->flushfriend();
                // 文件列表刷新延迟到用户首次点击"文件"页时
            });
        });
    }
    else if(ret==-1){
        Client::getinstance().showLoadingAnimation(false);
        QTimer::singleShot(1500,[](){
            QMessageBox::information(&Client::getinstance(),"提示","用户或密码其中一个错误");
        });
    }
    else{
        Client::getinstance().showLoadingAnimation(false);
        QTimer::singleShot(1500,[](){
            QMessageBox::information(&Client::getinstance(),"提示","状态异常");
        });
    }
}

void MsgHandler::handlonlineuser(){
      //取出所有在线用户的用户名
      int leng=pdu->uiMsgLen/32;//柔性数组长度
      QStringList userNamelist;
      for(int i=0;i<leng;i++){
          char userName[33]={'\0'};
          memcpy(userName,pdu->caMsg+i*32,32);//取出每一个用户的用户名
          if(strcmp(userName,Client::getinstance().m_LoginName.toStdString().c_str())==0){
              continue;
          }
          userNamelist.append(QString("%1.%2").arg(i+1).arg(userName));//将编号还有用户名都统一加进去
      }
      Index::getinstance().getFriend()->m_onlineUser->updateOnlineUser(userNamelist);
}

void MsgHandler::handlAddFriend()
{
    int ret=0;
    memcpy(&ret,pdu->caData,sizeof (ret));
    qDebug()<<"AddFriend ans ret"<<ret<<endl;
    if(ret==1){
        Index::getinstance().getFriend()->flushfriend();
    }
    else if(ret==-2){
        QMessageBox::information(Index::getinstance().getFriend()->m_onlineUser,"提示","该用户不在线");
    }
    else {
        QMessageBox::information(Index::getinstance().getFriend()->m_onlineUser,"提示","添加失败");
    }
}
//同意添加好友
void MsgHandler::handlAddFriendAgree(const QString &myName)
{
    char seedName[32]={'\0'};
    memcpy(seedName,pdu->caData,32);
    int ret=QMessageBox::question(Index::getinstance().getFriend(),"提示",QString("你同意%1的好友申请吗?").arg(seedName));
    if(ret!=QMessageBox::Yes){
        return;
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_ADDFRIEND_RESPOND;
    memcpy(pdu->caData,myName.toStdString().c_str(),sizeof(myName.toStdString()));
    memcpy(pdu->caData+32,&seedName,sizeof (seedName));
    Client::getinstance().sorket.write((char*)pdu,pdu->uiTotalLen);
}

void MsgHandler::handlFlushFriend()
{
    int leng=pdu->uiMsgLen/32;//柔性数组长度
    QStringList userNamelist;
    for(int i=0;i<leng;i++){
        char userName[32]={'\0'};
        memcpy(userName,pdu->caMsg+i*32,32);//取出每一个用户的用户名
        userNamelist.append(QString("%1.%2").arg(i+1).arg(userName));//将编号还有用户名都统一加进去
    }
    Index::getinstance().getFriend()->flushFriend_LW(userNamelist);
}
//删除好友
void MsgHandler::handlDeleteFriend()
{
    bool ret=0;
    memcpy(&ret,pdu->caData,sizeof(ret));
    if(ret){
        Index::getinstance().getFriend()->flushfriend();
    }
    else {
        QMessageBox::information(Index::getinstance().getFriend(),"提示","删除好友失败");
    }
}


//聊天
void MsgHandler::chat()
{
    char seedName[32]={'\0'};
    memcpy(seedName,pdu->caData,32);
    Msg* pdu_Msg=(Msg*)pdu->caMsg;
    char msg[110]={'\0'};
    memcpy(msg,pdu_Msg->msg,pdu_Msg->msgLen);
    char currtime[30]={'\0'};
    memcpy(currtime,pdu_Msg->Time,30);
    Index::getinstance().getFriend()->m_chat->updateShow_TE(QString("%1:%2 发送时间:%3").arg(seedName).arg(msg).arg(currtime));
    Index::getinstance().getFriend()->m_chat->m_strChatName=seedName;
    if(Index::getinstance().getFriend()->m_chat->isHidden()){
        Index::getinstance().getFriend()->m_chat->show();
        Index::getinstance().getFriend()->m_chat->setWindowTitle(seedName);
    }
}
//获取所有未读消息
void MsgHandler::getcharmsg()
{
        QStringList Msglist;
        //记录偏移还有是否还有位置没有读取
        for(int dis=0;dis<pdu->uiMsgLen;){
            Msg* respdu_Msg=(Msg*)(pdu->caMsg+dis);
            char msg[110]={'\0'};
            char currTime[30]={'\0'};
            memcpy(msg,respdu_Msg->msg,respdu_Msg->msgLen);
            memcpy(currTime,respdu_Msg->Time,30);
            Msglist.append(QString(msg));
            Msglist.append(QString(currTime));
            qDebug()<<"getmsg msg:"<<QString(msg)<<" currTime:"<<QString(currTime)<<"  ";
            dis+=respdu_Msg->totalLen;
        }
        // 更新界面
        Index::getinstance().getFriend()->m_chat->updateShow_TEs(Msglist);
}
//创建目录
void MsgHandler::mkdir()
{
    bool ret=1;
    memcpy(&ret,pdu->caData,sizeof (ret));
    qDebug()<<"mkdir ret"<<ret<<endl;
    if(ret){
        QMessageBox::information(&Index::getinstance(),"提示","创建文件夹成功");
        Index::getinstance().getFile()->flushfile();
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示","创建文件夹失败");
    }
}

void MsgHandler::deldir()
{
    bool ret=0;
    memcpy(&ret,pdu->caData,sizeof(ret));
    qDebug()<<"删除文件结果"<<ret<<endl;
    if(ret){
        QMessageBox::information(&Index::getinstance(),"提示","删除文件夹成功");
        PDU* pdu=mkPDU(Index::getinstance().getFile()->m_strCurPath.size()+1);
        pdu->uiType=ENUM_MSG_TYPE_FLUSHFILE_REQUEST;
        memcpy(pdu->caMsg,Index::getinstance().getFile()->m_strCurPath.toStdString().c_str(),Index::getinstance().getFile()->m_strCurPath.size());
        Client::getinstance().seedMsg(pdu);
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示","删除文件夹失败");
    }
}

void MsgHandler::changedir()
{
    bool ret=0;
    memcpy(&ret,pdu->caData,sizeof(ret));
    if(ret){//如果路径存在
        Index::getinstance().getFile()->m_strCurPath=pdu->caMsg;
        Index::getinstance().getFile()->flushfile();
    }
    else {//如果路径不存在
        QMessageBox::information(&Index::getinstance(),"提示","路径不存在");
        Index::getinstance().getFile()->flushCurrLE();
    }
}
//重命名文件
void MsgHandler::renamedir()
{
    bool ret=0;
    memcpy(&ret,pdu->caData,sizeof (ret));
    qDebug()<<"rename ret"<<ret<<endl;
    if(ret){
        QMessageBox::information(&Index::getinstance(),"提示","文件重命名成功");
        PDU* pdu=mkPDU(Index::getinstance().getFile()->m_strCurPath.toUtf8().toStdString().size()+1);
        pdu->uiType=ENUM_MSG_TYPE_FLUSHFILE_REQUEST;
        memcpy(pdu->caMsg,Index::getinstance().getFile()->m_strCurPath.toStdString().c_str(),Index::getinstance().getFile()->m_strCurPath.toUtf8().toStdString().size());
        Client::getinstance().seedMsg(pdu);
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示","文件重命名失败");
    }
}

void MsgHandler::sharefileagree()
{
    char seedName[32]={'\0'};
    memcpy(seedName,pdu->caData,32);
    QString filePath=pdu->caMsg;
    int index=filePath.lastIndexOf('/');
    QString fileName=filePath.right(filePath.size()-index-1);
    qDebug()<<"接收到seedName:"<<seedName<<"的文件fileName "<<fileName<<" ";
    int ret=QMessageBox::question(&Index::getinstance(),"分享文件请求",QString("你是否同意接受%1的文件%2").arg(seedName).arg(fileName));
    qDebug()<<" ret="<<ret<<endl;
    if(ret!=QMessageBox::Yes){
        return ;
    }
    PDU* respdu=mkPDU(pdu->uiMsgLen);
    respdu->uiType=ENUM_MSG_TYPE_SHAREFILE_AGREE_RESPOND;
    memcpy(respdu->caData,Client::getinstance().m_LoginName.toStdString().c_str(),32);
    memcpy(respdu->caMsg,pdu->caMsg,respdu->uiMsgLen);
    Client::getinstance().seedMsg(respdu);
}

void MsgHandler::sharefilefinsh()
{
    bool ret=0;
    memcpy(&ret,pdu->caData,sizeof (ret));
    qDebug()<<"sharefilefinsh ret"<<ret<<endl;
    if(ret){
        QMessageBox::information(&Index::getinstance(),"提示","接收分享文件成功");
        Index::getinstance().getFile()->flushfile();//刷新一下文件
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示","接收分享文件失败");
    }
}

void MsgHandler::uploadfileinit()
{
    bool ret=0;
    qint64 resumeOffset=0;
    uint resumeMagic=0;
    memcpy(&ret,pdu->caData,sizeof (ret));
    memcpy(&resumeMagic,pdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,sizeof(resumeMagic));
    if(resumeMagic == UPLOAD_RESUME_MAGIC){
        memcpy(&resumeOffset,pdu->caData+UPLOAD_RESUME_OFFSET,sizeof(resumeOffset));
    }
    qDebug()<<"uploadfileinit ret"<<ret<<" resumeOffset"<<resumeOffset<<endl;
    if(ret){
        QFile localFile(Index::getinstance().getFile()->m_strUploadPath);
        if(resumeMagic == UPLOAD_RESUME_MAGIC && localFile.exists() && resumeOffset >= localFile.size()){
            Index::getinstance().getFile()->flushfile();
            return;
        }
        Index::getinstance().getFile()->uploadfile(resumeOffset);
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示","上传文件失败");
    }
}

void MsgHandler::uploadfiledata()
{
    bool ret=true;
    uint resumeMagic=0;
    memcpy(&resumeMagic,pdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,sizeof(resumeMagic));
    if(resumeMagic == UPLOAD_RESUME_MAGIC){
        memcpy(&ret,pdu->caData,sizeof(ret));
    }
    if(!ret){
        QMessageBox::information(&Index::getinstance(),"提示","上传文件失败");
        return;
    }
    Index::getinstance().getFile()->flushfile();
}

void MsgHandler::flushfile()
{
    QList<FileInfo*>qFileinfolist;
    int len=pdu->uiMsgLen/sizeof(FileInfo);
    for(int i=0;i<len;i++){
        FileInfo* newFileInfo=new FileInfo;
        memcpy(newFileInfo,pdu->caMsg+i*sizeof (FileInfo),sizeof(FileInfo));
        qFileinfolist.append(newFileInfo);
        qDebug()<<"newFileInfo->caName"<<newFileInfo->caName<<" newFileInfo->uiType"<<newFileInfo->uiType<<" ";
    }
    Index::getinstance().getFile()->updateCurrfile(qFileinfolist);
}

void MsgHandler::downloadfile()
{
    //旧版兼容：若 caMsg 有数据则按原逻辑处理（单次下载完成）
    if(pdu->uiMsgLen > 0){
        bool ret=false;
        memcpy(&ret,pdu->caData,sizeof(ret));
        QString fileName=QString::fromUtf8(pdu->caData+32);
        if(fileName.isEmpty()){
            fileName="download.bin";
        }
        if(!ret){
            QMessageBox::information(&Index::getinstance(),"提示","下载文件失败");
            return;
        }
        QString monthDirPath=QString("%1/%2")
                .arg(Index::getinstance().getFile()->m_strUserPath)
                .arg(QDate::currentDate().toString("yyyy-MM"));
        QDir dir;
        if(!dir.mkpath(monthDirPath)){
            QMessageBox::information(&Index::getinstance(),"提示","创建下载目录失败");
            return;
        }
        QString savePath=QDir(monthDirPath).filePath(fileName);
        QFileInfo saveInfo(savePath);
        QString baseName=saveInfo.completeBaseName();
        QString suffix=saveInfo.suffix();
        int index=1;
        while(QFileInfo::exists(savePath)){
            QString candidate=suffix.isEmpty()
                    ? QString("%1(%2)").arg(baseName).arg(index)
                    : QString("%1(%2).%3").arg(baseName).arg(index).arg(suffix);
            savePath=QDir(monthDirPath).filePath(candidate);
            index++;
        }
        QFile file(savePath);
        if(!file.open(QIODevice::WriteOnly)){
            QMessageBox::information(&Index::getinstance(),"提示","保存下载文件失败");
            return;
        }
        qint64 writeCount=file.write(pdu->caMsg,pdu->uiMsgLen);
        file.close();
        if(writeCount!=pdu->uiMsgLen){
            QFile::remove(savePath);
            QMessageBox::information(&Index::getinstance(),"提示","保存下载文件失败");
            return;
        }
        QMessageBox::information(&Index::getinstance(),"提示",QString("下载完成：%1").arg(savePath));
        return;
    }
    //新流式分块模式：RESPOND 仅含元数据，后续 DATA/FINISH 由独立 handler 处理
    handleDownloadRespond();
}

void MsgHandler::handleDownloadRespond()
{
    bool ret=false;
    qint64 fileSize=0;
    memcpy(&ret,pdu->caData,sizeof(ret));
    memcpy(&fileSize,pdu->caData+sizeof(bool),sizeof(fileSize));
    QString fileName=QString::fromUtf8(pdu->caData+32);
    if(fileName.isEmpty()){
        fileName="download.bin";
    }
    m_recvFileName=fileName;

    if(!ret){
        QMessageBox::information(&Index::getinstance(),"提示","服务端拒绝下载请求");
        return;
    }

    QString monthDirPath=QString("%1/%2")
            .arg(Index::getinstance().getFile()->m_strUserPath)
            .arg(QDate::currentDate().toString("yyyy-MM"));
    QDir dir;
    if(!dir.mkpath(monthDirPath)){
        QMessageBox::information(&Index::getinstance(),"提示","创建下载目录失败");
        return;
    }

    QString savePath=QDir(monthDirPath).filePath(fileName);
    QFileInfo saveInfo(savePath);
    QString baseName=saveInfo.completeBaseName();
    QString suffix=saveInfo.suffix();
    int index=1;
    while(QFileInfo::exists(savePath)){
        QString candidate=suffix.isEmpty()
                ? QString("%1(%2)").arg(baseName).arg(index)
                : QString("%1(%2).%3").arg(baseName).arg(index).arg(suffix);
        savePath=QDir(monthDirPath).filePath(candidate);
        index++;
    }

    //清理已有的接收文件
    if(m_recvFile){
        m_recvFile->close();
        delete m_recvFile;
        m_recvFile=nullptr;
    }

    m_recvFile=new QFile(savePath);
    if(!m_recvFile->open(QIODevice::WriteOnly)){
        QMessageBox::information(&Index::getinstance(),"提示","无法创建下载文件");
        delete m_recvFile;
        m_recvFile=nullptr;
        return;
    }
    m_recvFilePath=savePath;
    qDebug()<<"下载准备就绪:"<<savePath<<"预期大小:"<<fileSize;
}

void MsgHandler::handleDownloadData()
{
    if(!m_recvFile || !m_recvFile->isOpen()){
        qDebug()<<"下载数据块到达但文件未打开";
        return;
    }
    qint64 written=m_recvFile->write(pdu->caMsg,pdu->uiMsgLen);
    if(written != (qint64)pdu->uiMsgLen){
        qDebug()<<"写入下载数据块失败";
    }
}

void MsgHandler::handleDownloadFinish()
{
    bool success=false;
    memcpy(&success,pdu->caData,sizeof(success));

    if(m_recvFile){
        m_recvFile->close();
        delete m_recvFile;
        m_recvFile=nullptr;
    }

    if(success){
        QMessageBox::information(&Index::getinstance(),"提示",QString("下载完成：%1").arg(m_recvFilePath));
    }
    else {
        if(!m_recvFilePath.isEmpty()){
            QFile::remove(m_recvFilePath);
        }
        QMessageBox::information(&Index::getinstance(),"提示","下载文件失败");
    }
    m_recvFilePath.clear();
    m_recvFileName.clear();
}

void MsgHandler::handlfinduser(){
    int ret;
    memcpy(&ret,pdu->caData,sizeof(ret));
    qDebug()<<"client finduser ret"<<ret<<endl;
    if(ret==2){
        QMessageBox::information(&Client::getinstance(),"提示","该用户不存在");
    }
    else if(ret==1){
        QMessageBox::information(&Client::getinstance(),"提示","该用户在线");
    }
    else if(ret==0){
       QMessageBox::information(&Client::getinstance(),"提示","该用户不在线");
    }
}

void MsgHandler::handleCaptcha(){
    QByteArray codeBytes(pdu->caMsg,pdu->uiMsgLen);
    QString code=QString::fromUtf8(codeBytes).trimmed();
    qDebug()<<"收到服务端验证码:"<<code;
    Client::getinstance().updateCaptchaDisplay(code);
    Client::getinstance().startCaptchaCooldown();
}

void MsgHandler::handlMoveFile()
{
    bool ret=false;
    memcpy(&ret,pdu->caData,sizeof(ret));
    char fileName[32]={'\0'};
    memcpy(fileName,pdu->caData+32,32);
    qDebug()<<"movefile ret:"<<ret<<" fileName:"<<QString::fromUtf8(fileName);
    if(ret){
        QMessageBox::information(&Index::getinstance(),"提示",QString("文件%1移动成功").arg(QString::fromUtf8(fileName)));
        Index::getinstance().getFile()->flushfile();
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示","文件移动失败");
    }
}

void MsgHandler::handlUserInformation()
{
    int resultCode=0;
    int online=0;
    memcpy(&resultCode,pdu->caData,sizeof(resultCode));
    memcpy(&online,pdu->caData+sizeof(int),sizeof(online));

    char userName[32]={'\0'};
    memcpy(userName,pdu->caData+32,32);

    qDebug()<<"handlUserInformation result:"<<resultCode<<"online:"<<online<<"name:"<<QString::fromUtf8(userName);

    if(resultCode!=0){
        QMessageBox::information(&Index::getinstance(),"提示","用户信息获取失败");
        return;
    }

    //构建最小信息列表，仅填充用户名和在线状态，其余显示"未设置/暂无资料"
    QStringList msg;
    msg.append(QString::fromUtf8(userName));       //0: 用户名
    msg.append("未设置");                            //1: 昵称
    msg.append("0");                                //2: 年龄
    msg.append("0");                                //3: 性别
    msg.append("未设置");                            //4: 省
    msg.append("未设置");                            //5: 市
    msg.append("未设置");                            //6: 县
    msg.append("未设置");                            //7: 职业
    msg.append("未设置");                            //8-11: 学校
    msg.append("未设置");
    msg.append("未设置");
    msg.append("未设置");
    msg.append(online ? "在线" : "离线");            //12: 使用签名字段显示在线状态
    UserInformation::getinstance().fluMsg(msg);
}

void MsgHandler::handlLogoff()
{
    bool ret=false;
    memcpy(&ret,pdu->caData,sizeof(ret));
    qDebug()<<"handlLogoff result:"<<ret;
    if(ret){
        QMessageBox::information(&Index::getinstance(),"提示","账户已成功注销，即将返回登录界面。");
        // 关闭主界面，返回登录界面
        Index::getinstance().hide();
        Client::getinstance().show();
        Client::getinstance().flus();
        // 断开连接
        Client::getinstance().sorket.disconnectFromHost();
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示","注销失败：密码错误或用户不存在。");
    }
}
