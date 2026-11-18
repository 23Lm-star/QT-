#include "protocol.h"
#include <stdlib.h>
#include <string.h>
#include "memalloc.h"

// PDU 内存管理规则：
// 1. 所有 PDU 必须由分配该 PDU 的同一进程内配套函数释放
//    - Client: mkPDU() 用 malloc → 必须用 free() 释放
//    - Server: mkPDU() 用 memalloc → 必须用 memfree() 释放
// 2. 禁止跨进程/跨策略释放 PDU
// 3. 接收缓冲区中的原始字节不由 seedMsg() 释放
// 构造一个服务端 PDU：整块内存一次分配（头 + 正文），清零后写入长度字段。
// 调用方负责最终用 memfree() 释放；分配失败直接 exit(1)，属于进程级不可恢复错误。
PDU *mkPDU(uint uiMsgLen)
{
    uint uitotallen = sizeof(PDU) + uiMsgLen;
    // 必须使用服务端内存池（memalloc），与后续 sendAndRelease/seedMsg 的 memfree 配对。
    PDU* pdu = (PDU*)memalloc(uitotallen);
    if(pdu == nullptr){
        exit(1);
    }
    memset(pdu, 0, uitotallen);
    pdu->uiTotalLen = uitotallen;
    pdu->uiMsgLen = uiMsgLen;
    return pdu;
}


// ===== REQ-AUDIT-FULL-001：PDU 报文日志（终端 + 文件双落盘，逐类型字段解码，供纠错排查） =====
// 规则：密码等敏感字段一律打码只留长度（AGENTS：密码不进日志）；验证码答案不回显；
// 心跳（50/51）跳过；上传/下载数据块（31/42）不落正文只记关键偏移。
#include "logging.h"

