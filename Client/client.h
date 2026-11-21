#ifndef CLIENT_H
#define CLIENT_H
// 客户端主窗口：单例，负责登录/注册页、与服务端的 TCP 连接与收发。
// - 一个 QTcpSocket 长连接；心跳定时器每 30s 发 PING，90s 未收到 PONG 判定失联。
// - recvmsg() 负责从字节流中拆出完整 PDU（处理粘包/半包），再按消息类型分发。
// - 登录成功后隐藏本窗口，显示 Index 主界面；重连后强制回到登录页。
#include "groupchatstate.h"
#include "messagestore.h"
#include "msghandler.h"
#include "protocol.h"
#include "reconnectdialog.h"
#include <QHash>
#include <QWidget>
#include <QSslSocket>
#include "tlspinning.h"
#include <QTime>
#include <QtGlobal>
#include <QPropertyAnimation>
#include <QTimer>
#include <QLabel>
QT_BEGIN_NAMESPACE
namespace Ui { class Client; }
QT_END_NAMESPACE

// 客户端主窗口单例；构造即建连，生命周期贯穿整个进程。
class Client : public QWidget {
    Q_OBJECT

public:

    //处理和服务器的交互和连接的对象 （REQ-EXT-TLS-001 起为 QSslSocket；明文连接行为不变）
    // 必须留在 GUI 线程：readyRead/disconnected 槽都直接操作控件。
    QSslSocket sorket;
    // REQ-EXT-TLS-001：true=本次连接走 TLS，握手完成前不收发 PDU。
    bool m_tlsMode = false;
    // REQ-EXT-TLS-001：TLS 连接已建立 TCP 但握手未完成（此期间禁止写出 PDU）。
    bool tlsPending() const { return m_tlsMode && !sorket.isEncrypted(); }
    // D-1：本次断线的服务端可见原因，决定要不要弹「账号可能已在其他设备登录」。
    tls::DisconnectCause m_disconnectCause = tls::DisconnectCause::Other;
    //连接的服务器的ip地址和端口号
    QString m_strlp;
    quint16 m_usPort;
    //记录用户信息
    // 当前会话登录名，登录响应同步写入
    QString m_LoginName;
    // 服务端文件存储根（配置第 3 行），注册时用于建用户目录
    QString m_strRootPath;
    // REQ-EXT-SESSION-001：持久化客户端唯一 ID（32hex，随登录负载附加区发送）
    QString m_clientId;
    // ===== REQ-AUTH-CONNECTION-001：会话恢复凭据（内存持有，QSettings 落盘） =====
    // 登录成功时服务端一次性回传；恢复成功沿用原 token。退出/注销/被踢/恢复失败即清除。
    // 不落日志、不进错误提示。
    QString m_resumeToken;
    // 凭据归属账号（恢复响应回带的用户名；登录成功即登录名）
    QString m_tokenUser;
    // 121 恢复请求已发出、等待 122（期间到达的断线信号不弹重连窗）
    bool m_resumePending = false;
    // 恢复凭据持久化（账号+token 成对写入；格式非法按无凭据处理）
    void saveResumeToken(const QString& user, const QString& token);
    // 清除内存与落盘凭据（幂等）
    void clearResumeToken();
    // 启动时读回凭据；格式非法即丢弃并清落盘
    void loadResumeToken();
    // 发送 119 退出登录请求（连接保持匿名，由 120 响应完成本地状态清理）
    void sendSessionLogout();
    // 重连打开后优先发 121 恢复（无凭据/格式非法直接走重新登录）
    void tryResumeSession();
    // 123 失效事件处理：清凭据、停业务、存传输快照、回登录页并显示原因
    void handleSessionInvalidated(PDU* pdu);
    // 会话建立后的统一入口（登录成功/恢复成功共用）：切主界面、恢复传输、声明能力、拉申请
    void enterLoggedInSession(const QString& user);
    // 注销成功后复位登录监控（m_loggedIn=false，避免随后主动断线弹重连窗）
    void logoutLocalStateReset() { m_loggedIn = false; m_resumePending = false; }
    //进行信息处理的对象 指针
    // 裸指针，进程内常驻不释放（随单例一起活到退出）。
    MsgHandler* m_mhptr;
    // 接收缓冲区：保存尚未凑成完整 PDU 的半包字节（TCP 流式，无消息边界）。
    QByteArray m_pdulist;
    //加载配置文件
    // 从 Qt 资源 :/connect.config 读取三行：服务器 IP、端口、存储根路径。
    void loadconfig();
    //架构函数 静态 调用 构建唯一实例
    // 进程内首次调用触发构造（同时建立 TCP 连接），之后返回同一对象。
    static Client& getinstance();
    // REQ-UX-001 A5：按 2 倍基准放大窗口尺寸，并收敛到可用屏幕内（避免窗口超出显示器）
    static QSize scaledWindowSize(int baseW, int baseH);
    //进行验证码刷新的函数
    // 清空登录页的账号与密码输入框（验证码占位提示由服务端刷新）。
    void flus();
    // 生成随机验证码字符串；当前服务端下发验证码，客户端保留此工具函数。
    QString generateCaptcha(int length = 6);
    //进行信息处理的函数
    // PDU 分发入口：按 uiType 调用 MsgHandler 或 TransferManager/Index 的处理器。
    // 注意：pdu 指向 m_pdulist 内部；处理期间不得再次进入 recvmsg 修改缓冲。
    void handlMsg(PDU* &pdu);
    //登录/注册成功动画
    // 当前为空实现（预留的加载动画钩子）。
    void showLoadingAnimation(bool success);
    // 把服务端下发的验证码显示为输入框占位提示。
    // REQ-AUDIT-001 R8：验证码改为图片，文本答案不再下发到客户端
    void updateCaptchaImage(const QByteArray& png);
    // 图片展示控件：懒创建，插入到"验证码"文字标签之后，不覆盖标签文字
    QLabel* m_captchaImageLabel = nullptr;
    // 冷却结束/解锁后恢复“获取验证码”按钮。
    void resetCaptchaButton();

