// 客户端日志分类定义；分类名固定为 "xmu.cli"（与日志过滤规则匹配）。
#include "logging.h"

// 定义 lcCli 分类对象（名称 "xmu.cli"），供 qCDebug/qCWarning 等使用。
Q_LOGGING_CATEGORY(lcCli, "xmu.cli")
// REQ-AUDIT-FULL-001：PDU 报文日志分类（"xmu.cli.pdu"），main.cpp 默认开启
Q_LOGGING_CATEGORY(lcPdu, "xmu.cli.pdu")

// ===== REQ-AUDIT-FULL-001：PDU 报文日志（终端 + 文件双落盘） =====
// Windows 下 Server 为 console 子系统（stderr 即终端）；Client 为 GUI 子系统，
// stderr 由 Qt Creator 输出面板捕获，落盘文件为主要排障通道。10MB 轮转保留一份 .1 备份。
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <cstdio>

namespace {
QFile g_pduLogFile;
constexpr qint64 kMaxLogBytes = 10 * 1024 * 1024;

const char *msgTypeChar(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return "D";
    case QtInfoMsg:     return "I";
    case QtWarningMsg:  return "W";
    case QtCriticalMsg: return "C";
    case QtFatalMsg:    return "F";
    }
    return "?";
}

void fileMessageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    const QByteArray head = QByteArray("[")
            .append(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz")).toUtf8())
            .append("][")
            .append(ctx.category)
            .append("][")
            .append(msgTypeChar(type))
            .append("] ");
    // 终端打印：Windows 控制台/Qt Creator 输出面板按本地编码（GBK）解码，走 toLocal8Bit 才不乱码
    QByteArray termLine = head + msg.toLocal8Bit() + '\n';
    fwrite(termLine.constData(), 1, size_t(termLine.size()), stderr);
    fflush(stderr);
    // 文件落盘：保持 UTF-8，便于工具检索中文
    if (!g_pduLogFile.isOpen()) return;
    if (g_pduLogFile.size() > kMaxLogBytes) {
        const QString path = g_pduLogFile.fileName();
        g_pduLogFile.close();
        QFile::remove(path + QStringLiteral(".1"));
        QFile::rename(path, path + QStringLiteral(".1"));
        g_pduLogFile.setFileName(path);
        g_pduLogFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }
    g_pduLogFile.write(head + msg.toUtf8() + '\n');
    g_pduLogFile.flush();
}
} // namespace

void installFileLogging(const QString &appTag)
{
    const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/logs");
    QDir().mkpath(dir);
    g_pduLogFile.setFileName(dir + QLatin1Char('/') + appTag + QStringLiteral("-pdu.log"));
    if (!g_pduLogFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        fprintf(stderr, "[logging] PDU 日志文件打开失败：%s\n",
                g_pduLogFile.errorString().toUtf8().constData());
        return;
    }
    qInstallMessageHandler(fileMessageHandler);
}
