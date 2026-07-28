#include "mytcpsocket.h"
#include "operatedb.h"
#include "protocol.h"
#include "mytcpserver.h"
#include "memalloc.h"

#include <QByteArray>
#include <QFileInfo>

MyTcpSocket::MyTcpSocket()
{
    connect(this,&MyTcpSocket::readyRead,this,&MyTcpSocket::recvmsg);
    connect(this,&MyTcpSocket::disconnected,this,&MyTcpSocket::clientOffLine);
    m_MsgHandler=new MsgHandler;
    a=0;
    m_downloadFile=nullptr;
    m_downloadTotalSize=0;
    m_downloadSentSize=0;
    m_downloadActive=false;
    m_invalidPduCount=0;
}

MyTcpSocket::~MyTcpSocket(){
    if(m_downloadFile){
        m_downloadFile->close();
        delete m_downloadFile;
        m_downloadFile=nullptr;
    }
    delete m_MsgHandler;
}

// 所有权规则：
// - 返回非空 PDU → 由 recvmsg() 调用 seedMsg() 发送并释放
// - 返回 nullptr   → PDU 已由 handlMsg() 内部发送并释放（如下载流），或无需响应
// 禁止同一 PDU 既返回又通过 sendAndRelease/seedMsg 发送
PDU *MyTcpSocket::handlMsg(PDU *pdu)
{
    qDebug()<<"——————开始进行处理————————"<<endl;
    PDU* respdu=nullptr;
    m_MsgHandler->pdu=pdu;//这里它原来存储的pdu已经被字节数组（缓冲区）释放掉了
    //根据接收到的来自客户端的消息类型 进行处理
    switch(pdu->uiType){
        //注册用户
        case ENUM_MSG_TYPE_REGIST_REQUEST :{
             respdu=m_MsgHandler->rigist();
             break;
        }
        //登录用户
         case ENUM_MSG_TYPE_LOGIN_REQUEST :{
            respdu=m_MsgHandler->login(this->m_strLoginName);
            break;
         }
        //查找用户
    case ENUM_MSG_TYPE_findUser_REQUEST:{
            respdu=m_MsgHandler->finduser();
        break;
    }
        //获取所有非好友用户
    case ENUM_MSG_TYPE_ONLINEUSER_REQUEST:{
        respdu=m_MsgHandler->onlineuser(this->m_strLoginName);
        break;
    }
        //添加好友请求
    case ENUM_MSG_TYPE_ADDFRIEND_REQUEST:{
        respdu=m_MsgHandler->addfriend();
        break;
    }
        //添加好友回应
    case ENUM_MSG_TYPE_ADDFRIEND_RESPOND:{
        respdu=m_MsgHandler->addfriendFin();
        break;
    }
        //刷新好友
    case ENUM_MSG_TYPE_FLUSHFRIEND_REQUEST:{
        respdu=m_MsgHandler->flushfriend(this->m_strLoginName);
        break;
    }
        //删除好友
    case ENUM_MSG_TYPE_DELETEFRIEND_REQUEST:{
        respdu=m_MsgHandler->deletefriend();
        break;
    }
        //聊天（单条消息）
    case ENUM_MSG_TYPE_CHAT_REQUEST:{
        respdu=m_MsgHandler->chat();
        break;
    }
        //获取所有未读消息
    case ENUM_MSG_TYPE_GETCHATMSG_REQUEST:{
        respdu=m_MsgHandler->chatmsg();
        break;
    }
        //创建目录
    case ENUM_MSG_TYPE_MKDIR_REQUEST:{
        respdu=m_MsgHandler->MKDIR();
        break;
    }
        //跳转目录
    case ENUM_MSG_TYPE_CHANGEDIR_REQUEST:{
        respdu=m_MsgHandler->changedir();
        break;
    }
        //删除文件
    case ENUM_MSG_TYPE_DELFILE_REQUEST:{
        respdu=m_MsgHandler->deldir();
        break;
    }
        //刷新文件
    case ENUM_MSG_TYPE_FLUSHFILE_REQUEST:{
        respdu=m_MsgHandler->flushfile();
        break;
    }
        //上传文件数据请求
    case ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST:{
        respdu=m_MsgHandler->uploadfiledata();
        break;
    }
        //上传文件初始化请求
    case ENUM_MSG_TYPE_UPLOADFILE_INIT_REQUEST:{
        respdu=m_MsgHandler->uploadfileinit();
        break;
    }
        //重命名文件
    case ENUM_MSG_TYPE_RENAMEFILE_REQUEST:{
        respdu=m_MsgHandler->renamefile();
        break;
    }
        //分享文件请求
    case ENUM_MSG_TYPE_SHAREFILE_REQUEST:{
        respdu=m_MsgHandler->sharefile();
        break;
    }
        //分享文件同意回应
    case ENUM_MSG_TYPE_SHAREFILE_AGREE_RESPOND:{
        respdu=m_MsgHandler->sharefilemove();
        break;
    }
        //移动文件请求
    case ENUM_MSG_TYPE_MOVEFILE_REQUEST:{
        respdu=m_MsgHandler->movefile(this->m_strLoginName);
        break;
    }
    //下载文件请求——流式分块
    case ENUM_MSG_TYPE_DOWNLOADFILE_REQUEST:{
        respdu=m_MsgHandler->downloadfile(); //仅校验，返回元数据
        if(respdu){
            bool valid=false;
            memcpy(&valid,respdu->caData,sizeof(valid));
            sendAndRelease(respdu); //先发送并释放元数据响应
            respdu=nullptr;
            if(valid && !m_MsgHandler->m_downloadCanonicalPath.isEmpty()){
                startDownloadStream(m_MsgHandler->m_downloadCanonicalPath);
            }
        }
        return nullptr; //已经通过 sendAndRelease 发送或启动流式发送
    }
    case ENUM_MSG_TYPE_CAPTCHA_REQUEST:{
        respdu=m_MsgHandler->captcha();
        break;
    }
    case ENUM_MSG_TYPE_GETUSERINFORMATION_REQUEST:{
        respdu=m_MsgHandler->getuserinformation();
        break;
    }
    case ENUM_MSG_TYPE_LOGOFF_REQUEST:{
        respdu=m_MsgHandler->logoff();
        break;
    }
        default:
            break;
    }
   return respdu;
}

