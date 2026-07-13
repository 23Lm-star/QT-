#ifndef SERVER_H
#define SERVER_H

#include <QWidget>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include "mytcpsocket.h"
#include <QList>
class Server : public QWidget
{
    Q_OBJECT

public:
    //记录ip地址和端口号
    QString m_strlp;
    quint16 m_usPort;
    //服务器所有用户文件路径
    QString filesPath;
    ~Server();
    void loadconfig();//加载配置文件
    static Server& getinstance();
private:
    Server(QWidget *parent = nullptr);
    Server(const Server& other)=delete;
    Server& operator=(const Server& other)=delete;
};
#endif // SERVER_H
