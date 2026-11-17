#ifndef CLI_LOGGING_H
#define CLI_LOGGING_H
// 客户端日志分类声明：全客户端统一使用 lcCli（分类名 "xmu.cli"），
// 默认 debug 关闭，排查时用 QT_LOGGING_RULES="xmu.cli.debug=true" 打开。

#include <QLoggingCategory>

// 决策 D3：客户端单分类，与 Server 对称建文件并登记 Client.pro
// 声明日志分类对象；定义在 logging.cpp。
Q_DECLARE_LOGGING_CATEGORY(lcCli)   // 客户端全部日志
Q_DECLARE_LOGGING_CATEGORY(lcPdu)   // REQ-AUDIT-FULL-001：PDU 报文日志（收发内容，默认开启）

// 安装"终端+文件"双路日志输出：stderr + <exe目录>/logs/client-pdu.log（10MB 轮转）
void installFileLogging(const QString &appTag);

#endif // CLI_LOGGING_H
