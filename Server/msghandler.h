#ifndef MSGHANDLER_H
#define MSGHANDLER_H

#include "protocol.h"
#include "fileobjectstore.h"
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
    PDU* login(QString& strname, bool& authOk, quint64& loginVersion);//登录（REQ-EXT-SEC-001：authOk 供连接绑定 actor_context；REQ-EXT-SESSION-001：loginVersion 供连接绑定用户域版本）
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
    PDU* uploadfilefastcheck();//REQ-EXT-FILE-002：秒传预检查（命中零正文，未命中登记暂存会话）
    PDU* uploadfiledata();//上传文件数据
    PDU* downloadfile();
    PDU* chatmsg();//获取聊天消息
    PDU* captcha();//验证码生成
    QString generateCaptchaCode(int length = 6);
    PDU* getuserinformation();//获取用户信息
    PDU* logoff(bool& revoked);//注销账户（REQ-EXT-SEC-001：授权依据为连接级身份，revoked 供会话撤销）
    // REQ-EXT-GROUP-001：统一解析、校验并处理 60-87 群协议请求。
    PDU* groupRequest(uint requestType);
    // REQ-EXT-GROUP-003：统一解析、校验并处理群文件协议请求（99/101/103/105）。
    // 权限判定全部落在 OperateDB::handleGroupFileOperation（服务端即时判定）。
    PDU* groupFileRequest(uint requestType);

    // ===== REQ-EXT-SEC-001：连接级 actor_context（仅服务端登录成功时写入，客户端 PDU 字段不作为授权依据） =====
    QString m_actorName;   // 服务端派生的操作者用户名
    quint64 m_connId = 0;  // 连接关联标识（拒绝审计用）
    QString actorUserRoot() const;                     // 服务端生成的规范用户根：filesPath/<actor>
    bool pathInActorRoot(const QString& path) const;   // 路径必须落在 actor 本人用户根内
    // REQ-EXT-FILE-001 D5：内容寻址后文件不再物理落用户路径，下载/删除改为
    // 校验其所在目录（目录由 MKDIR 物理创建，必存在）落在 actor 根内。
    bool parentDirInActorRoot(const QString& filePath) const;
    // REQ-EXT-FILE-001 design §6：逻辑条目操作（改名/移动/分享登记）不要求物理存在，
    // 仅做词法规范化 + actor 根越权判断（isPathInSandbox 原样留给仍在磁盘执行的读写）。
    bool logicPathInActorRoot(const QString& path) const;
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

    // ===== REQ-EXT-FILE-001 T3/T4：内容寻址上传落定 =====
    qint64 m_uploadSessionId = -1;   // 当前暂存会话 upload_session.session_id
    // REQ-EXT-FILE-002 D2：当前上传声明的 SHA-256（= upload_session.client_sha256），
    // 只作命中判定与复算对照，永不作为转正依据。
    QString m_uploadExpectedSha;
    // ===== REQ-EXT-GROUP-003：当前分块上传的群作用域（非 0 = 群文件上传）=====
    // 由 101 未命中路径写入；上传完成（finalizeUploadOnComplete）时建群引用后清空。
    quint64 m_groupUploadGroupId = 0;
    QString m_groupUploadDisplayName;
    quint64 m_lastGroupFileId = 0;   // 最近一次群上传落定的 group_file_id（0=无）
    // 上传完成时调用：commitUpload（对象 find-or-create + 条目 + 引用计数，同事务）
    // + finishUploadSession；staging 由 FileObjectStore 转正或丢弃。
    // 返回转正结果（调用方可忽略）；失败时 CommitResult.ok=false。
    FileObjectStore::CommitResult finalizeUploadOnComplete();

    // REQ-FS-003: 统一路径沙箱校验（集中一处，所有路径入口调用）
    // 根与路径分别做 canonical 归一（解析 ../ 与符号链接）后判前缀；
    // 仅当 根存在 且 路径存在 且 canonical(path) 落在 canonical(root) 内（含根本身）时返回 true。
    // root 为空默认整个 filesPath；movefile/listdir 传入用户目录根以保持用户级隔离。
    static bool isPathInSandbox(const QString &path, const QString &root = QString());
};

#endif // MSGHANDLER_H
