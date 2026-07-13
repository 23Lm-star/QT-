#include "clienttask.h"
#include "mytcpserver.h"
#include "mytcpsocket.h"

ClientTask::ClientTask(MyTcpSocket *m_socket)
{
    socket=m_socket;
}

void ClientTask::run()
{

}

ClientTask::ClientTask()
{

}