//启动流式下载
void MyTcpSocket::startDownloadStream(const QString& filePath)
{
    if(m_downloadFile){
        m_downloadFile->close();
        delete m_downloadFile;
        m_downloadFile=nullptr;
    }

    m_downloadFile=new QFile(filePath);
    if(!m_downloadFile->open(QIODevice::ReadOnly)){
        qDebug()<<"下载文件打开失败:"<<filePath;
        delete m_downloadFile;
        m_downloadFile=nullptr;

        //发送失败完成 PDU
        PDU* finishPdu=mkPDU();
        finishPdu->uiType=ENUM_MSG_TYPE_DOWNLOADFILE_FINISH;
        bool success=false;
        memcpy(finishPdu->caData,&success,sizeof(success));
        seedMsg(finishPdu);
        return;
    }

    m_downloadTotalSize=m_downloadFile->size();
    m_downloadSentSize=0;
    m_downloadActive=true;

    qDebug()<<"开始流式下载:"<<filePath<<"大小:"<<m_downloadTotalSize;
    //用 0ms 延迟的单次定时器启动分块发送（异步，避免阻塞事件循环）
    QTimer::singleShot(0,this,&MyTcpSocket::sendNextChunk);
}

//发送下一个数据块
void MyTcpSocket::sendNextChunk()
{
    if(!m_downloadActive || !m_downloadFile || !m_downloadFile->isOpen()){
        qDebug()<<"下载状态无效，停止分块发送";
        m_downloadActive=false;
        return;
    }

    qint64 remaining=m_downloadTotalSize-m_downloadSentSize;
    if(remaining<=0){
        //发送完成 PDU
        PDU* finishPdu=mkPDU();
        finishPdu->uiType=ENUM_MSG_TYPE_DOWNLOADFILE_FINISH;
        bool success=true;
        memcpy(finishPdu->caData,&success,sizeof(success));
        seedMsg(finishPdu);

        qDebug()<<"下载完成，总发送:"<<m_downloadSentSize<<"字节";
        m_downloadFile->close();
        delete m_downloadFile;
        m_downloadFile=nullptr;
        m_downloadActive=false;
        return;
    }

    int chunkSize=qMin((qint64)DOWNLOAD_CHUNK_SIZE,remaining);
    QByteArray chunk=m_downloadFile->read(chunkSize);
    if(chunk.isEmpty()){
        qDebug()<<"读取文件块失败";
        m_downloadFile->close();
        delete m_downloadFile;
        m_downloadFile=nullptr;
        m_downloadActive=false;

        //发送失败完成 PDU
        PDU* finishPdu=mkPDU();
        finishPdu->uiType=ENUM_MSG_TYPE_DOWNLOADFILE_FINISH;
        bool success=false;
        memcpy(finishPdu->caData,&success,sizeof(success));
        seedMsg(finishPdu);
        return;
    }

    //构建数据块 PDU
    PDU* dataPdu=mkPDU(chunk.size());
    dataPdu->uiType=ENUM_MSG_TYPE_DOWNLOADFILE_DATA;
    memcpy(dataPdu->caMsg,chunk.constData(),chunk.size());
    m_downloadSentSize+=chunk.size();

    seedMsg(dataPdu);

    //调度下一个块
    QTimer::singleShot(0,this,&MyTcpSocket::sendNextChunk);
}
//接收客户端消息
void MyTcpSocket::recvmsg(){
    QByteArray dpui=this->readAll();
    this->m_dpulist.append(dpui);
    
    // 安全校验：未处理缓存超过 256 KB，防止耗尽内存攻击
    if (m_dpulist.size() > MAX_PENDING_INBOUND_BYTES) {
        qWarning() << "Security: Inbound buffer exceeded 256KB from" 
                   << this->peerAddress() << "- size:" << m_dpulist.size();
        m_dpulist.clear();
        this->disconnectFromHost();
        return;
    }

    while(this->m_dpulist.size()>=(int)sizeof (PDU)){
            PDU* pdu=(PDU*)m_dpulist.data();
            uint pduTotalLen=pdu->uiTotalLen;
            
            // 安全校验：解析头部合法性
            bool valid = false;
            if (pduTotalLen >= sizeof(PDU) && pduTotalLen <= MAX_INBOUND_PDU_SIZE) {
                if (pdu->uiMsgLen == pduTotalLen - sizeof(PDU) &&
                    pdu->uiType > ENUM_MSG_TYPE_MIN && 
                    pdu->uiType < ENUM_MSG_TYPE_MAX) {
                    valid = true;
                }
            }
            
            if (!valid) {
                m_invalidPduCount++;
                qWarning() << "Security: Invalid PDU from" << this->peerAddress()
                           << "Count:" << m_invalidPduCount << "/" << MAX_INVALID_PDU_THRESHOLD
                           << "uiTotalLen:" << pduTotalLen
                           << "uiMsgLen:" << pdu->uiMsgLen
                           << "uiType:" << pdu->uiType;
                
                if (m_invalidPduCount >= MAX_INVALID_PDU_THRESHOLD) {
                    qCritical() << "Security: Dropped packets exceeded threshold. Kicking client:" << this->peerAddress();
                    m_dpulist.clear();
                    this->disconnectFromHost();
                    return;
                }
                
                // 仅丢弃当前非法 PDU 的长度，继续解析剩余数据（尝试同步）
                m_dpulist.remove(0, pduTotalLen >= sizeof(PDU) ? pduTotalLen : sizeof(PDU));
                continue;
            }

            // 校验通过，重置计数器（证明客户端是合法的或已恢复同步）
            m_invalidPduCount = 0;

            if(m_dpulist.size()>=(int)pduTotalLen){
                qDebug()<<"请求信息已接收成功:"<<" "
                        <<"pdu->uiTotalLen"<<pdu->uiTotalLen<<" "
                        <<"pdu->uiMsgLen"<<pdu->uiMsgLen<<" "
                        <<"pdu->uiType"<<pdu->uiType<<"  "
                        <<"pdu->caData"<<pdu->caData<<" "
                        <<"pdu->caData+32"<<pdu->caData+32<<" "
                        <<"pdu->caMsg"<<pdu->caMsg<<endl;
                PDU* respdu=handlMsg(pdu);
                seedMsg(respdu);
                m_dpulist.remove(0,pdu->uiTotalLen);
            }
            else break;
        }
}
//客户端下线->更改客户端的登录状态，并且移除sorket对应的
void MyTcpSocket::clientOffLine(){
    //清理下载状态
    if(m_downloadFile){
        m_downloadFile->close();
        delete m_downloadFile;
        m_downloadFile=nullptr;
    }
    m_downloadActive=false;
    OperateDB::instance().handleOffLine(m_strLoginName.toStdString().c_str());
    MyTcpServer::getinstance().removeSocket(this);
    qDebug()<<"用户"<<m_strLoginName<<"已经退出登录"<<endl;
}
//发送 PDU 到客户端并释放其内存（所有权转移到此方法）
//pdu 为 nullptr 时安全空操作
void MyTcpSocket::sendAndRelease(PDU* pdu)
{
    if (pdu == nullptr) return;
    write((char*)pdu, pdu->uiTotalLen);
    memfree(pdu);
}
// 发送 PDU 给客户端。pdu 必须由 Server 端 mkPDU() (memalloc) 分配。
// 发送完成后调用 memfree(pdu) 释放。
// 调用方不得在 seedMsg 返回后再访问 pdu。
//给客户端发送respdu消息
void MyTcpSocket::seedMsg(PDU *respdu){
    if(respdu==nullptr){
        return;
    }
    this->write((char*)respdu,respdu->uiTotalLen);
    qDebug()<<"响应信息 已发送:"<<" "
            <<"respdu->uiTotalLen"<<respdu->uiTotalLen<<" "
            <<"respdu->uiMsgLen"<<respdu->uiMsgLen<<" "
            <<"respdu->uiType"<<respdu->uiType<<" "
              <<"respdu->caData"<<respdu->caData<<" "
            <<"respdu->caData+32"<<respdu->caData+32<<" "
            <<(respdu->uiType==ENUM_MSG_TYPE_DOWNLOADFILE_DATA ? QByteArray::number(respdu->uiMsgLen).constData() :
               respdu->uiType==ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND ? "metadata" : "respdu->caMsg")
            <<endl;
    memfree(respdu);
    respdu=nullptr;
}