namespace {
// 消息类型名表（索引=枚举值，仅日志可读性用；与 protocol.h 枚举保持一致）
const char *const kPduTypeNames[] = {
"MIN",
"REGIST_REQUEST",
"REGIST_RESPOND",
"LOGIN_REQUEST",
"LOGIN_RESPOND",
"findUser_REQUEST",
"findUser_RESPOND",
"ONLINEUSER_REQUEST",
"ONLINEUSER_RESPOND",
"ADDFRIEND_REQUEST",
"ADDFRIEND_RESPOND",
"FLUSHFRIEND_REQUEST",
"FLUSHFRIEND_RESPOND",
"DELETEFRIEND_REQUEST",
"DELETEFRIEND_RESPOND",
"CHAT_REQUEST",
"CHAT_RESPOND",
"GETCHATMSG_REQUEST",
"GETCHATMSG_RESPOND",
"MKDIR_REQUEST",
"MKDIR_RESPOND",
"CHANGEDIR_REQUEST",
"CHANGEDIR_RESPOND",
"FLUSHFILE_REQUEST",
"FLUSHFILE_RESPOND",
"DELFILE_REQUEST",
"DELFILE_RESPOND",
"RENAMEFILE_REQUEST",
"RENAMEFILE_RESPOND",
"UPLOADFILE_INIT_REQUEST",
"UPLOADFILE_INIT_RESPOND",
"UPLOADFILE_DATA_REQUEST",
"UPLOADFILE_DATA_RESPOND",
"SHAREFILE_REQUEST",
"SHAREFILE_RESPOND",
"SHAREFILE_AGREE_REQUEST",
"SHAREFILE_AGREE_RESPOND",
"SHAREFILE_FINISH_RESPOND",
"MOVEFILE_REQUEST",
"MOVEFILE_RESPOND",
"DOWNLOADFILE_REQUEST",
"DOWNLOADFILE_RESPOND",
"DOWNLOADFILE_DATA",
"DOWNLOADFILE_FINISH",
"CAPTCHA_REQUEST",
"CAPTCHA_RESPOND",
"GETUSERINFORMATION_REQUEST",
"GETUSERINFORMATION_RESPOND",
"LOGOFF_REQUEST",
"LOGOFF_RESPOND",
"HEARTBEAT_REQUEST",
"HEARTBEAT_RESPOND",
"CREATE_PUBLINK_REQUEST",
"CREATE_PUBLINK_RESPOND",
"RESOLVE_PUBLINK_REQUEST",
"RESOLVE_PUBLINK_RESPOND",
"QUERY_FILE_PUBLINK_REQUEST",
"QUERY_FILE_PUBLINK_RESPOND",
"LISTDIR_REQUEST",
"LISTDIR_RESPOND",
"CREATE_GROUP_REQUEST",
"CREATE_GROUP_RESPOND",
"INVITE_GROUP_MEMBER_REQUEST",
"INVITE_GROUP_MEMBER_RESPOND",
"ACCEPT_GROUP_INVITATION_REQUEST",
"ACCEPT_GROUP_INVITATION_RESPOND",
"REJECT_GROUP_INVITATION_REQUEST",
"REJECT_GROUP_INVITATION_RESPOND",
"LEAVE_GROUP_REQUEST",
"LEAVE_GROUP_RESPOND",
"REMOVE_GROUP_MEMBER_REQUEST",
"REMOVE_GROUP_MEMBER_RESPOND",
"SET_GROUP_ROLE_REQUEST",
"SET_GROUP_ROLE_RESPOND",
"TRANSFER_GROUP_OWNER_REQUEST",
"TRANSFER_GROUP_OWNER_RESPOND",
"DISSOLVE_GROUP_REQUEST",
"DISSOLVE_GROUP_RESPOND",
"LIST_GROUPS_REQUEST",
"LIST_GROUPS_RESPOND",
"LIST_GROUP_MEMBERS_REQUEST",
"LIST_GROUP_MEMBERS_RESPOND",
"QUERY_GROUP_INVITATIONS_REQUEST",
"QUERY_GROUP_INVITATIONS_RESPOND",
"GROUP_MEMBER_EVENT",
"GROUP_INVITATION_EVENT",
"QUERY_GROUP_SUMMARY_REQUEST",
"QUERY_GROUP_SUMMARY_RESPOND",
"GROUP_SEND_MESSAGE_REQUEST",
"GROUP_SEND_MESSAGE_RESPOND",
"GROUP_MESSAGE_EVENT",
"GROUP_PULL_MESSAGES_REQUEST",
"GROUP_PULL_MESSAGES_RESPOND",
"GROUP_READ_CONFIRM_REQUEST",
"GROUP_READ_CONFIRM_RESPOND",
"GROUP_UNREAD_SUMMARY_REQUEST",
"GROUP_UNREAD_SUMMARY_RESPOND",
"UPLOADFILE_FASTCHECK_REQUEST",
"UPLOADFILE_FASTCHECK_RESPOND",
"GROUP_FILE_LIST_REQUEST",
"GROUP_FILE_LIST_RESPOND",
"GROUP_FILE_UPLOAD_INIT_REQUEST",
"GROUP_FILE_UPLOAD_INIT_RESPOND",
"GROUP_FILE_DELETE_REQUEST",
"GROUP_FILE_DELETE_RESPOND",
"GROUP_FILE_DOWNLOAD_REQUEST",
"GROUP_FILE_DOWNLOAD_RESPOND",
"UPLOADFILE_ABORT_REQUEST",
"UPLOADFILE_ABORT_RESPOND",
"CHAT_DELIVER_EVENT",
"CHAT_DELIVER_ACK",
"CHAT_PULL_PENDING_REQUEST",
"CHAT_PULL_PENDING_RESPOND",
"LIST_PENDING_FRIEND_REQUEST",
"LIST_PENDING_FRIEND_RESPOND",
"REJECT_FRIEND_REQUEST",
"CHAT_ACK_CAPABILITY",
"CHAT_HISTORY_REQUEST",
"CHAT_HISTORY_RESPOND"
};

// ---- 安全读取助手（全部带边界钳制，杜绝日志器自身越界） ----
QString fixedStr(const char *base, int off, int maxLen)
{
    if (base == nullptr || off < 0 || off >= maxLen) return QStringLiteral("<越界>");
    const int n = qMin(maxLen - off, 32);
    char buf[33];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, base + off, size_t(n));
    buf[32] = '\0';
    return QString::fromUtf8(buf);
}
QString maskedStr(const char *base, int off, int maxLen)
{
    if (base == nullptr || off < 0 || off >= maxLen) return QStringLiteral("<越界>");
    int n = 0;
    while (n < maxLen - off && base[off + n] != '\0') ++n;
    return QStringLiteral("***(%1B)").arg(n);
}
quint32 u32at(const char *base, int off, int maxLen)
{
    if (base == nullptr || off < 0 || off + 4 > maxLen) return 0;
    quint32 v = 0;
    memcpy(&v, base + off, sizeof(v));
    return v;
}
qint32 i32at(const char *base, int off, int maxLen)
{
    if (base == nullptr || off < 0 || off + 4 > maxLen) return 0;
    qint32 v = 0;
    memcpy(&v, base + off, sizeof(v));
    return v;
}
qint64 q64at(const char *base, int off, int maxLen)
{
    if (base == nullptr || off < 0 || off + 8 > maxLen) return 0;
    qint64 v = 0;
    memcpy(&v, base + off, sizeof(v));
    return v;
}
QByteArray hexHead(const char *data, int size, int head)
{
    if (data == nullptr || size <= 0) return QByteArray("(空)");
    const int n = qMin(size, head);
    QByteArray out;
    out.reserve(n * 3 + 8);
    static const char *digits = "0123456789ABCDEF";
    for (int i = 0; i < n; ++i) {
        const quint8 b = quint8(data[i]);
        out.append(digits[b >> 4]);
        out.append(digits[b & 0xF]);
        out.append(' ');
    }
    if (size > head) out.append("...");
    return out;
}

