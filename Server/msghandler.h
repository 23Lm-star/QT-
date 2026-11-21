// msghandler.h — 服务端「单连接请求处理器」MsgHandler 的声明。
// 每个客户端连接（MyTcpSocket）持有一个实例：按 PDU 类型分发到注册/登录/好友/
// 文件与传输/群组等处理函数，并集中保存该连接的认证身份、验证码状态与传输上下文。
// 所有方法都在连接所属的 I/O 线程内调用，类内部不加锁。
#ifndef MSGHANDLER_H
#define MSGHANDLER_H

#include "protocol.h"
#include "fileobjectstore.h"
#include "resumableuploadsession.h"

#include <QString>
#include <QByteArray>
#include <QDateTime>
#include <QPair>
#include <QMap>
#include <QHash>
#include <QVector>

//用于作为 服务器处理客户端请求 的类
class MsgHandler
{
public:
    // 构造：pdu 置空、验证码失败计数归零；首次构造时顺带播种全局 qrand。
    MsgHandler();
    //记录客户端发送的请求DPU
    PDU* pdu;
    // 历史用户名槽位（当前源码未使用；授权一律以 m_actorName 为准）
    QString ClientName;

    // ===== REQ-EXT-TRANSFER-001：每连接 per-transfer 上传上下文 =====
    struct UploadCtx {
        quint64  id = 0;                 // 0 = 无效编号
        ResumableUploadSession session;  // 既有类型，原样复用
        qint64   sessionId = -1;         // upload_session.session_id
        QString  expectedSha;            // 既有 m_uploadExpectedSha
        quint64  groupId = 0;            // 既有 m_groupUploadGroupId
        QString  groupDisplayName;       // 既有 m_groupUploadDisplayName
        bool     writeInFlight = false;  // 既有 m_uploadWriteInFlight（逐 transfer 化）
        qint64   lastProgressMs = 0;     // 空闲回收用
        // AUD-1：97 秒传预检查借临时槽做暂存登记，这不是一次上传，**不占用上传并发名额**。
        // 首次收到该槽的 31 正文（或该槽被随后的 29 采纳）时本标记失效。
        bool     fastcheckPlaceholder = false;
        // AUD-4：107 在写盘在途时到达 → 延迟释放。立即 abort 会让在途写落在 ctx 消失之后，
        // 使暂存物理大小越过 upload_session.confirmed_offset。置位后由 handleUploadWriteFinished 收尾。
        bool     pendingAbort = false;
        // REQ-EXT-TRANSFER-003 S8/D6：进度落库节流（≥1 MiB 或 ≥2 s 才 UPDATE 一次）
        qint64   lastPersistOffset = 0;  // 上次落库时的已确认偏移
        qint64   lastPersistMs = 0;      // 上次落库时刻
        // ===== REQ-EXT-PERF-002：完成复算在途与迟到应答上下文 =====
        // 复算在途（线程池算整文件 SHA-256 期间）：该槽不被空闲回收（与 writeInFlight 同口径），
        // 107 到达转 pendingAbort 延迟执行；结果回主线程时按 transferId 校验本标志，不符即丢弃。
        bool     hashInFlight = false;
        // 复算落定后要迟到补发的应答类型（DeferredRespond）。
        quint8   deferredRespond = 0;
        // 32：末块写落定时的 success/下一偏移（M3 后 32 成败以提交结果为准，本字段仅留档）。
        bool     deferredAckSuccess = false;
        qint64   deferredAckOffset = 0;
        // 98：回带的 97 请求标识 + 命中时的续传起点（=文件总大小）。
        QByteArray deferredReqId;
        // 98/102：登记的文件总大小（迟到应答的 nextOffset 字段）。
        qint64   deferredFileSize = 0;
        // 102：群信封 request_id 与展示名（应答体重建用）。
        quint64  deferredRequestId = 0;
        QString  deferredName;
        // REQ-RESUME-AUDIT-001 M3：30 迟到补发参数（29 的暂存已在尾部时，complete 依赖条目落定）。
        qint64   deferredResumeOffset = 0;      // 成功时回带的续传起点（封顶后）
        bool     deferredResumeMagicOn = false; // 29 请求是否启用续传魔数
    };
    QHash<quint64, UploadCtx> m_uploads;   // key 为有效传输编号
    // handlMsg 按 PDU 落位写入的当前传输 id（29/31/40/101/105/107）；非传输族 = 0
    quint64 m_currentTransferId = 0;
    // 97 未命中建立的会话暂存：随后的 29 INIT（同 parent/display/size）直接采纳，
    // 避免重复建会话，并保留客户端摘要的服务端复算对照（FILE-002 D2）。
    struct PendingFastcheck {
        bool    active = false;
        qint64  sessionId = -1;
        QString shaHex;
        QString parent;
        QString display;
        qint64  size = 0;
        // REQ-RESUME-AUDIT-001（审计修复）：97 登记时的已确认偏移——29 采纳（adopt）路径
        // 以此为续传起点封顶，使 adopt 与复用/新建路径同受 min(物理, 已确认) 约束。
        qint64  offset = 0;
    };
    PendingFastcheck m_pendingFastcheck;
    // 按传输 id 查找上下文；不存在返回 nullptr。返回指针指向哈希表内部节点，
    // 后续 m_uploads 的插入/删除可能使其失效，不可跨调用长期持有。
    UploadCtx* findUpload(quint64 id);
    // REQ-EXT-TRANSFER-003 S5 补充：同一暂存路径可能被一个以上上下文引用（97 预检查槽 / 幂等重放 /
    // 旧客户端占位）。动文件（rename/delete/truncate）前必须释放**全部**指向该路径的句柄，
    // 否则 Windows 下会被占用句柄拒绝（实测：97 别名句柄让 commitUpload 的 rename 失败 → 会话判 failed）。
    void releaseOtherStagingHandles(quint64 keepId, const QString &stagingPath);
    UploadCtx& currentUpload();            // 不存在则新建（调用方须先过 acquireUploadSlot）
    // 上限（D7）与重复编号（T-17）判定
    bool acquireUploadSlot(quint64 id);
    void abortUpload(quint64 id);          // 释放槽位：upload_session 置 expired，staging 保留
    void reapIdleUploads(qint64 nowMs, int idleSec);   // 空闲回收（返回被回收条目数）
    // AUD-1：上传并发名额计数**排除** 97 预检查占位，其余条目照旧计入
    int  activeUploadCount() const {
        int n = 0;
        for (QHash<quint64, UploadCtx>::const_iterator it = m_uploads.constBegin();
             it != m_uploads.constEnd(); ++it) {
            if (!it->fastcheckPlaceholder) {
                ++n;
            }
        }
        return n;
    }
    //各种处理函数，进行各种类型请求的处理
    PDU* rigist();//注册
    PDU* login(QString& strname, bool& authOk, quint64& loginVersion,
               QString& outClientId, QString& outResumeToken);//登录（REQ-EXT-SEC-001：authOk 供连接绑定 actor_context；REQ-EXT-SESSION-001：loginVersion 供连接绑定用户域版本；REQ-AUTH-CONNECTION-001：outClientId=严格校验后的设备标识（空=旧兼容路径），outResumeToken=一次性恢复凭据明文，仅建会话成功时回传）
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
    // 下载校验：只解析/校验目标并回元数据，正文由 MyTcpSocket 流式发送。
    PDU* downloadfile();
    PDU* chatmsg();//获取聊天消息
    // REQ-AUDIT-001 R2：本次 18 响应携带的未送达主键——由连接层在写出成功后确认，
    // 写出失败/断线则不调用，消息保持待送达等下次重投。
    QVector<qlonglong> m_pendingDeliveredIds;
    void confirmChatDelivered();
    // ===== REQ-CHAT-ACK-001：桌面端私聊接收确认 =====
    // 111：拉取本人待送达私聊（来自 caData[0,32) 指定的对方），响应 112 每条带消息 ID，
    //      只读不标记——只有客户端 110 确认后才置已送达。
    PDU* chatPending();
    // 110：客户端确认已处理的消息 ID 列表（caMsg 为 N×qint64），按「ID+收件人」标记，
    //      无响应；返回 nullptr。
    void chatDeliveredAck();
    // 113/114：本人仍待处理的好友申请（离线期间登记的也在内）
    PDU* listPendingFriendRequests();
    // ===== REQ-PAGE-FETCH-001：117/118 聊天历史分页（桌面端专用；17/18 网页端语义不变） =====
    PDU* chatHistory();

