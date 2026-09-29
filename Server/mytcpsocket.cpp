#include "logging.h"
#include "mytcpsocket.h"
#include "operatedb.h"
#include "protocol.h"
#include "mytcpserver.h"
#include "memalloc.h"
#include "uploadwritetask.h"

#include <QByteArray>
#include <QFileInfo>
#include <QTimer>
#include <QFile>
#include <QDateTime>
#include <QThreadPool>

MyTcpSocket::MyTcpSocket()
{
    connect(this,&MyTcpSocket::readyRead,this,&MyTcpSocket::recvmsg);
    connect(this,&MyTcpSocket::bytesWritten,this,&MyTcpSocket::onBytesWritten);
    connect(this,&MyTcpSocket::disconnected,this,&MyTcpSocket::clientOffLine);
    m_MsgHandler=new MsgHandler;
    a=0;
    m_downloadFile=nullptr;
    m_downloadTotalSize=0;
    m_downloadSentSize=0;
    m_downloadActive=false;
    m_downloadNextChunkScheduled=false;
    m_uploadWriteInFlight=false;
    m_connectionId=0;
    m_invalidPduCount=0;
    m_lastActiveMs = QDateTime::currentMSecsSinceEpoch();
}

MyTcpSocket::~MyTcpSocket(){
    if(m_downloadFile){
        m_downloadFile->close();
        delete m_downloadFile;
        m_downloadFile=nullptr;
    }
    delete m_MsgHandler;
}