// ---- 字段行收集器 ----
struct FieldSink {
    QStringList lines;
    void str(const char *label, const char *base, int off, int maxLen)          // 定长字符串槽
    { lines << QStringLiteral("%1=\"%2\"").arg(QString::fromUtf8(label), fixedStr(base, off, maxLen)); }
    void mask(const char *label, const char *base, int off, int maxLen)         // 敏感字段打码
    { lines << QStringLiteral("%1=%2").arg(QString::fromUtf8(label), maskedStr(base, off, maxLen)); }
    void u32(const char *label, const char *base, int off, int maxLen)
    { lines << QStringLiteral("%1=%2").arg(QString::fromUtf8(label)).arg(u32at(base, off, maxLen)); }
    void i32(const char *label, const char *base, int off, int maxLen)
    { lines << QStringLiteral("%1=%2").arg(QString::fromUtf8(label)).arg(i32at(base, off, maxLen)); }
    void q64(const char *label, const char *base, int off, int maxLen)
    { lines << QStringLiteral("%1=%2").arg(QString::fromUtf8(label)).arg(q64at(base, off, maxLen)); }
    void raw(const QString &text) { lines << text; }
};

// Msg 记录预览（15 请求与 18/109 的载荷）：totalLen/msgLen/Time[30]/msg[]
QString msgRecordPreview(const char *base, int maxLen)
{
    if (base == nullptr || maxLen < 38) return QStringLiteral("载荷不足 Msg 头");
    const quint32 total = u32at(base, 0, maxLen);
    const quint32 mlen = u32at(base, 4, maxLen);
    const char *time = base + 8;
    const char *body = base + 38;
    int blen = int(mlen) - 1;
    if (mlen == 0 || blen < 0 || 38 + int(mlen) > maxLen) blen = 0;
    char buf[41];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, body, size_t(qMin(blen, 40)));
    return QStringLiteral("total=%1 msgLen=%2 时间=\"%3\" 正文=\"%4\"")
            .arg(total).arg(mlen).arg(QString::fromUtf8(time)).arg(QString::fromUtf8(buf));
}

