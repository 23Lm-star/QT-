#include "client.h"
#include "msghandler.h"
#include <QString>
#include <QMessageBox>
#include <qmessagebox.h>
#include <qdir.h>
#include <QFile>
#include "index.h"
MsgHandler::MsgHandler()
{

}

void MsgHandler::handlrigist(){
    bool ret;
    memcpy(&ret,this->pdu->caData,sizeof(ret));
    qDebug()<<"rigist ret"<<ret<<endl;
    char rigistName[32]={'\0'};
    memcpy(rigistName,this->pdu->caData+32,32);
    qDebug()<<QString(rigistName)<<endl;
    if(ret){
        QMessageBox::information(&Client::getinstance(),"提示","注册成功");
        Client::getinstance().flus();
        QDir dir;
        bool ret1=dir.mkdir(QString("%1").arg(Client::getinstance().m_strRootPath));
        bool ret2=dir.mkdir(QString("%1/%2").arg(Client::getinstance().m_strRootPath).arg(rigistName));
        qDebug()<<"创建用户目录 :ret1"<<ret1<<" "<<QString("%1").arg(Client::getinstance().m_strRootPath);
        qDebug()<<"创建用户目录 :ret2"<<ret2<<" "<<QString("%1/%2").arg(Client::getinstance().m_strRootPath).arg(rigistName);
    }
    else {
        QMessageBox::information(&Client::getinstance(),"提示","注册失败");
    }
}

void MsgHandler::handllogin()
{
    int ret;
    memcpy(&ret,pdu->caData,sizeof(ret));
    if(ret==1){
        QMessageBox::information(&Client::getinstance(),"提示","登录成功");
        char lName[32]={'\0'};
        memcpy(lName,pdu->caData+32,sizeof(lName));
        Client::getinstance().m_LoginName=QString(lName);
        qDebug()<<"登录账号成功，用户名为:"<<Client::getinstance().m_LoginName<<endl;
        //展示索引界面（进入内部界面）
        Index::getinstance().show();
        //隐藏原界面
        Client::getinstance().hide();
    }
    else if(ret==-1){
        QMessageBox::information(&Client::getinstance(),"提示","用户或密码其中一个错误");
    }
    else{
        QMessageBox::information(&Client::getinstance(),"提示","状态异常");
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
            dis+=respdu_Msg->totoalLen;
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
