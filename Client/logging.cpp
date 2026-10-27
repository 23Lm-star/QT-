// 客户端日志分类定义；分类名固定为 "xmu.cli"（与日志过滤规则匹配）。
#include "logging.h"

// 定义 lcCli 分类对象（名称 "xmu.cli"），供 qCDebug/qCWarning 等使用。
Q_LOGGING_CATEGORY(lcCli, "xmu.cli")
