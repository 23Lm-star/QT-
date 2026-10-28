// ===== REQ-CLI-MSGBND-001 私聊正文边界实现 =====
// 两端各持一份同名实现（与 chatbounds.h 的约定一致）：正文长度只能由报文实际
// 携带的字节决定，声明值（msgLen/totalLen）是网络边界上的外部输入，不可信。
// 两端一致性由 project-docs/evidence/REQ-CLI-MSGBND-001/verify.py 的 C1/C2 拦下。
#include "chatbounds.h"

// 定长字段按内容取值：遇首个 '\0' 截断，不依赖调用方缓冲区的自带终止符。
static QString utf8FieldToString(const char *data, int maxLen)
{
    QByteArray bytes(data, maxLen);
    const int end = bytes.indexOf('\0');
    if (end >= 0) {
        bytes.truncate(end);
    }
    return QString::fromUtf8(bytes);
}

QByteArray utf8TrimIncomplete(const QByteArray &utf8)
{
    const int n = utf8.size();
    if (n == 0) {
        return utf8;
    }
    // 回看尾部续字节（10xxxxxx），最多 3 个：UTF-8 最长 4 字节序列 = 1 起始 + 3 续
    int i = n - 1;
    int back = 0;
    while (i >= 0 && back < 3 && (((quint8)utf8.at(i)) & 0xC0) == 0x80) {
        --i;
        ++back;
    }
    if (i < 0) {
        return QByteArray();          // 整段都是续字节：没有完整码点可保留
    }
    const quint8 lead = (quint8)utf8.at(i);
    int need = 0;                     // 该起始字节声明的序列总长
    if      ((lead & 0x80) == 0x00) need = 1;
    else if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    // need == 0 → 起始字节非法（含尾部仍是续字节的情形），连同其后内容一并丢弃
    if (need == 0 || n - i < need) {
        return utf8.left(i);
    }
    return utf8;
}

bool readChatMsg(const char *caMsg, uint uiMsgLen, QByteArray *body, QString *timeText)
{
    if (caMsg == nullptr || body == nullptr || uiMsgLen < (uint)sizeof(Msg)) {
        return false;
    }
    const Msg *m = reinterpret_cast<const Msg *>(caMsg);
    const uint avail = uiMsgLen - CHAT_MSG_BODY_OFFSET;   // 报文从正文偏移起实际携带的字节数
    uint want = 0;                                        // 正文字节数（不含结尾 '\0'）
    if (m->msgLen >= 1) {
        want = m->msgLen - 1;
    }
    if (want > avail) {                                   // 声明越界：按实际携带长度钳制
        want = avail;
    }
    *body = utf8TrimIncomplete(QByteArray(caMsg + CHAT_MSG_BODY_OFFSET, int(want)));
    if (timeText != nullptr) {
        *timeText = utf8FieldToString(m->Time, 30);
    }
    return true;
}

bool checkChatMsgStrict(const char *caMsg, uint uiMsgLen, uint maxBody,
                        QByteArray *body, QString *timeText, QString *why)
{
    if (why != nullptr) {
        why->clear();
    }
    if (caMsg == nullptr || uiMsgLen < (uint)sizeof(Msg)) {
        if (why != nullptr) {
            *why = QString("报文正文 %1 字节，不足一条 Msg 头(%2)")
                       .arg(uiMsgLen).arg((uint)sizeof(Msg));
        }
        return false;
    }
    const Msg *m = reinterpret_cast<const Msg *>(caMsg);
    // msgLen 含结尾 '\0'，小于 2 即没有正文
    if (m->msgLen < 2) {
        if (why != nullptr) {
            *why = QString("正文为空(msgLen=%1)").arg(m->msgLen);
        }
        return false;
    }
    const uint avail = uiMsgLen - CHAT_MSG_BODY_OFFSET;
    if (m->msgLen > avail) {
        if (why != nullptr) {
            *why = QString("msgLen 声明 %1 越出报文实际携带 %2").arg(m->msgLen).arg(avail);
        }
        return false;
    }
    const QByteArray raw(caMsg + CHAT_MSG_BODY_OFFSET, int(m->msgLen - 1));
    if ((uint)raw.size() > maxBody) {
        if (why != nullptr) {
            *why = QString("正文 %1 字节，超过上限 %2").arg(raw.size()).arg(maxBody);
        }
        return false;
    }
    if (body != nullptr) {
        *body = raw;
    }
    if (timeText != nullptr) {
        *timeText = utf8FieldToString(m->Time, 30);
    }
    return true;
}

uint chatMsgRecordStride(const char *caMsg, uint uiMsgLen, uint offset)
{
    if (caMsg == nullptr || offset + (uint)sizeof(Msg) > uiMsgLen) {
        return 0;
    }
    const Msg *m = reinterpret_cast<const Msg *>(caMsg + offset);
    if (m->totalLen < (uint)sizeof(Msg)) {
        return 0;                                        // 含 totalLen==0，避免步进死循环
    }
    if (m->msgLen < 1 || m->msgLen > m->totalLen) {
        return 0;
    }
    if (offset + m->totalLen > uiMsgLen) {
        return 0;                                        // 记录越出报文
    }
    return m->totalLen;
}
