#include "logging.h"
#include "server.h"

#include <QApplication>
#include "operatedb.h"
#include "reclaimscheduler.h"
// 服务端启动顺序（失败即退出，避免半初始化状态）：
// 配置/监听 → DB 连接池 → schema 版本 → 在线态对账 → 逻辑键归一化 → 存量迁移
// → 回收调度器 → 进入事件循环；退出时先停调度器再销毁 QApplication。
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // 默认安静：debug 级不输出。排查时设 QT_LOGGING_RULES="xmu.*.debug=true" 打开全部，
    // 或单类 "xmu.db.debug=true"。Qt 5.14 实测环境变量优先于程序化规则，无需守卫。
    // REQ-AUDIT-FULL-001：PDU 报文日志默认开启（终端+文件，供纠错排查；环境变量可覆盖）。
    QLoggingCategory::setFilterRules(QStringLiteral("xmu.*.debug=false\nxmu.net.pdu.debug=true"));
    installFileLogging(QStringLiteral("server"));

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
    // REQ-EXT-SESSION-002：启动期在线态对账——上个进程被 kill/崩溃/断电时，当时在线的用户
    // 没有任何机会走 handleOffLine，库里会留下 online=1 的幽灵（在线列表与用户信息照旧把
    // 它们当在线，"加好友"还会朝幽灵转发）。必须在这里、a.exec() 之前做：此刻本进程一条
    // 连接都没有，任何 online=1 都必然是上个进程的残留。失败即退出——库不可写时登录/注册
    // 同样会失败，带着幽灵启动只是把问题藏起来。
    if (!OperateDB::instance().reconcileOnlineFlags()) {
        qCCritical(lcApp) << "Online-state reconciliation failed (see lcDb critical log). Exiting.";
        return 1;
    }
    // REQ-MIG-001：存量键归一化与存量文件迁移均已完成一次性收敛，启动不再跑迁移管线。
    // 文件域只认逻辑条目 + objects/ 对象存储，不做旧物理路径兼容。
    // REQ-EXT-RECLAIM-001：周期清理调度器（延迟删除的收割者）。
    // 放在这里而不是更早：它要用 filesPath（initialize 载入）、DB 连接池（connect）
    // 与迁移后的 schema（checkSchemaVersion 已 fail-fast）。
    // 它挂在主线程事件循环上、每拍只动有界条数，不阻塞收发；第一次心跳就会先处置
    // 上一个进程崩溃留下的 reclaiming 行。enabled=0 时它自己拒绝启动。
    ReclaimScheduler::instance().start();
    s.show();
    const int rc = a.exec();
    // 在 QApplication 析构之前停表：调度器是静态单例，它的析构发生在 QApplication
    // 之后，那时 Qt 已拆掉主线程的 QThread 标识，停一个还活着的定时器会报
    // "Timers cannot be stopped from another thread"。这里显式收尾，退出保持干净。
    ReclaimScheduler::instance().stop();
    return rc;
}