    // 115：拒绝好友申请（只清本人收到的 pending 行，无响应）
    void rejectFriendRequest();
    // 取验证码：成功则生成新码并从 caMsg 返回，锁定/冷却中只回状态码与剩余秒数。
    PDU* captcha();//验证码生成
    // 从固定字符表随机生成 length 位验证码（依赖 MsgHandler 构造时的 qrand 播种）。
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
    // REQ-EXT-TRANSFER-001 D7：下载方向并发名额是否可用（由 MyTcpSocket 在本连接会话前写入；
    // 超上限时 40 → ret=false、105 → code=-2107，且不建流）
    bool m_downloadSlotAvailable = true;
    // REQ-EXT-TRANSFER-002 S2：客户端在 40/105 声明的续传起点（由 MyTcpSocket 处理前写入；
    // 0 = 从头，旧客户端由 mkPDU 全量清零天然写 0）
    qint64 m_downloadResumeOffset = 0;

    // ===== REQ-EXT-FILE-001 T3/T4：内容寻址上传落定 =====
    // REQ-EXT-FILE-002 D2：上传声明的 SHA-256（= upload_session.client_sha256）现落在 UploadCtx.expectedSha，
    // 只作命中判定与复算对照，永不作为转正依据。
    quint64 m_lastGroupFileId = 0;   // 最近一次群上传落定的 group_file_id（0=无）
    // 上传完成时调用：commitUpload（对象 find-or-create + 条目 + 引用计数，同事务）
    // + finishUploadSession；staging 由 FileObjectStore 转正或丢弃。
    // 返回转正结果（调用方可忽略）；失败时 CommitResult.ok=false。
    // REQ-EXT-PERF-002：本函数不再在主线程复算整文件——校验通过即投递 StagingHashTask
    // 并返回 CommitResult.deferred=true；后半段（提交/群引用/会话落定/迟到应答）在
    // onStagingHashFinished 中完成。校验失败的即时拒绝路径 deferred=false，语义同改前。
    FileObjectStore::CommitResult finalizeUploadOnComplete();

