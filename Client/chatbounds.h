#ifndef CHATBOUNDS_H
#define CHATBOUNDS_H
// ===== REQ-CLI-MSGBND-001 私聊正文边界（两端同值，服务端为权威） =====
// 一对一消息（15/16/17/18）正文长度的取用与校验。单独成文件而不是写进 protocol.h：
// 两端 protocol.h 是线协议兼容边界，受既有冻结门禁管束（REQ-EXT-TRANSFER-003 S-03、
// REQ-EXT-SESSION-001 E7b 只管追加进契约头的常量块）；本需求的长度契约是业务校验，
// 不新增协议编号、不改 PDU/Msg 布局，因此保持冻结面零改动。
// 两端各持一份同名实现，漂移由 project-docs/evidence/REQ-CLI-MSGBND-001/verify.py 的
// C1（常量同值）与 C2（实现区块逐字相同）拦下。
#include "protocol.h"

#include <QByteArray>
#include <QString>

// 正文上限按 UTF-8 字节数计，不含协议要求的结尾 '\0'。这是业务规则，不进任何结构体布局。
static const uint CHAT_MAX_BODY_BYTES = 2000;
// Msg 记录正文的实际字节偏移：totalLen(4) + msgLen(4) + Time[30] = 38。
// sizeof(Msg) 因 4 字节对齐为 40，尾部 2 字节填充由 mkPDU 的清零保证。
static const uint CHAT_MSG_BODY_OFFSET = 38;

// 裁掉尾部不完整的 UTF-8 序列：按字节长度钳制时宁可少显示最后一个码点，
// 也不让 QString::fromUtf8 产出替换字符。
QByteArray utf8TrimIncomplete(const QByteArray &utf8);

// 宽松读一条 Msg（接收端显示与落库用）：uiMsgLen 不足一条记录头时返回 false；
// 声明的 msgLen 越出报文实际携带的字节时按实际长度钳制，并裁到最后一个完整码点；
// Time 按 30 字节定长字段的内容取，不依赖缓冲区自带终止符。
bool readChatMsg(const char *caMsg, uint uiMsgLen, QByteArray *body, QString *timeText);

// 严格读一条 Msg（服务端权威校验用）：报文不足一条记录头、正文为空、
// msgLen 越出报文实际携带的字节、或正文超过 maxBody 时返回 false；
// why 回填原因，只含长度不含正文明文。
bool checkChatMsgStrict(const char *caMsg, uint uiMsgLen, uint maxBody,
                        QByteArray *body, QString *timeText, QString *why);

// 18 响应里连续 Msg 记录的步进校验：返回本条记录占用的字节数；
// 不足一条记录头、totalLen 非法、msgLen 与 totalLen 矛盾或记录越出报文时返回 0，
// 表示解析到此为止（与网页端 parseMsgRecords 的“畸形即止”同口径）。
uint chatMsgRecordStride(const char *caMsg, uint uiMsgLen, uint offset);

#endif // CHATBOUNDS_H
