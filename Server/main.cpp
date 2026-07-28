#include "server.h"

#include <QApplication>
#include "operatedb.h"
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    // 显式初始化服务端（加载配置+监听端口），失败时退出
    Server& s = Server::getinstance();
    if (!s.initialize()) {
        qCritical() << "Server initialization failed. Exiting.";
        return 1;
    }
    // 仅成功后初始化数据库连接池
    OperateDB::instance().connect();
    s.show();
    return a.exec();
}
