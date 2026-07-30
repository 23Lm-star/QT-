#ifndef MSGHANDLER_H
#define MSGHANDLER_H

#include "protocol.h"
#include "resumableuploadsession.h"

#include <QString>
#include <QDateTime>
#include <QPair>
#include <QMap>

//用于作为 服务器处理客户端请求 的类
class MsgHandler
{
public:
    MsgHandler();
    //记录客户端发送的请求DPU
    PDU* pdu;
    QString ClientName; 
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
    PDU* deldir(const QString& loginName);//删除目录
    PDU* changedir();//跳转目录
    PDU* flushfile();//刷新文件
    PDU* renamefile(const QString& loginName);//重命名文件
    PDU* sharefile();//分享文件
    PDU* sharefilemove();//分享文件移动
    PDU* movefile(const QString& loginName);//移动文件
    PDU* listdir(const QString& loginName);//列出子目录（移动文件选择目标用）
    PDU* uploadfileinit();//上传文件初始化
    PDU* uploadfiledata();//上传文件数据
    PDU* downloadfile();
    PDU* chatmsg();//获取聊天消息
    PDU* captcha();//验证码生成
    QString generateCaptchaCode(int length = 6);
    PDU* getuserinformation();//获取用户信息
    PDU* logoff();//注销账户
    PDU* createPublishLink(const QString& loginName);//创建外链分享
    
    // 新增：安全外链分享（BUG-ARC-PUBLINK-001）
    PDU* resolvePublishLink(const QString& loginName);
    PDU* queryFilePublishLink();

    //验证码存储
    QMap<QString, QDateTime> m_captchas;
    int validateCaptcha(const char* caMsg, uint uiMsgLen); // 0通过,-2错误,-3过期

    //下载文件时存储通过验证的规范路径
    QString m_downloadCanonicalPath;
};

#endif // MSGHANDLER_H