PDU *MyTcpSocket::handlMsg(PDU *pdu)
{
    qCDebug(lcNet)<<"——————开始进行处理————————"<<endl;
    PDU* respdu=nullptr;
    m_MsgHandler->pdu=pdu;
    
    switch(pdu->uiType){
        case ENUM_MSG_TYPE_REGIST_REQUEST :{
             respdu=m_MsgHandler->rigist();
             break;
        }
         case ENUM_MSG_TYPE_LOGIN_REQUEST :{
            respdu=m_MsgHandler->login(this->m_strLoginName);
            break;
         }
    case ENUM_MSG_TYPE_findUser_REQUEST:{
            respdu=m_MsgHandler->finduser();
        break;
    }
    case ENUM_MSG_TYPE_ONLINEUSER_REQUEST:{
        respdu=m_MsgHandler->onlineuser(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_ADDFRIEND_REQUEST:{
        respdu=m_MsgHandler->addfriend();
        break;
    }
    case ENUM_MSG_TYPE_ADDFRIEND_RESPOND:{
        respdu=m_MsgHandler->addfriendFin();
        break;
    }
    case ENUM_MSG_TYPE_FLUSHFRIEND_REQUEST:{
        respdu=m_MsgHandler->flushfriend(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_DELETEFRIEND_REQUEST:{
        respdu=m_MsgHandler->deletefriend();
        break;
    }
    case ENUM_MSG_TYPE_CHAT_REQUEST:{
        respdu=m_MsgHandler->chat();
        break;
    }
    case ENUM_MSG_TYPE_GETCHATMSG_REQUEST:{
        respdu=m_MsgHandler->chatmsg();
        break;
    }
    case ENUM_MSG_TYPE_MKDIR_REQUEST:{
        respdu=m_MsgHandler->MKDIR();
        break;
    }
    case ENUM_MSG_TYPE_CHANGEDIR_REQUEST:{
        respdu=m_MsgHandler->changedir();
        break;
    }
    case ENUM_MSG_TYPE_DELFILE_REQUEST:{
        respdu=m_MsgHandler->deldir(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_FLUSHFILE_REQUEST:{
        respdu=m_MsgHandler->flushfile();
        break;
    }
    case ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST:{
        handleUploadData(pdu);
        return nullptr;
    }
    case ENUM_MSG_TYPE_UPLOADFILE_INIT_REQUEST:{
        respdu=m_MsgHandler->uploadfileinit();
        break;
    }
    case ENUM_MSG_TYPE_RENAMEFILE_REQUEST:{
        respdu=m_MsgHandler->renamefile(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_SHAREFILE_REQUEST:{
        respdu=m_MsgHandler->sharefile();
        break;
    }
    case ENUM_MSG_TYPE_SHAREFILE_AGREE_RESPOND:{
        respdu=m_MsgHandler->sharefilemove();
        break;
    }
    case ENUM_MSG_TYPE_MOVEFILE_REQUEST:{
        respdu=m_MsgHandler->movefile(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_LISTDIR_REQUEST:{
        respdu=m_MsgHandler->listdir(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_DOWNLOADFILE_REQUEST:{
        respdu=m_MsgHandler->downloadfile();
        if(respdu){
            bool valid=false;
            memcpy(&valid,respdu->caData,sizeof(valid));
            sendAndRelease(respdu);
            respdu=nullptr;
            if(valid && !m_MsgHandler->m_downloadCanonicalPath.isEmpty()){
                startDownloadStream(m_MsgHandler->m_downloadCanonicalPath);
            }
        }
        return nullptr;
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
    case ENUM_MSG_TYPE_HEARTBEAT_REQUEST:{
        m_lastActiveMs = QDateTime::currentMSecsSinceEpoch();
        qCDebug(lcNet) << "[HEARTBEAT] recv PING from" << m_strLoginName << "-> send PONG";
        PDU* hRespdu = mkPDU();
        hRespdu->uiType = ENUM_MSG_TYPE_HEARTBEAT_RESPOND;
        return hRespdu;
    }
    case ENUM_MSG_TYPE_CREATE_PUBLINK_REQUEST:{
        respdu=m_MsgHandler->createPublishLink(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_RESOLVE_PUBLINK_REQUEST:{
        respdu=m_MsgHandler->resolvePublishLink(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_REQUEST:{
        respdu=m_MsgHandler->queryFilePublishLink();
        break;
    }
        default:
            break;
    }
   return respdu;
}

//启动流式下载
void MyTcpSocket::handleUploadData(PDU* pdu)
{
    uint resumeMagic=0;
    qint64 chunkOffset=0;
    memcpy(&resumeMagic,pdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,sizeof(resumeMagic));
    memcpy(&chunkOffset,pdu->caData+UPLOAD_CHUNK_OFFSET,sizeof(chunkOffset));

    const qint64 expectedOffset=m_MsgHandler->m_uploadSession.receivedSize();
    if(resumeMagic != UPLOAD_RESUME_MAGIC || m_uploadWriteInFlight
            || chunkOffset != expectedOffset || pdu->uiMsgLen == 0
            || pdu->uiMsgLen > m_MsgHandler->m_uploadSession.totalSize()-expectedOffset
            || m_MsgHandler->m_uploadSession.stagingFilePath().isEmpty()){
        sendUploadAck(false,expectedOffset,m_MsgHandler->m_uploadSession.isComplete());
        return;
    }

    m_uploadWriteInFlight=true;
    const QByteArray data(pdu->caMsg,pdu->uiMsgLen);
    QThreadPool::globalInstance()->start(new UploadWriteTask(
            m_connectionId,m_MsgHandler->m_uploadSession.stagingFilePath(),chunkOffset,data));
}

void MyTcpSocket::handleUploadWriteFinished(qint64 offset, qint64 length, bool success)
{
    m_uploadWriteInFlight=false;
    if(success){
        success=m_MsgHandler->m_uploadSession.acceptWrittenChunk(offset,length);
    }
    sendUploadAck(success,m_MsgHandler->m_uploadSession.receivedSize(),
                  success && m_MsgHandler->m_uploadSession.isComplete());
}

void MyTcpSocket::sendUploadAck(bool success, qint64 nextOffset, bool complete)
{
    PDU* response=mkPDU();
    const uint resumeMagic=UPLOAD_RESUME_MAGIC;
    response->uiType=ENUM_MSG_TYPE_UPLOADFILE_DATA_RESPOND;
    memcpy(response->caData,&success,sizeof(success));
    memcpy(response->caData+UPLOAD_ACK_NEXT_OFFSET,&nextOffset,sizeof(nextOffset));
    memcpy(response->caData+UPLOAD_ACK_COMPLETE_OFFSET,&complete,sizeof(complete));
    memcpy(response->caData+UPLOAD_RESUME_MAGIC_OFFSET,&resumeMagic,sizeof(resumeMagic));
    seedMsg(response);
}

void MyTcpSocket::startDownloadStream(const QString& filePath)
{
    if(m_downloadFile){
        m_downloadFile->close();
        delete m_downloadFile;
        m_downloadFile=nullptr;
    }

    m_downloadFile=new QFile(filePath);
    if(!m_downloadFile->open(QIODevice::ReadOnly)){
        qCWarning(lcNet)<<"下载文件打开失败:"<<filePath;
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

    qCDebug(lcNet)<<"开始流式下载:"<<filePath<<"大小:"<<m_downloadTotalSize;
    //用 0ms 延迟的单次定时器启动分块发送（异步，避免阻塞事件循环）
    scheduleNextDownloadChunk();
}

void MyTcpSocket::scheduleNextDownloadChunk()
{
    if(!m_downloadActive || m_downloadNextChunkScheduled){
        return;
    }
    m_downloadNextChunkScheduled=true;
    QTimer::singleShot(0,this,&MyTcpSocket::sendNextChunk);
}

void MyTcpSocket::onBytesWritten(qint64)
{
    if(m_downloadActive && bytesToWrite()<DOWNLOAD_WRITE_HIGH_WATER){
        scheduleNextDownloadChunk();
    }
}

//发送下一个数据块
void MyTcpSocket::sendNextChunk()
{
    m_downloadNextChunkScheduled=false;
    if(!m_downloadActive || !m_downloadFile || !m_downloadFile->isOpen()){
        qCDebug(lcNet)<<"下载状态无效，停止分块发送";
        m_downloadActive=false;
        return;
    }

    if(bytesToWrite()+DOWNLOAD_CHUNK_SIZE+static_cast<qint64>(sizeof(PDU))
            > DOWNLOAD_WRITE_HIGH_WATER){
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

        qCDebug(lcNet)<<"下载完成，总发送:"<<m_downloadSentSize<<"字节";
        m_downloadFile->close();
        delete m_downloadFile;
        m_downloadFile=nullptr;
        m_downloadActive=false;
        return;
    }

    int chunkSize=qMin((qint64)DOWNLOAD_CHUNK_SIZE,remaining);
    QByteArray chunk=m_downloadFile->read(chunkSize);
    if(chunk.isEmpty()){
        qCWarning(lcNet)<<"读取文件块失败";
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
    scheduleNextDownloadChunk();
}
//接收客户端消息
void MyTcpSocket::recvmsg(){
    m_lastActiveMs = QDateTime::currentMSecsSinceEpoch();
    QByteArray dpui=this->readAll();
    this->m_dpulist.append(dpui);
    
    // 安全校验：未处理缓存超过 256 KB，防止耗尽内存攻击
    if (m_dpulist.size() > MAX_PENDING_INBOUND_BYTES) {
        qCWarning(lcNet) << "Security: Inbound buffer exceeded 256KB from" 
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
                qCWarning(lcNet) << "Security: Invalid PDU from" << this->peerAddress()
                           << "Count:" << m_invalidPduCount << "/" << MAX_INVALID_PDU_THRESHOLD
                           << "uiTotalLen:" << pduTotalLen
                           << "uiMsgLen:" << pdu->uiMsgLen
                           << "uiType:" << pdu->uiType;
                
                if (m_invalidPduCount >= MAX_INVALID_PDU_THRESHOLD) {
                    qCCritical(lcNet) << "Security: Dropped packets exceeded threshold. Kicking client:" << this->peerAddress();
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
                qCDebug(lcNet)<<"请求信息已接收成功:"<<" "
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
    qCDebug(lcNet)<<"用户"<<m_strLoginName<<"已经退出登录"<<endl;
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
    qCDebug(lcNet)<<"响应信息 已发送:"<<" "
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
