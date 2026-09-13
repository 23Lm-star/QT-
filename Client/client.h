#ifndef CLIENT_H
#define CLIENT_H
#include "msghandler.h"
#include "protocol.h"
#include "reconnectdialog.h"
#include <QWidget>
#include <QTcpSocket>
#include <QTime>
#include <QtGlobal>
#include <QPropertyAnimation>
#include <QTimer>
#include <QLabel>
#include "userinformation.h"
QT_BEGIN_NAMESPACE
namespace Ui { class Client; }
QT_END_NAMESPACE

class Client : public QWidget {
    Q_OBJECT

public:

    //处理和服务器的交互和连接的对象 （QTcpSocket类）
    QTcpSocket sorket;
    //连接的服务器的ip地址和端口号
    QString m_strlp;
    quint16 m_usPort;
    //记录用户信息
    QString m_LoginName;
    QString m_strRootPath;
    //进行信息处理的对象 指针
    MsgHandler* m_mhptr;
    QByteArray m_pdulist;
    //加载配置文件
    void loadconfig();
    //架构函数 静态 调用 构建唯一实例
    static Client& getinstance();
    //进行验证码刷新的函数
    void flus();
    QString generateCaptcha(int length = 6);
    //进行信息处理的函数
    void handlMsg(PDU* &pdu);
    //登录/注册成功动画
    void showLoadingAnimation(bool success);
    void updateCaptchaDisplay(const QString& code);
    void resetCaptchaButton();

    QTimer* m_captchaCooldownTimer;
    int m_captchaCooldownSec;
    bool m_captchaLocked;   // 账号已锁定（10分钟）：注册/登录/获取验证码均在客户端拦截
    void startCaptchaCooldown(int sec, bool locked = false);
    void onCaptchaCooldownTick();
    void sendHeartbeat();
    // 心跳开关（调试/模拟僵尸用，true=正常发PING，false=停发模拟假死）
    static bool s_heartbeatEnabled;

    // 服务端存活检测（复用心跳 PING/PONG）：失联时弹出重连窗口
    static const qint64 SERVER_ALIVE_TIMEOUT_MS;  // 90s = 3×30s 心跳周期
    void onServerLost();

    ~Client();
public slots:
    void seedMsg(PDU* pdu);
    //连接服务器的函数
    void connectsort();
    //接受服务器信息调用槽函数
    void recvmsg();
    //发送注销请求
    void sendLogoffRequest();
private slots:
    //注册消息发送槽函数
    void on_register_PB_clicked();
    //登录消息发送槽函数
    void on_login_PB_clicked();
    //生成随机验证码槽函数
    void on_pushButton_clicked();
    //socket 断开信号（服务端 abort / 网络中断）
    void onSocketDisconnected();
    //重连窗口"重新连接"按钮 → 发起重连
    void onReconnectRequested();
    //重连成功后强制回到登录页（服务端重启后需重新鉴权）
    void forceReLogin();

private:
    Ui::Client *ui;
    QTimer* m_heartbeatTimer;
    // 服务端存活检测相关
    bool m_serverLostShown;     // 重连窗口是否已弹出（去重，避免重复弹）
    qint64 m_lastPongMs;        // 最后收到 PONG 的时间(ms)
    bool m_loggedIn;            // 是否已登录（未登录不弹窗，避免登录界面误报）
    ReconnectDialog* m_reconnectDlg;
    //构造函数设定为私有
    //拷贝构造函数和拷贝赋值函数删除
    Client(QWidget *parent = nullptr);
    Client(const Client& other)=delete;
    Client& operator=(const Client&)=delete;
};
#endif // CLIENT_H
