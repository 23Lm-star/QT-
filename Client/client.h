#ifndef CLIENT_H
#define CLIENT_H
#include "msghandler.h"
#include "protocol.h"
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
    void startCaptchaCooldown();
    void onCaptchaCooldownTick();

    ~Client();
public slots:
    void seedMsg(PDU* pdu);
    //连接服务器的函数
    void connectsort();
    //接受服务器信息调用槽函数
    void recvmsg();
private slots:
    //注册消息发送槽函数
    void on_register_PB_clicked();
    //登录消息发送槽函数
    void on_login_PB_clicked();
    //生成随机验证码槽函数
    void on_pushButton_clicked();

private:
    Ui::Client *ui;
    //构造函数设定为私有
    //拷贝构造函数和拷贝赋值函数删除
    Client(QWidget *parent = nullptr);
    Client(const Client& other)=delete;
    Client& operator=(const Client&)=delete;
};
#endif // CLIENT_H
