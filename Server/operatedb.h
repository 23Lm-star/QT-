#ifndef OPERATEDB_H
#define OPERATEDB_H

#include <QObject>
#include <QByteArray>
#include <QDateTime>
#include <QStringList>

// OperateDB：服务端 MySQL 访问的单例入口（用户/好友/消息/分享链接/上传会话/群组/群消息/群文件）。
// 每项操作从全局连接池取一条连接，作用域结束自动归还；失败用返回值/空值表达，不抛异常。
// 只有 SQL 或连接异常写日志；业务性失败（用户名不存在、密码错等）由调用方按返回码判定。
//数据库操作类
class OperateDB : public QObject
{
    Q_OBJECT
public:
    // 销毁单例；连接池在 DatabaseConnectionPool 析构时统一关闭
    ~OperateDB();
    //管理/维护一个数据库的连接
    void connect();
    static OperateDB& instance();//单例模式
    //REQ-EXT-FILE-001 T1: 文件资产 schema 版本（D1 启动 fail-fast，不静默建表）
    static const int kFileAssetSchemaVersion = 2026092901;
    // REQ-EXT-GROUP-001：群生命周期迁移版本；启动时要求数据库至少达到此版本。
    static const int kGroupSchemaVersion = 2026093001;
    // REQ-EXT-GROUP-002：群消息迁移版本（group_info.last_seq + group_message 表）。
    static const int kGroupMessageSchemaVersion = 2026100101;
    // REQ-EXT-FILE-002：秒传预检查迁移版本（upload_session.client_request_id/file_id + uk_req）。
    // 取独立版本号：与 GROUP-002 的 2026100101 同名会让"未执行 FILE-002 迁移"的库通过启动检查，
    // fail-fast 失效（见 development 节点实测记录）。
    static const int kFastUploadSchemaVersion = 2026100102;
    // REQ-EXT-GROUP-003：群文件迁移版本（group_file 表）。
    static const int kGroupFileSchemaVersion = 2026100103;
    // REQ-EXT-RECLAIM-001：周期清理调度器迁移版本（file_object.reclaim_at +
    // status 扩 'reclaiming' + idx_status_reclaim + group_file.object_id 放宽为可空）。
    // 必须有独立版本号：调度器与 4 处引用归零打标都直接写 reclaim_at，
    // 未执行本迁移的库若靠旧版本号蒙过启动检查，会在第一次释放文件时 SQL 报错。
    static const int kReclaimSchemaVersion = 2026100701;
    //启动检查：schema_version 达标返回 true；库不可达/表缺失/版本低返回 false 并打日志
    bool checkSchemaVersion();

    // REQ-EXT-SESSION-002：启动期在线态对账——把上一个进程遗留的 online=1 全部归零。
    // 依据：online 的唯一权威来源是本进程的活连接（断开即由 handleOffLine 归零），
    // 启动瞬间本进程一条连接都没有，故库里残留的 online=1 一律是幽灵。
    // 单实例假设：本项目单进程单端口单库；若将来多实例共享一库，全量归零会误清另一实例的在线用户，
    // 那时必须改成"按实例/租约"的形态，不能沿用本函数。
    // 返回是否成功；失败时由调用方决定是否阻止启动（main 当前按 fail-fast 处理）。
    bool reconcileOnlineFlags();

