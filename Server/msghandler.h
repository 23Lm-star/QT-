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

    // ===== 验证码：单码复用 + 服务端强制冷却 + 连续错误锁定 =====
    // 参数（与服务端逻辑强一致）
    static const int CAPTCHA_COOLDOWN_SEC = 60;   // 正常冷却：获取新码后 60s 内禁止再取
    static const int CAPTCHA_LOCKOUT_SEC = 600;   // 锁定：连续错误达上限后冷静 10 分钟
    static const int CAPTCHA_MAX_FAILS   = 5;     // 连续错误达到此值触发锁定
    static const int CAPTCHA_EXPIRE_SEC  = 60;    // 单条验证码有效期（秒）

    // 单码状态（每连接仅保留一个当前有效码，重取即让旧码失效）
    QString m_captchaCode;
    QDateTime m_captchaExpiry;

    // 冷却 / 锁定状态
    QDateTime m_captchaNextAllowed;   // 最早可再次成功获取验证码的时间
    QDateTime m_captchaLockUntil;     // 锁定结束时间（isValid() 且 now < 此值 => 锁定中）
    int m_captchaFailCount;           // 连续错误计数

    // 返回：0通过 / -2错误(不存在或不符) / -3过期 / -4已锁定；remainingSec 输出锁定剩余秒数
    int validateCaptcha(const char* caMsg, uint uiMsgLen, int& remainingSec);

    //下载文件时存储通过验证的规范路径
    QString m_downloadCanonicalPath;

    // REQ-FS-003: 统一路径沙箱校验（集中一处，所有路径入口调用）
    // 根与路径分别做 canonical 归一（解析 ../ 与符号链接）后判前缀；
    // 仅当 根存在 且 路径存在 且 canonical(path) 落在 canonical(root) 内（含根本身）时返回 true。
    // root 为空默认整个 filesPath；movefile/listdir 传入用户目录根以保持用户级隔离。
    static bool isPathInSandbox(const QString &path, const QString &root = QString());
};

#endif // MSGHANDLER_H
