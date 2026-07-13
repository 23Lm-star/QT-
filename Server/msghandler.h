#ifndef MSGHANDLER_H
#define MSGHANDLER_H

#include "protocol.h"
#include "resumableuploadsession.h"

#include <QString>

//用于作为 服务器处理客户端请求 的类
class MsgHandler
{
public:
    MsgHandler();
    //记录客户端发送的请求DPU
    PDU* pdu;
    //记录要上传的文件对象还有已上传的大小和总大小
    ResumableUploadSession m_uploadSession;
    //各种处理函数，进行各种类型请求的处理
    PDU* rigist();//注册
    PDU* login(QString& strname);//登录
    PDU* finduser();//查找用户
    PDU *onlineuser(const QString & myname);//在线用户
    PDU* addfriend();//添加用户
    PDU* addfriendFin();//添加好友
    PDU* flushfriend(const  QString& strname);//刷新好友
    PDU* deletefriend();//删除好友
    PDU* chat();//聊天
    PDU* MKDIR();//创建目录
    PDU* deldir();//删除目录
    PDU* changedir();//跳转目录
    PDU* flushfile();//刷新文件
    PDU* renamefile();//重命名文件
    PDU* sharefile();//分享文件
    PDU* sharefilemove();//分享文件移动
    PDU* uploadfileinit();//上传文件初始化
    PDU* uploadfiledata();//上传文件数据
    PDU* chatmsg();//获取聊天消息
};

#endif // MSGHANDLER_H
