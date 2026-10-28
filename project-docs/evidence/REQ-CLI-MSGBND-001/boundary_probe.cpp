// REQ-CLI-MSGBND-001 私聊正文边界纯函数探针（无网络、无数据库、无界面）。
// 编译的是产品实现 Server/protocol.cpp，不是复制的等价实现。
//
// 用例只断言返回值、长度与解码结果：越界读不会稳定崩在这里，
// 「畸形报文把服务端进程打死」这类后果由同目录 boundary_e2e.py 用进程存活断言覆盖。
#include "Server/protocol.h"

#include <QByteArray>
#include <QString>
#include <cstdio>

// 探针不使用产品内存池：Server/protocol.cpp 的 mkPDU 引用 MemAlloc::allocate/deallocate，
// 这里给一对最小实现只为让被测的正文边界函数能单独编译链接，不替代任何被测代码。
namespace MemAlloc {
void *allocate(size_t size) { return ::malloc(size); }
void deallocate(void *ptr) { ::free(ptr); }
}


static int g_failed = 0;

static void check(const char *id, bool ok, const QString &detail = QString())
{
    printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", id,
           detail.isEmpty() ? "" : " :: ", detail.toUtf8().constData());
    fflush(stdout);
    if (!ok) ++g_failed;
}

// 按线协议拼一条 Msg 记录：totalLen@0、msgLen@4、Time@8..37、正文@38
static QByteArray makeRecord(const QByteArray &body, const QByteArray &timeText,
                             quint32 declaredMsgLen = 0xFFFFFFFFu,
                             quint32 declaredTotalLen = 0xFFFFFFFFu,
                             bool withTrailingNul = true)
{
    QByteArray rec;
    const quint32 ml = (declaredMsgLen == 0xFFFFFFFFu) ? quint32(body.size() + (withTrailingNul ? 1 : 0))
                                                       : declaredMsgLen;
    const quint32 tl = (declaredTotalLen == 0xFFFFFFFFu) ? quint32(sizeof(Msg) + body.size()
                                                                   + (withTrailingNul ? 1 : 0))
                                                          : declaredTotalLen;
    rec.append(reinterpret_cast<const char *>(&tl), 4);
    rec.append(reinterpret_cast<const char *>(&ml), 4);
    QByteArray t = timeText;
    t.resize(30);
    for (int i = 0; i < 30; ++i) {
        if (i >= timeText.size()) t[i] = '\0';
    }
    rec.append(t);
    rec.append(body);
    if (withTrailingNul) rec.append('\0');
    // sizeof(Msg)=40 而正文偏移 38：正常记录尾部有 2 字节填充
    while (uint(rec.size()) < tl) rec.append('\0');
    return rec;
}

