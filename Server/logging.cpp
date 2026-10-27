#include "logging.h"

// 日志分类定义：显示名为 xmu.net 等，规则串与需求文档一致（见 logging.h 的 D1 决策）。
// 分类对象本身为全局常量，进程生命周期内有效，不参与内存管理。
Q_LOGGING_CATEGORY(lcNet,  "xmu.net")
Q_LOGGING_CATEGORY(lcDb,   "xmu.db")
Q_LOGGING_CATEGORY(lcFs,   "xmu.fs")
Q_LOGGING_CATEGORY(lcAuth, "xmu.auth")
Q_LOGGING_CATEGORY(lcApp,  "xmu.app")
