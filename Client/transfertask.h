// 传输任务管理模块：文件级任务的状态机、有限并发调度、断点续传与快照持久化。
// 上传/下载各自独立计名额；任务用稳定行键 key 标识，wire 上用 transfer_id 与 PDU 字段映射。
#ifndef TRANSFERTASK_H
#define TRANSFERTASK_H

#include "protocol.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QWidget>

class QFile;
class QPushButton;
class QTableWidget;
class QLabel;
class Uploader;

// REQ-EXT-TRANSFER-001：客户端多文件有限并发的任务管理与调度（§5）。
// 传输单元 = 文件级任务；上传/下载两个方向各 3 个名额（D6），超限排队（不报错不丢文件）。
class TransferManager : public QObject
{
    Q_OBJECT
public:
    // 任务来源：个人/群 与 上传/下载 的组合
    enum Kind { PersonalUpload, GroupUpload, PersonalDownload, GroupDownload };
    // 任务状态机：Queued→Hashing→Running→终态；Paused 可经 resume 回 Queued
    enum State { Queued, Hashing, Running, Paused, Done, Failed, Cancelled };

    // 单个文件传输任务的全部上下文（UI 数据 + 协议字段 + 本地文件句柄 + 上传句柄）
    struct Task {
        quint64 key = 0;         // 稳定行键（UI 行标识；续传换 id 不变）
        quint64 id = 0;          // 当前 wire transfer_id
        Kind    kind = PersonalUpload;
        State   state = Queued;
        QString localPath;       // 上传源路径
        QString displayName;     // 文件展示名
        QString dirPath;         // 个人上传目标目录（服务端路径）
        QString serverPath;      // 个人下载源（服务端路径）
        quint64 groupId = 0;     // 群作用域
        quint64 groupFileId = 0; // 群下载源
        // 文件总字节数（服务端回写权威值）
        qint64  totalBytes = 0;
        // 已完成字节；下载续传起点以其分片物理大小为准
        qint64  doneBytes = 0;
        // 失败原因（面板展示；继续/重试时清空）
        QString failReason;
        QByteArray shaHex;       // 群上传预检查
        bool    hashPending = false;
        // 下载分片句柄（续传时重开并 seek）
        QFile*  recvFile = nullptr;
        // 下载分片落盘路径（恢复续传的唯一依据）
        QString savePath;
        // 上传正文发送器（INIT 受理后创建）
        Uploader* uploader = nullptr;
        bool    needFastcheck = true;   // 个人上传首次走 97 预检查；续传跳过（S3）
        bool    wasRestored = false;    // 快照恢复的任务：首次继续仍走 97（服务端 staging 可能已变）
        QString fastcheckToken;         // AUD-3：本次 97 的请求标识（服务端 98 回带，用于按标识归属）
        bool isUpload() const { return kind == PersonalUpload || kind == GroupUpload; }
    };

    // 进程内单例（GUI 线程使用）
    static TransferManager& instance();

    // 分配下一个 wire transfer_id（单调递增，不复用）
    quint64 allocId();

    // 入队（返回任务行键）
    quint64 enqueuePersonalUpload(const QString& localPath, const QString& dirPath, const QString& displayName);
    quint64 enqueueGroupUpload(quint64 groupId, const QString& localPath, const QString& displayName, const QByteArray& shaHex);
    quint64 enqueuePersonalDownload(const QString& serverPath, const QString& displayName);
    quint64 enqueueGroupDownload(quint64 groupId, quint64 groupFileId, const QString& displayName);

    // 按键取任务；不存在返回 nullptr
    Task* taskByKey(quint64 key) { return m_tasks.contains(key) ? &m_tasks[key] : nullptr; }
    // 全部稳定行键（升序，用于面板逐行刷新）
    QList<quint64> taskKeys() const;

    // 路由（由 Client::handlMsg 调用）
    void onFastcheckRespond(PDU* pdu);         // 98（个人上传预检查；无 transfer_id，按单飞槽归属）
    void onUploadInitRespond(PDU* pdu);        // 30
    void onUploadDataRespond(PDU* pdu);        // 32
    void onGroupUploadInitRespond(PDU* pdu);   // 102
    void onDownloadRespond(PDU* pdu);          // 41
    void onDownloadData(PDU* pdu);             // 42
    void onDownloadFinish(PDU* pdu);           // 43
    void onGroupDownloadRespond(PDU* pdu);     // 106
    void onAbortRespond(PDU* pdu);             // 108

    // 控制（上传/下载两方向可用；REQ-EXT-TRANSFER-002 起下载支持暂停/继续/取消/续传）
    void pause(quint64 key);
    void resume(quint64 key);
    void cancel(quint64 key);
    void retry(quint64 key);

    // 41 的 id 落在独立偏移（D2），其余传输族落在统一偏移
    static quint64 transferIdOf(const PDU* pdu);
    static QString stateText(State s);
    static bool isUploadKind(Kind k) { return k == PersonalUpload || k == GroupUpload; }

    void showPanel();

    // ===== 会话间持久化：按用户保存未完成任务，二次登录恢复 =====
    // 登录成功时调用。D-b：不清表重建——同用户（含 空名→真名）保持内存任务并按键合并快照
    // （内存为准、快照补缺）；只有确实是另一个账号时才丢弃上一个账号的内存任务。
    void setCurrentUser(const QString& name);
    // 程序退出前落盘（把内存中非终态任务写进快照）
    void saveSnapshotNow();
    // 本次会话恢复出的任务数（面板提示用）
    int restoredCount() const { return m_restoredCount; }
    QString currentUser() const { return m_currentUser; }

