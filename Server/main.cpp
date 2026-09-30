#include "logging.h"
#include "server.h"

#include <QApplication>
#include "operatedb.h"
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // 默认安静：debug 级不输出。排查时设 QT_LOGGING_RULES="xmu.*.debug=true" 打开全部，
    // 或单类 "xmu.db.debug=true"。Qt 5.14 实测环境变量优先于程序化规则，无需守卫。
    QLoggingCategory::setFilterRules(QStringLiteral("xmu.*.debug=false"));

    // 显式初始化服务端（加载配置+监听端口），失败时退出
    Server& s = Server::getinstance();
    if (!s.initialize()) {
        qCCritical(lcApp) << "Server initialization failed. Exiting.";
        return 1;
    }
    // 仅成功后初始化数据库连接池
    OperateDB::instance().connect();
    // REQ-EXT-FILE-001 T1/D1：schema 版本检查 fail-fast——库不可达或未执行迁移则拒绝启动
    if (!OperateDB::instance().checkSchemaVersion()) {
        qCCritical(lcApp) << "DB schema not ready (see lcDb critical log). Exiting.";
        return 1;
    }
    // REQ-EXT-FILE-001 T6：存量迁移在版本检查通过后、事件循环启动前自动执行一次。
    // 此时尚未处理任何业务请求（a.exec 未开始），且幂等：重跑仅跳过已映射文件。
    OperateDB::instance().migrateLegacyFiles();
    s.show();
    return a.exec();
}