    // 验证码冷却/锁定倒计时：每秒 tick 更新按钮文案。
    QTimer* m_captchaCooldownTimer;
    // 剩余秒数；>0 时禁止再次请求验证码
    int m_captchaCooldownSec;
    bool m_captchaLocked;   // 账号已锁定（10分钟）：注册/登录/获取验证码均在客户端拦截
    // 登录失败（验证码错/过期）后置位：立即补拉一次；若服务端冷却中，倒计时归零自动再拉
    bool m_captchaAutoRefresh = false;
    // 启动倒计时；sec<=0 时普通冷却取 60s、锁定取 600s。locked=true 同时禁用注册/登录。
    void startCaptchaCooldown(int sec, bool locked = false);
    // 定时器槽：递减剩余秒数，归零时恢复按钮并解除锁定。
    void onCaptchaCooldownTick();
    // 登录验证码失败后自动换新验证码（见 m_captchaAutoRefresh）
    void requestCaptchaAutoRefresh();
    // 周期发送心跳 PING，并据此判断服务端是否失联。
    void sendHeartbeat();
    // 心跳开关（调试/模拟僵尸用，true=正常发PING，false=停发模拟假死）
    static bool s_heartbeatEnabled;

    // 服务端存活检测（复用心跳 PING/PONG）：失联时弹出重连窗口
    static const qint64 SERVER_ALIVE_TIMEOUT_MS;  // 90s = 3×30s 心跳周期
    // 心跳超时触发：收敛在途传输任务并弹出重连窗口（带去重）。
    void onServerLost();

    // REQ-EXT-GROUP-004：群读状态属于账号会话，不属于窗口。
    // 登录成功时清空重建并取一次锚点；窗口只是读写者，开关窗口不改变语义。
    // 按 group_id 取群读状态；首次访问会在哈希表中插入默认值。
    GroupChatState &groupState(quint64 groupId) { return m_groupStates[groupId]; }
    // 清空全部群的已读游标/水位（登录切换、重连、注销时调用）。
    void resetGroupStates() { m_groupStates.clear(); }

