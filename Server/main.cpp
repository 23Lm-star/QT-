#include "server.h"

#include <QApplication>
#include "operatedb.h"
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    //利用 静态 操作数据库的对象 连接数据库
    OperateDB::instance().connect();
    Server::getinstance();
//    w.show();
    return a.exec();
}