// 逐类型字段解码：返回多行文本（每行一个字段或说明）
QString decodePdu(const PDU *pdu)
{
    FieldSink f;
    const char *d = pdu->caData;            // 64B 定长参数区
    const char *m = pdu->caMsg;             // 变长载荷区
    const int dl = 64;
    const int ml = int(pdu->uiMsgLen);
    const uint t = pdu->uiType;
    switch (t) {
    case ENUM_MSG_TYPE_REGIST_REQUEST:
        f.str("用户名", d, 0, dl); f.mask("密码", d, 32, dl);
        f.raw(QStringLiteral("验证码=\"%1\"").arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_REGIST_RESPOND:
        f.i32("结果码", d, 0, dl); f.str("回显用户名", d, 32, dl); break;
    case ENUM_MSG_TYPE_LOGIN_REQUEST:
        f.str("用户名", d, 0, dl); f.mask("密码", d, 32, dl);
        f.raw(QStringLiteral("验证码=\"%1\"").arg(fixedStr(m, 0, ml)));
        if (ml > 0) { int nul = 0; while (nul < ml && m[nul] != '\0') ++nul;
            if (nul + 1 < ml) f.raw(QStringLiteral("客户端ID=\"%1\"").arg(fixedStr(m, nul + 1, ml))); }
        break;
    case ENUM_MSG_TYPE_LOGIN_RESPOND:
        f.i32("结果码", d, 0, dl);
        if (i32at(d, 0, dl) == -4004) f.i32("锁定剩余秒", d, 4, dl); break;
    case ENUM_MSG_TYPE_findUser_REQUEST:
        f.str("目标用户", d, 0, dl); break;
    case ENUM_MSG_TYPE_findUser_RESPOND:
        f.i32("结果码", d, 0, dl); f.u32("在线(1=在线)", d, 4, dl); break;
    case ENUM_MSG_TYPE_ONLINEUSER_REQUEST:
        f.u32("每页条数(0=全量)", d, 0, dl); f.u32("分页起点", d, 4, dl);
        f.str("搜索关键词(空=不过滤)", d, 8, dl); break;
    case ENUM_MSG_TYPE_ONLINEUSER_RESPOND: {
        f.i32("结果码", d, 0, dl); f.u32("符合条件总数(封顶后)", d, 4, dl);
        f.raw(QStringLiteral("还有下一页=%1 本页条数=%2 总量封顶=%3").arg(int(d[8])).arg(u32at(d, 12, dl)).arg(int(d[16])));
        const int cnt = qMin(u32at(d, 12, dl), quint32(ml / 33));
        for (int i = 0; i < qMin(cnt, 3); ++i)
            f.raw(QStringLiteral("  条目[%1] 名=\"%2\" 在线=%3").arg(i)
                  .arg(fixedStr(m, i * 33, i * 33 + 32 <= ml ? ml : ml)).arg(m[i * 33 + 32] == 1));
        if (cnt > 3) f.raw(QStringLiteral("  …共 %1 条").arg(cnt));
        break; }
    case ENUM_MSG_TYPE_ADDFRIEND_REQUEST:
        f.str("申请人", d, 0, dl); f.str("目标", d, 32, dl); break;
    case ENUM_MSG_TYPE_ADDFRIEND_RESPOND:
        f.i32("结果码", d, 0, dl); f.str("账号A", d, 0, dl); f.str("账号B", d, 32, dl); break;
    case ENUM_MSG_TYPE_FLUSHFRIEND_REQUEST:
        // REQ-PAGE-FETCH-002：好友真分页请求（旧端空 caData 即 limit=0 全量语义）
        f.u32("每页条数(0=全量)", d, 0, dl); f.u32("分页起点", d, 4, dl); break;
    case ENUM_MSG_TYPE_LIST_PENDING_FRIEND_REQUEST:
    case ENUM_MSG_TYPE_CAPTCHA_REQUEST:
    case ENUM_MSG_TYPE_CHAT_ACK_CAPABILITY:
        f.raw(QStringLiteral("空请求")); break;
    case ENUM_MSG_TYPE_FLUSHFRIEND_RESPOND: {
        f.i32("结果码", d, 0, dl);
        f.raw(QStringLiteral("本页条数=%1 好友总数(封顶后)=%2 还有下一页=%3 总量封顶=%4")
              .arg(u32at(d, 8, dl)).arg(u32at(d, 12, dl)).arg(int(d[16])).arg(int(d[17])));
        const int cnt = qMin(u32at(d, 8, dl), quint32(ml / 32));
        for (int i = 0; i < qMin(cnt, 3); ++i) f.raw(QStringLiteral("  好友[%1] \"%2\"").arg(i).arg(fixedStr(m, i * 32, ml)));
        if (cnt > 3) f.raw(QStringLiteral("  …共 %1 条").arg(cnt));
        break; }
    case ENUM_MSG_TYPE_DELETEFRIEND_REQUEST:
        f.str("被删好友", d, 0, dl); f.str("发起人", d, 32, dl); break;
    case ENUM_MSG_TYPE_DELETEFRIEND_RESPOND:
        f.i32("结果码", d, 0, dl); break;
    case ENUM_MSG_TYPE_CHAT_REQUEST:
        f.str("发送人", d, 0, dl); f.str("接收人", d, 32, dl);
        f.raw(QStringLiteral("消息: %1").arg(msgRecordPreview(m, ml))); break;
    case ENUM_MSG_TYPE_CHAT_RESPOND:
        // 16 布局=caData[0,32)对方名+[32)入库结果码+caMsg 回显 Msg（REQ-CLI-MSGSTORE-001）
        f.str("对方", d, 0, dl); f.i32("入库结果码", d, 32, dl);
        f.raw(QStringLiteral("回显: %1").arg(msgRecordPreview(m, ml))); break;
    case ENUM_MSG_TYPE_GETCHATMSG_REQUEST:
        f.str("查询者", d, 0, dl); f.str("对方", d, 32, dl);
        f.raw(QStringLiteral("记录数=%1").arg(ml / 38)); break;   // 旧消费式补发（网页端）
    case ENUM_MSG_TYPE_MKDIR_REQUEST:
        f.str("新目录名", d, 0, dl); f.raw(QStringLiteral("父目录=\"%1\"").arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_MKDIR_RESPOND:
        f.i32("结果码", d, 0, dl); break;
    case ENUM_MSG_TYPE_CHANGEDIR_REQUEST:
        f.raw(QStringLiteral("目标目录=\"%1\"").arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_CHANGEDIR_RESPOND:
        f.i32("结果码", d, 0, dl);
        if (ml > 0) f.raw(QStringLiteral("可进入目录=\"%1\"").arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_FLUSHFILE_REQUEST:
        f.u32("每页条数(0=全量)", d, 0, dl); f.u32("分页起点", d, 4, dl);
        f.raw(QStringLiteral("目录=\"%1\"").arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_FLUSHFILE_RESPOND: {
        f.i32("结果码", d, 0, dl); f.u32("条目总数", d, 4, dl);
        f.raw(QStringLiteral("还有下一页=%1 本页条数=%2").arg(int(d[8])).arg(u32at(d, 12, dl)));
        const int cnt = qMin(u32at(d, 12, dl), quint32(ml / int(sizeof(FileInfo))));
        for (int i = 0; i < qMin(cnt, 3); ++i)
            f.raw(QStringLiteral("  条目[%1] 名=\"%2\" %3").arg(i)
                  .arg(fixedStr(m, int(i * sizeof(FileInfo)), ml))
                  .arg(u32at(m, int(i * sizeof(FileInfo)) + 32, ml) == 0 ? "目录" : "文件"));
        if (cnt > 3) f.raw(QStringLiteral("  …共 %1 条").arg(cnt));
        break; }
    case ENUM_MSG_TYPE_DELFILE_REQUEST:
        f.raw(QStringLiteral("删除路径=\"%1\"").arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_DELFILE_RESPOND:
        f.i32("结果码", d, 0, dl); break;
    case ENUM_MSG_TYPE_RENAMEFILE_REQUEST:
        f.str("旧名", d, 0, dl); f.str("新名", d, 32, dl);
        f.raw(QStringLiteral("所在目录=\"%1\"").arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_RENAMEFILE_RESPOND:
        f.i32("结果码", d, 0, dl); f.str("回显最终名", d, 32, dl); break;
    case ENUM_MSG_TYPE_UPLOADFILE_INIT_REQUEST: {
        f.str("文件名", d, 0, dl); f.q64("文件大小", d, 32, dl);
        f.raw(QStringLiteral("续传魔数=0x%1 任务ID=%2")
              .arg(u32at(d, 40, dl), 8, 16, QChar('0')).arg(q64at(d, 48, dl)));
        f.raw(QStringLiteral("所在目录=\"%1\"").arg(fixedStr(m, 0, ml))); break; }
    case ENUM_MSG_TYPE_UPLOADFILE_INIT_RESPOND: {
        f.i32("结果码", d, 0, dl);
        const quint32 magic = u32at(d, 40, dl);
        f.raw(QStringLiteral("续传魔数=0x%1").arg(magic, 8, 16, QChar('0')));
        if (magic == 0x5253554d) f.raw(QStringLiteral("续传起点=%1 任务ID=%2").arg(q64at(d, 32, dl)).arg(q64at(d, 48, dl)));
        break; }
    case ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST:
        f.raw(QStringLiteral("[数据块] 块偏移=%1 任务ID=%2 数据=%3B")
              .arg(q64at(d, 0, dl)).arg(q64at(d, 48, dl)).arg(ml)); break;
    case ENUM_MSG_TYPE_UPLOADFILE_DATA_RESPOND: {
        f.i32("结果码", d, 0, dl);
        const quint32 magic = u32at(d, 40, dl);
        if (magic == 0x5253554d) f.raw(QStringLiteral("下一期望偏移=%1 完成=%2").arg(q64at(d, 8, dl)).arg(int(d[16])));
        break; }
    case ENUM_MSG_TYPE_SHAREFILE_REQUEST:
        f.str("目标用户", d, 0, dl); f.str("文件名", d, 32, dl);
        f.raw(QStringLiteral("文件路径=\"%1\"").arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_SHAREFILE_RESPOND:
    case ENUM_MSG_TYPE_SHAREFILE_AGREE_RESPOND:
        f.i32("结果码", d, 0, dl); break;
    case ENUM_MSG_TYPE_SHAREFILE_AGREE_REQUEST:
        f.str("同意人", d, 0, dl); f.str("分享人", d, 32, dl);
        f.raw(QStringLiteral("文件名=\"%1\"").arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_SHAREFILE_FINISH_RESPOND:
        f.i32("结果码", d, 0, dl); f.str("分享人", d, 0, dl); f.str("接收人", d, 32, dl);
        f.raw(QStringLiteral("文件=\"%1\"").arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_MOVEFILE_REQUEST:
        f.str("文件/目录名", d, 0, dl); f.str("移动后名", d, 32, dl);
        f.raw(QStringLiteral("源路径=\"%1\" 目标目录=\"%2\"").arg(fixedStr(m, 0, ml)).arg(fixedStr(m, int(qMin(ml - 1, 210)), ml))); break;
    case ENUM_MSG_TYPE_MOVEFILE_RESPOND:
        f.i32("结果码", d, 0, dl); f.str("回显最终名", d, 32, dl); break;
    case ENUM_MSG_TYPE_DOWNLOADFILE_REQUEST:
        f.raw(QStringLiteral("任务ID=%1 文件名=\"%2\" 路径=\"%3\"")
              .arg(q64at(d, 16, dl)).arg(fixedStr(d, 32, dl)).arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND:
        f.i32("结果码", d, 0, dl);
        f.raw(QStringLiteral("任务ID=%1 大小=%2 文件名=\"%3\"")
              .arg(q64at(d, 16, dl)).arg(q64at(d, 0, dl) == 0 ? QString("见caData[4,16)") : QString("见上"))
              .arg(fixedStr(d, 32, dl))); break;
    case ENUM_MSG_TYPE_DOWNLOADFILE_DATA:
        f.raw(QStringLiteral("[数据块] 数据=%1B").arg(ml)); break;
    case ENUM_MSG_TYPE_DOWNLOADFILE_FINISH:
        f.raw(QStringLiteral("[下载完成]")); break;
    case ENUM_MSG_TYPE_CAPTCHA_RESPOND: {
        f.i32("结果码", d, 0, dl); f.i32("冷却/剩余秒", d, 4, dl);
        if (ml >= 4 && memcmp(m, "\x89PNG", 4) == 0)
            f.raw(QStringLiteral("载荷=PNG 图像(%1B)").arg(ml));
        else
            f.raw(QStringLiteral("载荷=文本答案(长度=%1，不回显)").arg(ml));
        break; }
    case ENUM_MSG_TYPE_GETUSERINFORMATION_REQUEST:
        f.str("查询账号", d, 0, dl); break;
    case ENUM_MSG_TYPE_GETUSERINFORMATION_RESPOND:
        f.i32("结果码", d, 0, dl);
        f.raw(QStringLiteral("载荷=%1B %2").arg(ml).arg(hexHead(m, ml, 24).constData())); break;
    case ENUM_MSG_TYPE_LOGOFF_REQUEST:
        f.mask("确认密码", d, 0, dl); break;
    case ENUM_MSG_TYPE_LOGOFF_RESPOND:
        f.i32("结果码", d, 0, dl); break;
    case ENUM_MSG_TYPE_CREATE_PUBLINK_REQUEST:
    case ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_REQUEST:
        f.raw(QStringLiteral("相对路径=\"%1\"").arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_CREATE_PUBLINK_RESPOND:
    case ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_RESPOND:
        f.i32("结果码", d, 0, dl);
        f.raw(QStringLiteral("链接=\"%1\"").arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_RESOLVE_PUBLINK_REQUEST:
        f.raw(QStringLiteral("token=\"%1\"").arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_RESOLVE_PUBLINK_RESPOND:
        f.i32("结果码", d, 0, dl);
        f.raw(QStringLiteral("载荷=%1B %2").arg(ml).arg(hexHead(m, ml, 24).constData())); break;
    case ENUM_MSG_TYPE_LISTDIR_REQUEST:
        f.raw(QStringLiteral("相对目录=\"%1\"").arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_LISTDIR_RESPOND: {
        f.i32("结果码", d, 0, dl); f.raw(QStringLiteral("已截断=%1 本页条数=%2").arg(int(d[4])).arg(u32at(d, 8, dl)));
        const int cnt = qMin(u32at(d, 8, dl), quint32(ml / int(sizeof(FileInfo))));
        for (int i = 0; i < qMin(cnt, 3); ++i) f.raw(QStringLiteral("  子目录[%1] \"%2\"").arg(i).arg(fixedStr(m, int(i * sizeof(FileInfo)), ml)));
        if (cnt > 3) f.raw(QStringLiteral("  …共 %1 条").arg(cnt));
        break; }
    case ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_REQUEST:
        f.raw(QStringLiteral("版本=%1 魔数=0x%2 大小=%3 摘要=%4")
              .arg(u32at(d, 0, dl)).arg(u32at(d, 4, dl), 8, 16, QChar('0'))
              .arg(q64at(d, 8, dl)).arg(fixedStr(m, 0, ml))); break;
    case ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_RESPOND:
        f.i32("结果码", d, 0, dl); f.raw(QStringLiteral("秒传命中=%1").arg(int(d[4])));
        break;
    case ENUM_MSG_TYPE_UPLOADFILE_ABORT_REQUEST:
        f.raw(QStringLiteral("任务ID=%1").arg(q64at(d, 48, dl))); break;
    case ENUM_MSG_TYPE_UPLOADFILE_ABORT_RESPOND:
        f.i32("结果码", d, 0, dl); f.raw(QStringLiteral("任务ID=%1").arg(q64at(d, 48, dl))); break;
    case ENUM_MSG_TYPE_CHAT_DELIVER_EVENT:
        f.str("发送人", d, 0, dl); f.q64("消息ID", d, 32, dl);
        f.raw(QStringLiteral("消息: %1").arg(msgRecordPreview(m, ml))); break;
    case ENUM_MSG_TYPE_CHAT_DELIVER_ACK:
        f.raw(QStringLiteral("确认消息ID数=%1").arg(ml / 8)); break;
    case ENUM_MSG_TYPE_CHAT_PULL_PENDING_REQUEST:
        f.str("对方", d, 0, dl); break;   // 实际布局：首段=对方，收件人取连接身份
    case ENUM_MSG_TYPE_CHAT_PULL_PENDING_RESPOND: {
        f.i32("结果码", d, 0, dl);
        f.raw(QStringLiteral("已截断=%1 本批条数=%2").arg(int(d[4])).arg(u32at(d, 8, dl)));
        f.raw(QStringLiteral("载荷=%1B").arg(ml)); break; }
    case ENUM_MSG_TYPE_LIST_PENDING_FRIEND_RESPOND: {
        f.i32("结果码", d, 0, dl);
        f.raw(QStringLiteral("已截断=%1 本页条数=%2").arg(int(d[4])).arg(u32at(d, 8, dl)));
        const int cnt = qMin(u32at(d, 8, dl), quint32(ml / 32));
        for (int i = 0; i < qMin(cnt, 3); ++i) f.raw(QStringLiteral("  申请人[%1] \"%2\"").arg(i).arg(fixedStr(m, i * 32, ml)));
        if (cnt > 3) f.raw(QStringLiteral("  …共 %1 条").arg(cnt));
        break; }
    case ENUM_MSG_TYPE_REJECT_FRIEND_REQUEST:
        f.str("被拒申请人", d, 0, dl); break;
    case ENUM_MSG_TYPE_CHAT_HISTORY_REQUEST:
        f.str("查询者", d, 0, dl); f.str("对方", d, 32, dl);
        if (ml >= 12) f.raw(QStringLiteral("游标消息ID=%1(0=最新页) 每页=%2").arg(q64at(m, 0, ml)).arg(u32at(m, 8, ml)));
        break;
    case ENUM_MSG_TYPE_CHAT_HISTORY_RESPOND: {
        f.i32("结果码", d, 0, dl); f.u32("双方历史总数", d, 4, dl);
        f.raw(QStringLiteral("更早还有=%1").arg(int(d[8])));
        if (ml >= 12) {
            f.q64("本页最旧ID(下页游标)", m, 0, ml);
            f.u32("本页条数", m, 8, ml);
        }
        break; }
    default:
        // 群协议 60-106（含群文件 99-106）：20B 信封（版本|信封长|请求ID|群ID），响应其后跟 code(u32)
        if (t >= ENUM_MSG_TYPE_CREATE_GROUP_REQUEST && t <= ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_RESPOND) {
            if (ml >= 20) {
                const char *nm = (t < sizeof(kPduTypeNames) / sizeof(kPduTypeNames[0]))
                        ? kPduTypeNames[t] : "";
                const bool isReq = (strstr(nm, "_REQUEST") != nullptr);
                f.raw(QStringLiteral("信封: 版本=%1 信封长=%2 请求ID=%3 群ID=%4")
                      .arg(*reinterpret_cast<const quint16 *>(m)).arg(*reinterpret_cast<const quint16 *>(m + 2))
                      .arg(q64at(m, 4, ml)).arg(q64at(m, 12, ml)));
                const int bodyOff = isReq ? 20 : 24;
                if (!isReq && ml >= bodyOff + 4) f.i32("结果码", m, 20, ml);
                f.raw(QStringLiteral("业务载荷=%1B %2").arg(qMax(ml - bodyOff, 0))
                      .arg(hexHead(m + bodyOff, qMax(ml - bodyOff, 0), 16).constData()));
            }
            break;
        }
        f.raw(QStringLiteral("未解码类型，caData=%1 caMsg=%2")
              .arg(hexHead(d, dl, 16).constData()).arg(hexHead(m, ml, 16).constData()));
        break;
    }
    return f.lines.join(QLatin1Char('\n'));
}
} // namespace

// 每帧输出：首行=方向与长度，随后逐行字段；密码打码、心跳跳过、大块数据只记偏移与长度。
void logPduFrame(const char *dir, const PDU *pdu)
{
    if (pdu == nullptr) return;
    const uint type = pdu->uiType;
    if (type == ENUM_MSG_TYPE_HEARTBEAT_REQUEST || type == ENUM_MSG_TYPE_HEARTBEAT_RESPOND) {
        return;
    }
    const char *typeName = (type < sizeof(kPduTypeNames) / sizeof(kPduTypeNames[0]))
            ? kPduTypeNames[type] : "?";
    qCDebug(lcPdu, "%s type=%u(%s) total=%u msg=%u",
            dir, type, typeName, pdu->uiTotalLen, pdu->uiMsgLen);
    const QStringList lines = decodePdu(pdu).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (!line.isEmpty()) qCDebug(lcPdu, "      %s", line.toUtf8().constData());
    }
}
