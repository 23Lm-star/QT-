#include "logging.h"
#include "server.h"

#include <QApplication>
#include "operatedb.h"
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // 默认安静：debug 级不输出。排查时设 QT_LOGGING_RULES="xmu.*.debug=true" 打开全部，
    // 或单类 "xmu.db.debug=true"。守卫（D2）：已设环境变量时不覆盖。
    if (qEnvironmentVariableIsEmpty("QT_LOGGING_RULES"))
        QLoggingCategory::setFilterRules(QStringLiteral("xmu.*.debug=false"));

    // 显式初始化服务端（加载配置+监听端口），失败时退出
    Server& s = Server::getinstance();
    if (!s.initialize()) {
        qCCritical(lcApp) << "Server initialization failed. Exiting.";
        return 1;
    }
    // 仅成功后初始化数据库连接池
    OperateDB::instance().connect();
    s.show();
    return a.exec();
}