    // REQ-CLI-MSGSTORE-001：接收消息的本地存储（群一库、私聊一库，库内按 user 列隔离）。
    // 登录成功时 open，注销时 removeUser；库不可用时所有调用都是安全的空操作。
    MessageStore &msgStore() { return m_msgStore; }

    // 退出前落盘未完成传输快照，并释放 ui。
    ~Client();
public slots:
    // 发送 PDU：write 后立即 free，调用方不得再使用 pdu（所有权转移给本函数）。
    void seedMsg(PDU* pdu);
    //连接服务器的函数
    // QTcpSocket::connected 槽：重连成功时关闭重连窗并强制重新登录。
    void connectsort();
    // REQ-EXT-TLS-001：按 certs/server.crt 在位与否选择 TLS(主端口+1) 或明文(原端口) 发起连接。
    void startConnection();
    //接受服务器信息调用槽函数
    // QTcpSocket::readyRead 槽：累积字节并按 uiTotalLen 切出完整 PDU（粘包/半包处理）。
    void recvmsg();
    //发送注销请求
    // 密码确认 + 二次确认后发送 LOGOFF_REQUEST，成功后回登录页并断开连接。
    void sendLogoffRequest();
private slots:
    //注册消息发送槽函数
    // 校验账号/密码/验证码后组 REGIST_REQUEST（caData 前 32 字节名 + 后 32 字节 MD5）。
    void on_register_PB_clicked();
    //登录消息发送槽函数
    // 组 LOGIN_REQUEST，caMsg = 验证码\0 + 32 字节持久客户端 ID。
    void on_login_PB_clicked();
    //生成随机验证码槽函数
    // 冷却/锁定或未连接时直接拦截；否则发送 CAPTCHA_REQUEST。
    void on_pushButton_clicked();
    //socket 断开信号（服务端 abort / 网络中断）
    // 已登录时提示并弹出重连窗口；同时收敛在途传输任务。
    void onSocketDisconnected();
    //重连窗口"重新连接"按钮 → 发起重连
    // 若仍处于连接态先断开，重置 PONG 计时后重新 connectToHost。
    void onReconnectRequested();
    //重连成功后强制回到登录页（服务端重启后需重新鉴权）
    // 丢弃旧会话群读状态、清空登录页并提示用户重新登录。
    void forceReLogin();

private:
    // 由 setupUi 创建，析构时 delete
    Ui::Client *ui;
    QHash<quint64, GroupChatState> m_groupStates;   // REQ-EXT-GROUP-004：group_id → 群读状态
    MessageStore m_msgStore;                        // REQ-CLI-MSGSTORE-001：本地消息库
    // 30s 周期心跳，parent=this 自动释放
    QTimer* m_heartbeatTimer;
    // 服务端存活检测相关
    bool m_serverLostShown;     // 重连窗口是否已弹出（去重，避免重复弹）
    qint64 m_lastPongMs;        // 最后收到 PONG 的时间(ms)
    bool m_loggedIn;            // 是否已登录（未登录不弹窗，避免登录界面误报）
    // PDU 分发重入闸：模态弹窗会开嵌套事件循环，readyRead 重入 recvmsg 时只收不处理，
    // 防止嵌套 remove 使外层指向 m_pdulist 的裸指针失效（0xC0000374 堆损坏）
    bool m_dispatchingPdu = false;
    // 失联/被踢时显示的模态重连窗，parent=this
    ReconnectDialog* m_reconnectDlg;
    //构造函数设定为私有
    //拷贝构造函数和拷贝赋值函数删除
    Client(QWidget *parent = nullptr);
    Client(const Client& other)=delete;
    Client& operator=(const Client&)=delete;
};
#endif // CLIENT_H