int main()
{
    // 编译期确认布局假设：正文偏移 38、记录头 40（与两端 protocol.h 注释一致）
    static_assert(sizeof(Msg) == 40, "Msg 记录头必须为 40 字节");
    static_assert(CHAT_MSG_BODY_OFFSET == 38, "Msg 正文偏移必须为 38 字节");

    // ===== utf8TrimIncomplete =====
    {
        const QByteArray a("hello");
        check("P1_ascii_untouched", utf8TrimIncomplete(a) == a);
    }
    {
        const QByteArray zh = QString::fromUtf8("好的").toUtf8();      // 6 字节，2 个完整码点
        check("P2_complete_cjk_untouched", utf8TrimIncomplete(zh) == zh);
        const QByteArray cut = zh.left(5);                             // 「的」只剩 2 字节
        check("P3_half_codepoint_trimmed", utf8TrimIncomplete(cut) == zh.left(3),
              QString::fromUtf8(utf8TrimIncomplete(cut)));
    }
    {
        const QByteArray em = QString::fromUtf8("\xF0\x9F\x98\x80").toUtf8();  // emoji，4 字节
        check("P4_emoji_complete_untouched", utf8TrimIncomplete(em) == em);
        for (int keep = 1; keep <= 3; ++keep) {
            const QByteArray cut = em.left(keep);                      // 起始字节 + 0~2 个续字节
            check(QString("P5_emoji_truncated_to_%1_drops_all").arg(keep).toUtf8().constData(),
                  utf8TrimIncomplete(cut).isEmpty());
        }
    }
    {
        const QByteArray onlyCont("\xBC\xBC", 2);                      // 整段都是续字节
        check("P6_only_continuation_bytes_yield_empty", utf8TrimIncomplete(onlyCont).isEmpty());
        const QByteArray badLead("\xF8\xA0", 2);                       // 非法起始字节
        check("P7_invalid_lead_dropped", utf8TrimIncomplete(badLead).isEmpty());
        const QByteArray tail("ok\xF8", 3);                            // 尾部非法起始字节
        check("P8_valid_prefix_kept", utf8TrimIncomplete(tail) == QByteArray("ok", 2));
    }
    {
        // 前面完整 + 尾部残半个码点：必须只裁掉残的部分
        const QByteArray zh = QString::fromUtf8("中文").toUtf8();      // 6 字节
        const QByteArray cut = zh + QByteArray(zh.constData(), 2);
        check("P9_tail_half_dropped_prefix_kept", utf8TrimIncomplete(cut) == zh);
    }

    // ===== readChatMsg（接收端：钳制 + 裁码点，不拒绝） =====
    {
        const QByteArray body = QString::fromUtf8("私聊正文").toUtf8();
        const QByteArray rec = makeRecord(body, "2026-10-28 12:00:00");
        QByteArray got;
        QString tm;
        const bool ok = readChatMsg(rec.constData(), uint(rec.size()), &got, &tm);
        check("P10_normal_roundtrip", ok && got == body && tm == "2026-10-28 12:00:00",
              QString("body=%1 time=%2").arg(QString::fromUtf8(got), tm));
    }
    {
        // 声明 msgLen 越出实际携带字节：按实际长度钳制（旧写法会按声明值拷贝 → 溢出）
        const QByteArray body("x", 10);
        QByteArray rec = makeRecord(body, "t", 4000);
        QByteArray got;
        QString tm;
        const bool ok = readChatMsg(rec.constData(), uint(rec.size()), &got, &tm);
        check("P11_declared_overrun_clamped_to_available",
              ok && got.size() == int(rec.size()) - int(CHAT_MSG_BODY_OFFSET),
              QString("want=%1 got=%2").arg(rec.size() - CHAT_MSG_BODY_OFFSET).arg(got.size()));
    }
    {
        const QByteArray rec = makeRecord(QByteArray("abc"), "t");
        QByteArray got;
        check("P12_short_buffer_rejected",
              !readChatMsg(rec.constData(), 20, &got, nullptr));
        check("P13_null_pointer_rejected", !readChatMsg(nullptr, 100, &got, nullptr));
    }
    {
        // 钳制点正好落在码点中间：正文 3 个汉字（9 字节），报文被截到只剩 8 个正文字节，
        // 裁到最后完整码点后必须是 6 字节「好中」，界面不能出现替换字符
        const QByteArray zh3 = QString::fromUtf8("好中文").toUtf8();
        QByteArray rec = makeRecord(zh3, "t");
        rec.resize(int(CHAT_MSG_BODY_OFFSET + zh3.size() - 1));   // 实际携带少 1 字节 → 切断最后一个码点
        QByteArray got;
        QString tm;
        const bool ok = readChatMsg(rec.constData(), uint(rec.size()), &got, &tm);
        const QString shown = QString::fromUtf8(got);
        check("P14_clamped_result_is_valid_utf8",
              ok && got == zh3.left(6) && !shown.contains(QChar(0xFFFD))
                  && utf8TrimIncomplete(got) == got,
              QString("bytes=%1 text=%2").arg(got.size()).arg(shown));
    }
    {
        // Time 填满 30 字节且没有终止符：取值必须是这 30 字节内容，不能越界继续读
        const QByteArray fullTime = QByteArray("2026-10-28T00:00:0001234567890", 30);   // 恰好填满，无终止符
        const QByteArray rec = makeRecord(QByteArray("b"), fullTime);
        QByteArray got;
        QString tm;
        const bool ok = readChatMsg(rec.constData(), uint(rec.size()), &got, &tm);
        check("P15_time_full_30_bytes_no_terminator",
              ok && tm == QString::fromUtf8(fullTime) && tm.size() == 30,
              QString("len=%1 text=%2").arg(tm.size()).arg(tm));
    }
    {
        const QByteArray shortTime = "2026-10-28 12:00:00";
        const QByteArray rec = makeRecord(QByteArray("b"), shortTime);
        QByteArray got;
        QString tm;
        readChatMsg(rec.constData(), uint(rec.size()), &got, &tm);
        check("P16_time_stops_at_first_nul", tm == QString::fromUtf8(shortTime), tm);
    }

    // ===== checkChatMsgStrict（服务端权威：越限即拒） =====
    {
        const QByteArray body(2000, 'a');
        const QByteArray rec = makeRecord(body, "t");
        QByteArray got;
        QString tm, why;
        const bool ok = checkChatMsgStrict(rec.constData(), uint(rec.size()),
                                           CHAT_MAX_BODY_BYTES, &got, &tm, &why);
        check("P17_exactly_2000_accepted", ok && got == body, why);
    }
    {
        const QByteArray body(2001, 'a');
        const QByteArray rec = makeRecord(body, "t");
        QByteArray got;
        QString tm, why;
        const bool ok = checkChatMsgStrict(rec.constData(), uint(rec.size()),
                                           CHAT_MAX_BODY_BYTES, &got, &tm, &why);
        check("P18_2001_rejected_with_length_reason",
              !ok && why.contains("2001") && why.contains("2000") && got.isEmpty(), why);
    }
    {
        QByteArray rec = makeRecord(QByteArray("x"), "t", 1);          // msgLen=1 → 正文 0 字节
        QByteArray got;
        QString tm, why;
        check("P19_empty_body_rejected",
              !checkChatMsgStrict(rec.constData(), uint(rec.size()), 2000, &got, &tm, &why), why);

        rec = makeRecord(QByteArray("x"), "t", 0);                     // msgLen=0
        check("P20_zero_msglen_rejected",
              !checkChatMsgStrict(rec.constData(), uint(rec.size()), 2000, &got, &tm, &why), why);

        rec = makeRecord(QByteArray("x"), "t", 4000);                  // 声明越出实际携带
        check("P21_declared_overrun_rejected",
              !checkChatMsgStrict(rec.constData(), uint(rec.size()), 2000, &got, &tm, &why), why);

        const QByteArray normal = makeRecord(QByteArray("abc"), "t");
        check("P22_record_header_missing_rejected",
              !checkChatMsgStrict(normal.constData(), 20, CHAT_MAX_BODY_BYTES, &got, &tm, &why), why);
        check("P23_null_rejected",
              !checkChatMsgStrict(nullptr, 100, CHAT_MAX_BODY_BYTES, &got, &tm, &why), why);
    }
    {
        // 拒绝路径不得把正文明文带进原因串（原因只含长度）
        const QByteArray secret = QString::fromUtf8("机密内容").toUtf8();
        const QByteArray rec = makeRecord(secret, "t", quint32(secret.size() + 1));
        QByteArray got;
        QString tm, why;
        // 用 maxBody=1 逼出超限拒绝
        const bool ok = checkChatMsgStrict(rec.constData(), uint(rec.size()), 1, &got, &tm, &why);
        check("P24_reason_holds_no_plaintext",
              !ok && !why.contains(QString::fromUtf8(secret)),
              why);
    }

    // ===== chatMsgRecordStride（18 连续记录步进） =====
    {
        const QByteArray r1 = makeRecord(QByteArray("first"), "t1");
        const QByteArray r2 = makeRecord(QString::fromUtf8("第二条").toUtf8(), "t2");
        QByteArray joined = r1 + r2;
        const uint s1 = chatMsgRecordStride(joined.constData(), uint(joined.size()), 0);
        const uint s2 = chatMsgRecordStride(joined.constData(), uint(joined.size()), s1);
        check("P25_two_records_stride_matches",
              s1 == quint32(r1.size()) && s2 == quint32(r2.size()),
              QString("s1=%1 s2=%2 r1=%3 r2=%4").arg(s1).arg(s2).arg(r1.size()).arg(r2.size()));

        // 逐条取用后内容与顺序都要对得上
        QByteArray b1, b2, tm1, tm2;
        QString t1, t2;
        readChatMsg(joined.constData(), s1, &b1, &t1);
        readChatMsg(joined.constData() + s1, s2, &b2, &t2);
        check("P26_records_extracted_in_order",
              b1 == "first" && b2 == QString::fromUtf8("第二条").toUtf8() && t1 == "t1" && t2 == "t2",
              QString("%1/%2").arg(t1, t2));
    }
    {
        const QByteArray r1 = makeRecord(QByteArray("only"), "t");
        // totalLen=0：旧写法 dis += 0 会死循环
        QByteArray zero = r1;
        quint32 z = 0;
        zero.replace(0, 4, reinterpret_cast<const char *>(&z), 4);
        check("P27_zero_total_len_stops", chatMsgRecordStride(zero.constData(), uint(zero.size()), 0) == 0);

        // 记录越出报文：尾部被截断
        QByteArray trunc = r1.left(r1.size() - 2);
        check("P28_record_beyond_payload_stops",
              chatMsgRecordStride(trunc.constData(), uint(trunc.size()), 0) == 0);

        // msgLen > totalLen：自相矛盾
        QByteArray bad = r1;
        const quint32 big = 9999;
        bad.replace(4, 4, reinterpret_cast<const char *>(&big), 4);
        check("P29_msglen_gt_totallen_stops", chatMsgRecordStride(bad.constData(), uint(bad.size()), 0) == 0);

        // 不足一条记录头
        check("P30_less_than_one_header_stops",
              chatMsgRecordStride(r1.constData(), 20, 0) == 0
                  && chatMsgRecordStride(r1.constData(), uint(r1.size()), uint(r1.size())) == 0);
    }

    printf("\nPROBE %s failed=%d\n", g_failed ? "FAIL" : "PASS", g_failed);
    return g_failed ? 1 : 0;
}