    // 群请求操作编号（业务层内部枚举，不等同于网络 PDU 编号）。
    enum GroupOperation {
        GroupCreate = 1, GroupInvite, GroupAcceptInvitation, GroupRejectInvitation,
        GroupLeave, GroupRemoveMember, GroupSetRole, GroupTransferOwner,
        GroupDissolve, GroupList, GroupListMembers, GroupQueryInvitations,
        GroupSummary,
        // REQ-EXT-GROUP-002：群消息（发送/拉取/已读/未读）
        GroupSendMessage, GroupPullMessages, GroupReadConfirm, GroupUnreadSummary
    };
    // 群操作入参：字段按 operation 取用，其余保持默认值（0/空串即"未提供"）
    struct GroupRequest {
        GroupOperation operation;
        quint64 groupId;             // 群标识
        quint64 invitationId;        // 邀请标识
        QString groupName;           // 群名称
        QString targetName;          // 目标用户
        QString requestKey;          // 幂等键
        QString role;                // 角色 owner/admin/member
        // REQ-EXT-GROUP-002 字段
        QString clientMsgId;         // 发送幂等键（非空，<=64B）
        QString content;             // 消息正文（<=2000B UTF-8，非空）
        quint8 pullMode = 0;         // 0 前向补齐 / 1 后向翻页
        quint64 cursorSeq = 0;       // 拉取游标 seq
        quint16 limit = 50;          // 拉取条数（已钳制 1-100）
        quint64 readSeq = 0;         // 已读确认目标 seq（服务端单调钳制）
    };
    // REQ-EXT-GROUP-002：需要在线广播（PDU 90）的群消息记录
    struct GroupMessageRecord {
        // 群内单调序号（group_info.last_seq 分配）
        quint64 seq = 0;
        // group_message 自增主键
        quint64 msgId = 0;
        // 发送者账号；系统消息为触发者
        QString sender;
        quint8 msgType = 0;          // 0 文本 / 1 系统
        // 服务端落库时间（毫秒，取 MySQL created_at）
        quint64 createdAtMs = 0;
        QString content;
    };
    // 群操作统一结果：code=0 成功；responseBody 已按协议编码，调用方无需再拼字段
    struct GroupResult {
        int code;                    // 0 成功，负数为业务错误
        QByteArray responseBody;     // 已编码的响应业务字段
        QStringList notifyUsers;     // 需要接收成员事件的账号
        quint64 groupId;             // 事件所属群
        quint64 version;             // 提交后的群版本
        int eventType;               // 0 无事件，1 成员变化，2 邀请变化
        GroupMessageRecord message;  // REQ-EXT-GROUP-002：广播用消息记录
        bool broadcastMessage = false; // true=提交成功后广播（幂等重放=false）
    };
    // 在单个数据库事务中执行群状态变更或隔离查询；actor 来自连接认证上下文。
    GroupResult handleGroupOperation(const GroupRequest &request, const QString &actor);
    // REQ-EXT-GROUP-002：群消息操作（发送/拉取/已读确认/未读摘要）；actor 来自连接认证上下文。
    GroupResult handleGroupMessageOperation(const GroupRequest &request, const QString &actor);