    // 97 预检查为连接级单飞（服务端旧槽）：File 计算完 SHA 后回调提交，按序发送
    void submitFastcheck(quint64 key, const QByteArray& shaHex);
    void failTask(quint64 key, const QString& reason);

    // ===== REQ-EXT-TRANSFER-003 G7：断线任务态收敛（纯客户端，不涉协议） =====
    // 与服务端断开：在途（Running/Hashing）任务即刻失败并归还名额；排队/暂停保持原态；
    // 不删本地分片、不动服务端暂存（留给"重试即续传"）。
    void onConnectionLost(const QString& reason);
    // 重连并重新登录后恢复调度（由 Client 的 LOGIN_RESPOND 分支调用）
    void onConnectionRestored();

signals:
    void tasksChanged();
    void hashDue(quint64 key, const QString& localPath);   // M5：请求后台计算 SHA-256
    void fastcheckDue(quint64 key);                        // 轮到此任务发送 97 预检查

private:
    TransferManager();
    // 在名额允许时从排队列表启动任务（上传/下载两方向独立）
    void pump();
    // 单飞推进 97 预检查队列（同一时刻最多一个在途）
    void pumpFastcheck();
    // 按任务类型进入对应启动路径（摘要计算 / INIT / 下载请求）
    void startTask(Task& t);
    void startGroupHash(Task& t);          // M5：群上传 SHA-256 在后台线程计算
    // 发送群上传 INIT（101，群信封 + 摘要）
    void sendGroupUploadInit(Task& t);
    // 发送个人上传 INIT（29，caData 含大小/续传魔数/transfer_id）
    void sendPersonalUploadInit(Task& t);
    // 收敛到终态：归还名额、释放句柄、摘 id 映射、落快照并继续调度
    void finishTask(Task& t, State s, const QString& reason = QString());
    // 关闭并释放下载分片句柄（不删除分片文件）
    void closeRecv(Task& t);
    // 发送 107 中止/暂停请求（携带 transfer_id）
    void sendPause(quint64 id);
    // 仅在 Running 时发送中止，避免对未开始任务打扰服务端
    void sendAbortIfActive(Task& t);
    // 由 wire transfer_id 反查任务（经稳定键中转）
    Task* taskByTransferId(quint64 id);
    // 新建下载分片（yyyy-MM 目录、(N) 去重）并立即落快照
    bool beginReceiving(Task& t, const QString& fileName);
    // REQ-EXT-TRANSFER-002 C5：续传 = 重开同一分片并 seek 到声明起点（不新建 (N) 文件）
    bool resumeReceiving(Task& t, qint64 offset);
    // REQ-EXT-TRANSFER-002 C2：下载「继续/重试」共用路径（摘旧映射 + 起点 = 分片大小）
    void resumeDownload(Task& t);
    // wire id → 稳定键；未知 id 返回 0
    quint64 stableKeyOf(quint64 transferId);

    QHash<quint64, Task> m_tasks;        // key = 稳定行键
    QHash<quint64, quint64> m_idToKey;   // transfer_id → 稳定行键
    QList<quint64> m_uploadQueue;        // 待启动的任务行键（FIFO）
    // 下载方向排队
    QList<quint64> m_downloadQueue;
    // 在途上传数（含 Hashing，上限 3）
    int m_activeUploads = 0;
    // 在途下载数（上限 3）
    int m_activeDownloads = 0;
    // transfer_id 发号器
    quint64 m_nextId = 1;
    // 稳定行键发号器
    quint64 m_nextKey = 1;
    QWidget* m_panel = nullptr;
    QList<quint64> m_fastcheckQueue;      // 已算出摘要、待发 97 的任务键
    quint64 m_fastcheckInFlight = 0;      // 正在等待 98 的任务键（0 = 无）
    QString m_fastcheckInFlightToken;     // AUD-3：在途 97 的请求标识（与 98 回带比对；不匹配 = 迟到，丢弃）
    bool m_connectionDown = false;        // G7：与服务端断开期间不启动新任务（重连登录后复位）

    // 持久化（transfers_<user>.json，AppLocalDataLocation）
    // 序列化非终态任务
    void saveSnapshot();
    // 登录后读取并按身份合并
    void restoreSavedTasks();
    // AppLocalDataLocation/transfers_<safe>.json
    QString snapshotPath(const QString& user) const;
    // 当前已登录用户名
    QString m_currentUser;
    QString m_lastKnownUser;              // D-a：最近一次有效登录名（m_currentUser 未就位时快照的兜底键）
    QString m_restoredUser;               // 已为本用户恢复过快照（防断线重连重复恢复）
    // 本次会话恢复出的任务数
    int m_restoredCount = 0;
};

// 文件级任务列表窗口（§5.5；M6：并发时不再用逐文件 QMessageBox）
class TransferTaskPanel : public QWidget
{
    Q_OBJECT
public:
    explicit TransferTaskPanel(QWidget* parent = nullptr);
public slots:
    // 按 TransferManager 当前任务重建表格（节流由 tasksChanged 驱动）
    void refresh();
private slots:
    void onPause();
    void onResume();
    void onCancel();
    void onRetry();
private:
    // 当前选中行的稳定键（第 0 列 UserRole 保存）；无选中返回 0
    quint64 selectedKey() const;
    // 文件/方向/进度/状态/失败原因 五列表
    QTableWidget* m_table;
    QPushButton* m_pauseBtn;
    QPushButton* m_resumeBtn;
    QPushButton* m_cancelBtn;
    QPushButton* m_retryBtn;
    // 顶部说明/恢复提示
    QLabel* m_hint;
};

#endif // TRANSFERTASK_H