    // REQ-EXT-PERF-002：复算落定入口（主线程）。ctx.deferredRespond 决定迟到应答：
    // 32 → 连接层补发 sendUploadAck；98/102 → 连接层 seedMsg 发出 respondPdu（所有权转移）。
    enum DeferredRespond {
        DeferredNone = 0,
        DeferredDataAck32 = 1,
        DeferredFastcheck98 = 2,
        DeferredGroupInit102 = 3,
        DeferredInit30 = 4    // REQ-RESUME-AUDIT-001 M3：29 的暂存已在尾部 → 30 按提交结果迟到补发
    };
    struct HashFinishAction {
        PDU* respondPdu = nullptr;   // 迟到补发的 98/102/30（已构造，发送并 memfree 由连接层负责）
        bool needDataAck = false;    // 迟到补发的 32
        bool ackSuccess = false;
        qint64 ackOffset = 0;
        bool ackComplete = true;     // REQ-RESUME-AUDIT-001 M3：32 的 complete 随提交结果
    };
    HashFinishAction onStagingHashFinished(quint64 transferId, bool hashOk,
                                           const QString& shaHex, qint64 sizeBytes);
    // 98 应答体构造（同步尾部与迟到路径共用，字段布局逐字节一致）。
    PDU* buildFastcheck98(bool ret, bool hit, qint64 fileId, qint64 sessionId, qint64 nextOffset,
                          const QByteArray& finalName, const QByteArray& reqIdBytes);
    // REQ-RESUME-AUDIT-001 M3：30 迟到补发构造（29 的暂存已在尾部路径），
    // 布局与 uploadfileinit 同步路径逐字节一致（code/[32..39]=offset/[40..43]=magic/[48..55]=tid）。
    PDU* buildUploadInit30(quint64 tid, bool ok, qint64 resumeOffset, bool resumeMagicOn);
    // 102 应答体构造（信封 + caData 回带 transfer_id，同 groupFileRequest 内 response()）。
    PDU* buildGroupInit102(quint64 requestId, quint64 groupId, int code,
                           const QByteArray& body, quint64 transferId);

    // REQ-FS-003: 统一路径沙箱校验（集中一处，所有路径入口调用）
    // 根与路径分别做 canonical 归一（解析 ../ 与符号链接）后判前缀；
    // 仅当 根存在 且 路径存在 且 canonical(path) 落在 canonical(root) 内（含根本身）时返回 true。
    // root 为空默认整个 filesPath；movefile/listdir 传入用户目录根以保持用户级隔离。
    static bool isPathInSandbox(const QString &path, const QString &root = QString());
};

#endif // MSGHANDLER_H