    // ===== REQ-EXT-GROUP-003：群文件目录与访问控制 =====
    // 权限事实源与 GROUP-001/002 同源：group_info.status + group_member.status='active'。
    // 全部判定在服务端、每请求即时执行（退出/被移除即时失权），客户端不自报身份、不缓存授权。
    enum GroupFileOperation { GroupFileList = 1, GroupFileUploadInit, GroupFileDelete, GroupFileDownload };
    // 群文件入参/出参合体：出参字段仅在命中或 Download 时被填写
    struct GroupFileRequest {
        GroupFileOperation operation = GroupFileList;
        quint64 groupId = 0;         // 信封 group_id（目标群）
        quint64 groupFileId = 0;     // Delete/Download 的群文件引用
        quint32 offset = 0;          // List 分页起点
        quint16 limit = 50;          // List 每页条数（调用方已钳 1-200）
        qint64 fileSize = 0;         // UploadInit 声明大小
        QString shaHex;              // UploadInit 摘要（严格 64 小写 hex，调用方已校验）
        QString displayName;         // UploadInit 请求名；出参=服务端最终群展示名
        // 出参
        qint64 hostFileId = -1;      // UploadInit 命中：上传者 user_file.file_id
        qint64 objectId = -1;        // 命中/Download：物理对象
        qint64 sizeBytes = -1;       // Download：文件大小
    };
    // 群文件结果：hit 只对 UploadInit 有意义（true 表示零正文秒传）
    struct GroupFileResult {
        int code = -2000;            // 0 成功，负数为业务错误
        QByteArray responseBody;     // List/Download 的业务体（不含信封与 code）
        bool hit = false;            // UploadInit：true=秒传命中（零正文）
        quint64 groupFileId = 0;     // UploadInit 命中时的群文件引用
    };
    // P0-P7 判定点在同一事务/同一入口内按序执行；失败码段 -2xxx 与群消息同源。
    GroupFileResult handleGroupFileOperation(GroupFileRequest &request, const QString &actor);
    //REQ-EXT-FILE-001 T6：存量文件启动自动迁移（幂等：file_migration_map 已有记录即跳过；
    //失败记 failed 继续其余，绝不删除旧文件——验收5）。main 在版本检查通过后调用一次。
    void migrateLegacyFiles();
    //REQ-EXT-FILE-001 F-3/D5 修复：存量"绝对机器路径"键 → owner 相对逻辑键的一次性确定性转换
    //（幂等可重放；uk_loc 冲突按 file_id 最早保留，后到 active 行软删+减引用）。
    //main 在 checkSchemaVersion 后、migrateLegacyFiles 前调用。
    void normalizeLogicKeys();
    //执行注册用户操作函数
    // 注册新用户：用户名已存在直接失败；成功时生成随机盐并以 md5(客户端哈希+盐) 落库
    bool handleRegit(char* strname,char*strpwd);
    //登录用户
    // REQ-EXT-SESSION-001：clientId=登录携带的客户端唯一标识（可空）；成功(ret==1)时
    // outVersion=本次登录在事务内原子分配的用户域版本号
    int handleLogin(char*strname,char*strpwd,const QString& clientId,quint64& outVersion);
    //下线
    // 断线时把 online 置 0；SQL 失败静默（启动期 reconcileOnlineFlags 兜底）
    void handleOffLine(const char* strname);
    //查找用户
    // 返回 online（0 离线/1 在线）；用户不存在返回 2；参数或连接失败返回 -1
    int handleFindUser(const char* userName);
    //获取所有用户
    // 返回在线、非自己且尚未成为好友的用户名（用于陌生人列表）
    QStringList handleOnlineUsers(const QString& strname);
    //添加好友操作，查找
    // 目标在线返回 1，离线/不存在返回 0，参数或连接失败返回 -1
    int handleAddFriend(char* usrname,char* tarname);
    //添加好友操作，添加
    // 单条 INSERT，只写一个方向；双向关系由调用方对调参数再调一次
    int handleAddFriendFin(char* usrname,char* tarname);
    //刷新好友操作，
    // 返回与 strname 相关的双向好友名（UNION 去重）
    QStringList handleFlushFriend(const QString&strname);
    //删除好友操作
    // 事务内删除两个方向的好友行
    bool handleDeleteFriend(char*userName,char*tarName);
    //记录聊天消息信息
    // 写入私聊消息；issuccess 记录接收方当时是否在线；返回接收方是否在线
    // 返回值=接收方当时是否在线（沿用改前语义，决定要不要实时转发）；
    // storedOut 带回 INSERT 是否成功——REQ-CLI-MSGSTORE-001 的 16 应答以它为准，
    // 「发送成功」指服务端已入库，与对方在不在线无关。可不传（其它调用方不受影响）。
    bool seedchatMsg(char*seedName,char *revName,QString msg,QString seedTime,bool *storedOut=nullptr);
    //获取聊天信息
    // 事务内 SELECT ... FOR UPDATE 未送达消息并标记 issuccess=1；
    // 返回扁平列表（每行依次为 msg、seed_time）；标记失败时返回空列表以便重投
    QStringList getchatMsg(char*seedName,char *revName);
    //获取用户信息
    // 按名查询，命中时回填规范用户名与 online 标志
    bool handleGetUserInfo(const char* userName, QString& outName, int& outOnline);
    //注销/删除用户账户
    // 校验密码后单事务级联删除消息、好友、用户及分享/外链/登录档案；
    // 后三类清理失败不回滚（表可能尚未迁移）
    bool handleLogoff(const char* name, const char* pwd);
    //分享链接操作
    // 写入短码外链记录（publish_links），失败返回 false
    bool createPublishLink(const QString &code, const QString &path, const QString &creator);
    // 短码查文件路径；未命中或连接失败返回空串
    QString getFilePathByCode(const QString &code);
    
    // 新增：安全外链分享操作（BUG-ARC-PUBLINK-001）
    // token_hash 为随机令牌的哈希；同 owner+path 已有记录时直接复用，返回 true
    bool createShareLink(const QString &hash, const QString &owner, const QString &path, const QDateTime &expire);
    // 分享链接行：路径为相对 owner 根目录的逻辑键，expire_time 到点即失效
    struct ShareLinkInfo {
        // 链接创建者
        QString ownerName;
        // 相对 owner 根的逻辑路径
        QString relativePath;
        // 过期时间（库内为秒精度 DATETIME）
        QDateTime expireTime;
        // 是否已作废
        bool revoked;
        QString tokenHash;      // REQ-EXT-SHARE-001：按路径取信息时回带，供复用/续期拼链接
    };
    // 按 token 哈希精确查找；未命中返回 false 且不改 outInfo
    bool getShareLinkByHash(const QString &hash, ShareLinkInfo &outInfo);
    // 按 owner+path 取最新一行的 token_hash；未命中/连接失败返回空串
    QString getShareLinkByPath(const QString &owner, const QString &path);
    // REQ-EXT-SHARE-001：按 owner+path 取最新一行的完整信息（含 token_hash）
    bool getShareLinkInfoByPath(const QString &owner, const QString &path, ShareLinkInfo &outInfo);
    // REQ-EXT-SHARE-001：续期——只改 expire_time，不动 token/相对路径/撤销位；返回是否命中行
    bool renewShareLink(const QString &owner, const QString &path, const QDateTime &newExpire);
    // REQ-EXT-SHARE-001：重新生成前作废旧行（精确路径，不含子路径）
    bool removeShareLinkExact(const QString &owner, const QString &path);

