#ifndef SERVER_H
#define SERVER_H

#include <QWidget>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include "mytcpsocket.h"
#include <QList>
// 服务端外观类：仅持有运行配置并提供单例，实际的 TCP 监听/连接管理在 MyTcpServer。
// 生命周期：静态单例，由 main() 在 a.exec() 前 initialize()；析构时不额外清理全局资源。
class Server : public QWidget
{
    Q_OBJECT

public:
    //记录ip地址和端口号
    // 监听地址（connect.config 第 1 行）
    QString m_strlp;
    // 监听端口（connect.config 第 2 行）
    quint16 m_usPort;
    //服务器所有用户文件路径
    // 用户文件根目录（相对路径以运行目录为基准）
    QString filesPath;
    //数据库配置
    // 依次对应 connect.config 第 4-8 行；任一为空 initialize() 都会拒绝启动。
    QString m_dbHost;
    quint16 m_dbPort;
    QString m_dbName;
    QString m_dbUser;
    QString m_dbPassword;
    // 析构不负责停止监听：MyTcpServer 是独立单例。
    ~Server();
    bool loadconfig();//加载配置文件，成功返回 true
    // 加载配置并校验完整性，随后以 MyTcpServer 单例开始监听；任一步失败返回 false。
    bool initialize();//显式初始化：加载配置、监听端口，全部成功返回 true
    // 进程级唯一实例（函数内静态变量，C++11 起线程安全初始化）。
    static Server& getinstance();
private:
    Server(QWidget *parent = nullptr);
    Server(const Server& other)=delete;
    Server& operator=(const Server& other)=delete;
};
#endif // SERVER_H
