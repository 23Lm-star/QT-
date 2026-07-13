#include "mytcpsocket.h"
#include "operatedb.h"
#include "protocol.h"
#include "mytcpserver.h"
#include "memalloc.h"

MyTcpSocket::MyTcpSocket()
{
    connect(this,&MyTcpSocket::readyRead,this,&MyTcpSocket::recvmsg);
    connect(this,&MyTcpSocket::disconnected,this,&MyTcpSocket::clientOffLine);
    m_MsgHandler=new MsgHandler;
    a=0;
}

MyTcpSocket::~MyTcpSocket(){
    delete m_MsgHandler;
}

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
        default:
            break;
    }
   return respdu;
}
//接收客户端消息
void MyTcpSocket::recvmsg(){
    QByteArray dpui=this->readAll();
    this->m_dpulist.append(dpui);
    while(this->m_dpulist.size()>=(int)sizeof (PDU)){
            PDU* pdu=(PDU*)m_dpulist.data();
            uint pduTotalLen=pdu->uiTotalLen;
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
    OperateDB::instance().handleOffLine(m_strLoginName.toStdString().c_str());
    MyTcpServer::getinstance().removeSocket(this);
    qDebug()<<"用户"<<m_strLoginName<<"已经退出登录"<<endl;
}
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
            <<"respdu->caMsg"<<respdu->caMsg<<endl;
    memfree(respdu);
    respdu=nullptr;
}