    // 跨功能联动维护（文件删除/重命名/移动 → 分享链接记录一致性）
    // 删除 path 自身及其子路径（path/%）的全部链接记录
    bool deleteShareLinkByPath(const QString &owner, const QString &path);
    // 把 oldPath 自身与其子路径整体平移到 newPath（两条 UPDATE，任一失败返回 false）
    bool updateShareLinkPath(const QString &owner, const QString &oldPath, const QString &newPath);
    // 删除好友时清理双方私聊记录
    bool deleteFriendMessages(const QString &userA, const QString &userB);

    // ===== REQ-EXT-FILE-001 T4：上传会话持久化（upload_session 表） =====
    // 登记暂存会话；返回 session_id（<0 失败）。ttlSec<=0 时默认 3600。
    // REQ-EXT-FILE-002：clientRequestId 非空则落请求级幂等键（uk_req），旧调用方传空＝不写该列。
    qint64 createUploadSession(const QString &owner, const QString &parentPath,
                               const QString &displayName, qint64 expectedSize,
                               const QString &clientSha256, const QString &stagingPath,
                               qint64 confirmedOffset, int ttlSec,
                               const QString &clientRequestId = QString());
    // 持久化断点；仅 status='uploading' 的行会被更新，返回 SQL 是否执行成功
    bool updateUploadOffset(qint64 sessionId, qint64 confirmedOffset);
    // 完成/失败：写 status（done|failed|expired|verifying）；fileId>=0 时同写 file_id，
    // 使"已完成会话"能被请求级幂等短路直接命中（无需回退到内容预检查）。
    // REQ-EXT-TRANSFER-003 S10：confirmedOffset>=0 时同写 confirmed_offset（完成=expected_size，失败=0），
    // = -1（默认）表示不改该列（既有调用方行为不变）。
    bool finishUploadSession(qint64 sessionId, const QString &status, qint64 fileId = -1,
                             qint64 confirmedOffset = -1);

    // ===== REQ-EXT-FILE-002：请求级幂等短路的读侧（§2 步骤3） =====
    // parentPath/displayName/expectedSize/clientSha256 取自会话行（非本次请求），
    // 保证续传时 staging 令牌（owner|parent|display|size）与首次一致。
    struct UploadSessionLookup {
        bool found = false;
        qint64 sessionId = -1;
        QString status;              // uploading|verifying|done|failed|expired
        qint64 confirmedOffset = 0;
        qint64 fileId = -1;          // done 时的逻辑条目（无则 -1）
        QString displayName;         // 最终展示名：优先 user_file（含 (1) 后缀），否则请求名
        QString parentPath;          // 会话登记的逻辑父路径
        qint64 expectedSize = 0;
        QString clientSha256;
    };
    UploadSessionLookup lookupUploadSessionByRequestId(const QString &owner,
                                                       const QString &clientRequestId);

    // 失败/过期/半途会话以同一请求键重试：原地重置为 uploading（避免 uk_req 唯一键冲突）。
    // REQ-EXT-TRANSFER-003 S14：clientRequestId 非空时同改请求键（97 按暂存键复用行的收敛规则）。
    bool restartUploadSession(qint64 sessionId, const QString &parentPath,
                              const QString &displayName, qint64 expectedSize,
                              const QString &clientSha256, const QString &stagingPath,
                              qint64 confirmedOffset, int ttlSec,
                              const QString &clientRequestId = QString());

    // REQ-EXT-TRANSFER-003 S12/D7：按 (owner, staging_path) 找可复用的未完成会话行——
    // 暂存路径是"同一逻辑上传"的确定性身份（sha1(owner|parent|display|size) 产物），
    // 状态白名单排除 done/cancelled（done 由请求级幂等短路处理）。命中返回 session_id，否则 -1。
    qint64 findReusableUploadSession(const QString &owner, const QString &stagingPath);

    // 秒传命中：登记 status='done' 的会话行（零正文传输的完成态），返回 session_id（<0 失败）。
    qint64 insertCompletedUploadSession(const QString &owner, const QString &parentPath,
                                        const QString &displayName, qint64 expectedSize,
                                        const QString &clientSha256,
                                        const QString &clientRequestId, qint64 fileId);

    explicit OperateDB(QObject *parent = nullptr);
    OperateDB(const OperateDB&)=delete;
    OperateDB& operator=(const OperateDB&)=delete;
signals:

};

#endif // OPERATEDB_H
