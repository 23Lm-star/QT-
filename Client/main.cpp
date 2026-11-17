// 客户端进程入口：初始化 QApplication、日志过滤与全局 QSS，
// 然后取出 Client 单例显示登录窗口并进入事件循环。
#include "logging.h"
#include "client.h"
#include "index.h"

#include <QApplication>
#include <QFile>
#include <QString>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // 默认安静（与 Server 同策略）：排查时设 QT_LOGGING_RULES="xmu.cli.debug=true"
    // Qt 5.14 实测环境变量优先于程序化规则，无需守卫。
    // REQ-AUDIT-FULL-001：PDU 报文日志默认开启（终端+文件，供纠错排查；环境变量可覆盖）。
    QLoggingCategory::setFilterRules(QStringLiteral("xmu.*.debug=false\nxmu.cli.pdu.debug=true"));
    installFileLogging(QStringLiteral("client"));

    // 加载全局 QSS 炫酷主题
    // 资源缺失不致命，只记录警告，界面用默认样式继续运行。
    QFile qssFile(":/style.qss");
    if (qssFile.open(QFile::ReadOnly)) {
        QString qssContent = QString::fromUtf8(qssFile.readAll());
        a.setStyleSheet(qssContent);
        qssFile.close();
    } else {
        qCWarning(lcCli) << "QSS 加载失败: 未找到资源 :/style.qss";
    }

    //通过架构函数来获取唯一示例
    Client::getinstance().show();
    return a.exec();
}
