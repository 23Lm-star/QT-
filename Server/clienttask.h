#ifndef CLIENTTASK_H
#define CLIENTTASK_H

#include <QObject>
#include <QRunnable>
#include "mytcpsocket.h"
class ClientTask : public QObject,public QRunnable{
    Q_OBJECT
public:
    //维护了一个socket连接对象
    MyTcpSocket* socket;
    ClientTask(MyTcpSocket* m_socket);
    void run() override;
    explicit ClientTask();

signals:

};

#endif // CLIENTTASK_H
