// REQ-CLI-MSGSTORE-001：接收消息本地存储的存储层断言探针。
//
// 为什么需要它：本项目的回归套件是 socket 级 python，驱动不了 Qt 客户端的本地文件读写。
// 存储层因此抽成不依赖界面与网络的独立类（Client/messagestore.h/.cpp），由本探针
// **直接编译产品源码**来断言，而不是复制一份等价实现——复制出来的实现通过测试
// 不能证明产品代码正确。
//
// 编译与运行由同目录 verify.py 负责（工具链与项目一致：MinGW g++ + Qt 5.14.2）。
//
// 覆盖 PRD 验收标准「存储一」：水位推进、越界不落库、补洞晋升、重复幂等、
// 500 条截断、user 列隔离、群/私聊分库、写失败与损坏降级；另覆盖清空按钮语义、
// 注销清理、方向还原与重启持久化。
//
// 每个用例使用独立临时目录：水位是按会话存在库里的，共用目录会让前一个用例的
// 水位串进后一个用例，断言就成了互相依赖。
//
// 输出：[PASS]/[FAIL] <id> <详情>，末行 RESULT n/m PASS；全通过时退出码为 0。

#include "Client/messagestore.h"

#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>

#include <cstdio>
#include <string>

static int g_total = 0;
static int g_passed = 0;

static void check(const char *id, bool ok, const std::string &detail)
{
    ++g_total;
    if (ok) ++g_passed;
    std::printf("[%s] %s :: %s\n", ok ? "PASS" : "FAIL", id, detail.c_str());
}

static std::string s(const QString &v) { return v.toStdString(); }
static std::string n(long long v) { return std::to_string(v); }
static QString k(const char *v) { return QString::fromLatin1(v); }

// 每个用例一套独立临时目录；创建失败时路径为空，用例会以"打开失败"的形式暴露
struct Case
{
    QTemporaryDir tmp;
    QString path() const { return tmp.isValid() ? tmp.path() : QString(); }
};

// 写入一条并断言结果为 Stored 且水位推进到 expectWaterAfter
static bool put(MessageStore &st, MsgStoreKind kind, const QString &conv, quint64 seq,
                quint64 expectWaterAfter)
{
    MessageStore::AppendResult r =
        st.append(kind, conv, seq, QStringLiteral("alice"), 0, 1700000000000LL + qint64(seq),
                  QStringLiteral("m%1").arg(seq));
    if (r != MessageStore::AppendResult::Stored) return false;
    return st.waterSeq(kind, conv) == expectWaterAfter;
}

static void seed(MessageStore &st, MsgStoreKind kind, const QString &conv, quint64 upto)
{
    for (quint64 seq = 1; seq <= upto; ++seq)
        st.append(kind, conv, seq, QStringLiteral("a"), 0, 1, QStringLiteral("m%1").arg(seq));
}

int main()
{
    // S1 首次打开建表：无历史，水位 0，读回空
    {
        Case c;
        MessageStore st;
        bool opened = st.open(QStringLiteral("alice"), c.path());
        check("S1_open_creates_schema",
              opened && st.available(MsgStoreKind::Group) && st.available(MsgStoreKind::Chat)
                  && st.waterSeq(MsgStoreKind::Group, k("7")) == 0
                  && st.loadRecent(MsgStoreKind::Group, k("7"), 100).isEmpty(),
              "open=" + n(opened) + " err=" + s(st.lastError()));
    }

    // S2 连续序号推进水位并按升序读回，正文/时间可还原
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        bool ok = put(st, MsgStoreKind::Group, k("7"), 1, 1)
               && put(st, MsgStoreKind::Group, k("7"), 2, 2)
               && put(st, MsgStoreKind::Group, k("7"), 3, 3);
        QList<StoredMessage> rows = st.loadRecent(MsgStoreKind::Group, k("7"), 100);
        ok = ok && rows.size() == 3 && rows.at(0).seq == 1 && rows.at(2).seq == 3
                 && rows.at(1).content == QStringLiteral("m2")
                 && rows.at(1).createdAtMs == 1700000000002LL;
        check("S2_contiguous_advance", ok, "rows=" + n(rows.size()));
    }

    // S3 越界不落库：水位 3 时直接写 seq=6 必须被拒，水位与行数都不动
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        seed(st, MsgStoreKind::Group, k("7"), 3);
        MessageStore::AppendResult r =
            st.append(MsgStoreKind::Group, k("7"), 6, QStringLiteral("a"), 0, 1, k("beyond"));
        quint64 water = st.waterSeq(MsgStoreKind::Group, k("7"));
        int rows = st.loadRecent(MsgStoreKind::Group, k("7"), 100).size();
        check("S3_beyond_water_not_stored",
              r == MessageStore::AppendResult::RejectedBeyondWater && water == 3 && rows == 3,
              "result=" + n((int)r) + " water=" + n((long long)water) + " rows=" + n(rows));
    }

    // S4 补洞晋升：洞补齐后 4、5、6 依次落库，水位一路推到 6
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        seed(st, MsgStoreKind::Group, k("7"), 3);
        bool ok = put(st, MsgStoreKind::Group, k("7"), 4, 4)
               && put(st, MsgStoreKind::Group, k("7"), 5, 5)
               && put(st, MsgStoreKind::Group, k("7"), 6, 6);
        QList<StoredMessage> rows = st.loadRecent(MsgStoreKind::Group, k("7"), 100);
        check("S4_hole_fill_promotes", ok && rows.size() == 6 && rows.at(5).seq == 6,
              "rows=" + n(rows.size()));
    }

    // S5 重复幂等：已落库的 seq 再写返回 Duplicate，行数与水位不变
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        seed(st, MsgStoreKind::Group, k("7"), 3);
        MessageStore::AppendResult r =
            st.append(MsgStoreKind::Group, k("7"), 2, QStringLiteral("a"), 0, 9, k("dup"));
        int rows = st.loadRecent(MsgStoreKind::Group, k("7"), 100).size();
        check("S5_duplicate_seq_idempotent",
              r == MessageStore::AppendResult::Duplicate && rows == 3
                  && st.waterSeq(MsgStoreKind::Group, k("7")) == 3,
              "result=" + n((int)r) + " rows=" + n(rows));
    }

    // S6 截断只删最旧，水位继续推进（截断不影响水位口径）
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        st.setKeepLimit(5);
        bool ok = true;
        for (quint64 seq = 1; seq <= 8; ++seq)
            ok = ok && put(st, MsgStoreKind::Group, k("7"), seq, seq);
        QList<StoredMessage> rows = st.loadRecent(MsgStoreKind::Group, k("7"), 100);
        ok = ok && rows.size() == 5 && rows.at(0).seq == 4 && rows.at(4).seq == 8;
        check("S6_capacity_truncates_oldest", ok, "rows=" + n(rows.size())
              + (rows.isEmpty() ? "" : " first=" + n((long long)rows.at(0).seq)));
    }

    // S7 群与私聊分库：同一会话键互不可见，两个库文件都落到了磁盘
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        put(st, MsgStoreKind::Group, k("7"), 1, 1);
        put(st, MsgStoreKind::Chat, k("7"), 1, 1);
        QList<StoredMessage> g = st.loadRecent(MsgStoreKind::Group, k("7"), 100);
        QList<StoredMessage> ch = st.loadRecent(MsgStoreKind::Chat, k("7"), 100);
        bool files = st.filePath(MsgStoreKind::Group) != st.filePath(MsgStoreKind::Chat)
                       && QFile::exists(st.filePath(MsgStoreKind::Group))
                       && QFile::exists(st.filePath(MsgStoreKind::Chat));
        check("S7_kind_separated_databases", g.size() == 1 && ch.size() == 1 && files,
              "group=" + n(g.size()) + " chat=" + n(ch.size()) + " files=" + n(files ? 1 : 0));
    }

    // S8 user 列隔离：另一账号读不到 alice 的会话，即便会话键相同
    {
        Case c;
        {
            MessageStore a;
            a.open(QStringLiteral("alice"), c.path());
            seed(a, MsgStoreKind::Chat, k("bob"), 2);
        }
        MessageStore b;
        b.open(QStringLiteral("carol"), c.path());
        int rows = b.loadRecent(MsgStoreKind::Chat, k("bob"), 100).size();
        check("S8_user_column_isolation",
              rows == 0 && b.waterSeq(MsgStoreKind::Chat, k("bob")) == 0,
              "carol_rows=" + n(rows) + " water=" + n((long long)b.waterSeq(MsgStoreKind::Chat, k("bob"))));
    }

    // S9 重启持久化：重新打开同一目录，消息与水位仍在，并能从水位继续写
    {
        Case c;
        {
            MessageStore st;
            st.open(QStringLiteral("alice"), c.path());
            seed(st, MsgStoreKind::Chat, k("bob"), 2);
        }
        MessageStore st2;
        st2.open(QStringLiteral("alice"), c.path());
        QList<StoredMessage> rows = st2.loadRecent(MsgStoreKind::Chat, k("bob"), 100);
        quint64 water = st2.waterSeq(MsgStoreKind::Chat, k("bob"));
        bool cont = put(st2, MsgStoreKind::Chat, k("bob"), 3, 3);
        check("S9_reopen_restores_history",
              rows.size() == 2 && water == 2 && cont,
              "rows=" + n(rows.size()) + " water=" + n((long long)water)
                  + " can_continue=" + n(cont ? 1 : 0));
    }

    // S10 清空按钮语义：删该会话的行，水位保留；已被水位跨过的序号幂等丢弃，水位+1 可继续
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        seed(st, MsgStoreKind::Group, k("7"), 2);
        bool cleared = st.clearConversation(MsgStoreKind::Group, k("7"));
        int rows = st.loadRecent(MsgStoreKind::Group, k("7"), 100).size();
        MessageStore::AppendResult below =
            st.append(MsgStoreKind::Group, k("7"), 1, QStringLiteral("a"), 0, 1, k("back"));
        bool after = put(st, MsgStoreKind::Group, k("7"), 3, 3);
        quint64 water = st.waterSeq(MsgStoreKind::Group, k("7"));
        check("S10_clear_keeps_water",
              cleared && rows == 0 && below == MessageStore::AppendResult::Duplicate
                  && after && water == 3,
              "cleared=" + n(cleared ? 1 : 0) + " rows=" + n(rows) + " below=" + n((int)below)
                  + " water=" + n((long long)water));
    }

    // S11 清空只影响指定会话：其它会话不动
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        seed(st, MsgStoreKind::Chat, k("bob"), 1);
        seed(st, MsgStoreKind::Chat, k("dave"), 1);
        st.clearConversation(MsgStoreKind::Chat, k("bob"));
        int dave = st.loadRecent(MsgStoreKind::Chat, k("dave"), 100).size();
        int bob = st.loadRecent(MsgStoreKind::Chat, k("bob"), 100).size();
        check("S11_clear_scoped_to_conversation", bob == 0 && dave == 1,
              "bob=" + n(bob) + " dave=" + n(dave));
    }

    // S12 账号注销清除该 user 的消息与水位（按 user 列删除，不动他人）
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        seed(st, MsgStoreKind::Chat, k("bob"), 2);
        bool removed = st.removeUser();
        int rows = st.loadRecent(MsgStoreKind::Chat, k("bob"), 100).size();
        quint64 water = st.waterSeq(MsgStoreKind::Chat, k("bob"));
        bool restart = put(st, MsgStoreKind::Chat, k("bob"), 1, 1);   // 水位归零后可重新起算
        check("S12_remove_user_purges_rows",
              removed && rows == 0 && water == 0 && restart,
              "removed=" + n(removed ? 1 : 0) + " rows=" + n(rows) + " water=" + n((long long)water));
    }

    // S13 方向可还原：发送成功的条目落库带方向，读取不丢
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        st.append(MsgStoreKind::Chat, k("zoe"), 1, QStringLiteral("alice"), 1, 123, k("sent-ok"));
        QList<StoredMessage> rows = st.loadRecent(MsgStoreKind::Chat, k("zoe"), 100);
        check("S13_direction_preserved",
              rows.size() == 1 && rows.at(0).direction == 1 && rows.at(0).sender == QStringLiteral("alice"),
              rows.isEmpty() ? "no rows" : ("direction=" + n(rows.at(0).direction)));
    }

    // S14 目录不可用降级：open 失败、available 为假、append 返回 Unavailable 且不崩溃
    {
        Case c;
        const QString blocker = c.path() + QStringLiteral("/blocked.txt");
        QFile f(blocker);
        f.open(QIODevice::WriteOnly);
        f.write("x");
        f.close();
        MessageStore st;
        bool opened = st.open(QStringLiteral("alice"), blocker);   // 路径是普通文件，建不出目录
        MessageStore::AppendResult r =
            st.append(MsgStoreKind::Group, k("7"), 1, QStringLiteral("a"), 0, 1, k("x"));
        check("S14_unavailable_degrades_gracefully",
              !opened && !st.available(MsgStoreKind::Group)
                  && r == MessageStore::AppendResult::Unavailable
                  && st.waterSeq(MsgStoreKind::Group, k("7")) == 0
                  && st.loadRecent(MsgStoreKind::Group, k("7"), 100).isEmpty(),
              "opened=" + n(opened ? 1 : 0) + " result=" + n((int)r) + " err=" + s(st.lastError()));
    }

    // S15 库文件损坏降级：非 SQLite 内容 → 该库 available 为假，另一库不受影响
    {
        Case c;
        MessageStore prepare;
        prepare.open(QStringLiteral("alice"), c.path());          // 先正常建出两个库
        prepare.close();
        const QString groupPath = QDir(c.path()).filePath(MessageStore::fileName(MsgStoreKind::Group));
        QFile::remove(groupPath);
        QFile bad(groupPath);
        bad.open(QIODevice::WriteOnly);
        bad.write("this is definitely not a sqlite database file");
        bad.close();

        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        bool availGroup = st.available(MsgStoreKind::Group);
        bool availChat = st.available(MsgStoreKind::Chat);
        MessageStore::AppendResult r =
            st.append(MsgStoreKind::Group, k("7"), 1, QStringLiteral("a"), 0, 1, k("x"));
        bool chatStillWorks =
            st.append(MsgStoreKind::Chat, k("bob"), 1, QStringLiteral("a"), 0, 1, k("ok"))
                == MessageStore::AppendResult::Stored;
        check("S15_corrupt_db_degrades",
              !availGroup && r == MessageStore::AppendResult::Unavailable && availChat && chatStillWorks,
              "group_avail=" + n(availGroup ? 1 : 0) + " chat_avail=" + n(availChat ? 1 : 0)
                  + " result=" + n((int)r) + " chat_ok=" + n(chatStillWorks ? 1 : 0)
                  + " err=" + s(st.lastError()));
    }

    // S16 水位播种：本地全新（水位 0）而服务端锚点已到 100 时，必须把水位播种到锚点，
    // 否则锚点之后的 101 永远"越界"落不了库；播种只增不减，旧序号不再回填。
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        bool seeded = st.seedWater(MsgStoreKind::Group, k("7"), 100);
        MessageStore::AppendResult stale =
            st.append(MsgStoreKind::Group, k("7"), 5, QStringLiteral("a"), 0, 1, k("old"));
        bool next = put(st, MsgStoreKind::Group, k("7"), 101, 101);
        bool down = st.seedWater(MsgStoreKind::Group, k("7"), 3);   // 试图把水位拉低
        quint64 water = st.waterSeq(MsgStoreKind::Group, k("7"));
        check("S16_seed_water_monotonic",
              seeded && stale == MessageStore::AppendResult::Duplicate && next && !down && water == 101,
              "seeded=" + n(seeded ? 1 : 0) + " stale=" + n((int)stale) + " next=" + n(next ? 1 : 0)
                  + " down=" + n(down ? 1 : 0) + " water=" + n((long long)water));
    }

    // S17 私聊去重键：同 (会话, 去重键) 的第二次投递判为重复，水位与行数都不动
    //     （私聊没有服务端序号，15 实时投递与 18 离线补发可能返回同一条消息，
    //      靠自增序号无法防重，必须按时间与内容去重）
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        bool first = st.append(MsgStoreKind::Chat, k("bob"), 1, QStringLiteral("bob"), 0, 100,
                               k("hi"), QStringLiteral("2026-10-27 10:00:00|hi"))
                     == MessageStore::AppendResult::Stored;
        MessageStore::AppendResult second =
            st.append(MsgStoreKind::Chat, k("bob"), 2, QStringLiteral("bob"), 0, 101, k("hi"),
                      QStringLiteral("2026-10-27 10:00:00|hi"));
        quint64 waterAfterDup = st.waterSeq(MsgStoreKind::Chat, k("bob"));
        // 重复条目不占号：后续消息仍应从 2 起接上
        bool resumed = put(st, MsgStoreKind::Chat, k("bob"), 2, 2);
        int rows = st.loadRecent(MsgStoreKind::Chat, k("bob"), 100).size();
        check("S17_dedup_key_skips_duplicate",
              first && second == MessageStore::AppendResult::Duplicate && waterAfterDup == 1
                  && rows == 2 && resumed,
              "first=" + n(first ? 1 : 0) + " second=" + n((int)second) + " water_after_dup="
                  + n((long long)waterAfterDup) + " rows=" + n(rows) + " resumed=" + n(resumed ? 1 : 0));
    }

    // S18 去重键只在同会话内生效：别的会话相同键照常落库；群侧不带键则不受影响
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        bool a = st.append(MsgStoreKind::Chat, k("bob"), 1, QStringLiteral("bob"), 0, 100, k("hi"),
                           QStringLiteral("T|hi")) == MessageStore::AppendResult::Stored;
        bool b = st.append(MsgStoreKind::Chat, k("dave"), 1, QStringLiteral("dave"), 0, 100, k("hi"),
                           QStringLiteral("T|hi")) == MessageStore::AppendResult::Stored;
        MessageStore::AppendResult g1 =
            st.append(MsgStoreKind::Group, k("7"), 1, QStringLiteral("bob"), 0, 100, k("hi"));
        MessageStore::AppendResult g2 =
            st.append(MsgStoreKind::Group, k("7"), 2, QStringLiteral("bob"), 0, 100, k("hi"));
        check("S18_dedup_scoped_to_conversation",
              a && b && g1 == MessageStore::AppendResult::Stored
                  && g2 == MessageStore::AppendResult::Stored,
              "chat_bob=" + n(a ? 1 : 0) + " chat_dave=" + n(b ? 1 : 0) + " g1=" + n((int)g1)
                  + " g2=" + n((int)g2));
    }

    // S19 原文时间可还原：私聊的协议时间是字符串，必须原样存回来供界面复用；
    //      群侧走协议毫秒时间戳，不填该字段时读回应为空。
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        st.append(MsgStoreKind::Chat, k("bob"), 1, QStringLiteral("bob"), 0, 0, k("hi"),
                  k("D1"), QStringLiteral("2026-10-27 10:00:00"));
        st.append(MsgStoreKind::Group, k("7"), 1, QStringLiteral("bob"), 0, 1700000000000LL, k("hi"));
        QList<StoredMessage> ch = st.loadRecent(MsgStoreKind::Chat, k("bob"), 10);
        QList<StoredMessage> g = st.loadRecent(MsgStoreKind::Group, k("7"), 10);
        check("S19_time_text_roundtrip",
              ch.size() == 1 && ch.at(0).timeText == QStringLiteral("2026-10-27 10:00:00")
                  && g.size() == 1 && g.at(0).timeText.isEmpty()
                  && g.at(0).createdAtMs == 1700000000000LL,
              "chat_time=" + (ch.isEmpty() ? "?" : s(ch.at(0).timeText))
                  + " group_time_empty=" + n(g.isEmpty() ? 0 : (g.at(0).timeText.isEmpty() ? 1 : 0)));
    }

    // S20 暂存队列：水位之上断了洞时，一段都取不出来（越界只留内存）
    {
        MessageStaging q;
        StoredMessage a; a.seq = 5; a.content = k("five");
        StoredMessage b; b.seq = 6; b.content = k("six");
        q.insert(a); q.insert(b);
        check("S20_hole_blocks_promotion", q.size() == 2 && q.takeContiguousAbove(3).isEmpty(),
              "size=" + n(q.size()));
    }

    // S21 补洞后连续段一次取出、按 seq 升序；取走的条目不再留在队列
    {
        MessageStaging q;
        for (quint64 seq = 4; seq <= 7; ++seq) {
            StoredMessage m; m.seq = seq; m.content = QStringLiteral("m%1").arg(seq);
            q.insert(m);
        }
        QList<MessageStaging::Staged> ready = q.takeContiguousAbove(3);
        bool ordered = ready.size() == 4 && ready.at(0).msg.seq == 4 && ready.at(3).msg.seq == 7
                       && ready.at(2).msg.content == QStringLiteral("m6");
        check("S21_contiguous_run_drained", ordered && q.isEmpty(),
              "ready=" + n(ready.size()) + " left=" + n(q.size()));
    }

    // S22 部分连续：洞在中间时只取到洞前，洞后继续等
    {
        MessageStaging q;
        for (quint64 seq : { quint64(4), quint64(5), quint64(7) }) {
            StoredMessage m; m.seq = seq; q.insert(m);
        }
        QList<MessageStaging::Staged> ready = q.takeContiguousAbove(3);
        QList<MessageStaging::Staged> rest = q.takeContiguousAbove(5);
        check("S22_partial_run_stops_at_hole",
              ready.size() == 2 && rest.isEmpty() && q.size() == 1,
              "ready=" + n(ready.size()) + " rest=" + n(rest.size()) + " left=" + n(q.size()));
    }

    // S23 无序号条目（乐观发送、尚未拿到服务端 seq 的）不得进队列
    {
        MessageStaging q;
        StoredMessage optimistic; optimistic.seq = 0;
        bool rejected = !q.insert(optimistic);
        StoredMessage dup; dup.seq = 9;
        bool firstOk = q.insert(dup);
        dup.content = k("second-write-wins-no");
        bool dupOk = q.insert(dup);
        check("S23_zero_seq_rejected_and_idempotent",
              rejected && firstOk && !dupOk && q.size() == 1,
              "rejected=" + n(rejected ? 1 : 0) + " first=" + n(firstOk ? 1 : 0)
                  + " dup=" + n(dupOk ? 1 : 0) + " size=" + n(q.size()));
    }

    // S24 上限保护：长期补不上洞时丢最旧，不允许无界增长（丢掉 1、2、3，留下最新的 4、5）
    {
        MessageStaging q;
        for (quint64 seq = 1; seq <= 5; ++seq) {
            StoredMessage m; m.seq = seq; q.insert(m);
        }
        q.capTo(2);
        QList<MessageStaging::Staged> ready = q.takeContiguousAbove(3);
        bool ok = ready.size() == 2 && ready.at(0).msg.seq == 4 && ready.at(1).msg.seq == 5;
        check("S24_cap_drops_oldest", ok, "ready=" + n(ready.size())
              + (ready.isEmpty() ? "" : (" first=" + n((long long)ready.at(0).msg.seq))));
    }

    // S25 不可存条目必须占号：群里系统事件/成员提示占着 seq 但不入库，
    //     若把它整条丢掉，水位后面就永远缺这一格，后续所有文本消息都被判「越界」，
    //     群消息本地库从那一刻起永久停写。占号推进水位，才能继续接上后面的文本。
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        seed(st, MsgStoreKind::Group, k("7"), 2);            // 水位 2
        MessageStaging q;
        StoredMessage text1; text1.seq = 4; text1.content = k("after-system");
        StoredMessage text2; text2.seq = 5; text2.content = k("and-here");
        q.insert(text1, true);
        q.insert(text2, true);
        // 3 号是系统消息：不可存，但要占号
        StoredMessage system; system.seq = 3;
        QList<MessageStaging::Staged> blocked = q.takeContiguousAbove(2);
        bool stalled = blocked.isEmpty();
        q.reserve(3);                                        // 占号条目入队
        QList<MessageStaging::Staged> drained = q.takeContiguousAbove(2);
        bool ok = stalled && drained.size() == 3 && drained.at(0).storable == false
                  && drained.at(0).msg.seq == 3 && drained.at(1).msg.seq == 4
                  && drained.at(2).msg.seq == 5;
        check("S25_unstorable_hole_blocks_then_resumes", ok,
              "stalled=" + n(stalled ? 1 : 0) + " drained=" + n(drained.size())
                  + " first_storable=" + n(drained.isEmpty() ? -1 : (drained.at(0).storable ? 1 : 0)));
    }

    // S26 advanceWater：只允许按水位+1 单格推进，且不留消息行；越界与倒退都拒绝
    {
        Case c;
        MessageStore st;
        st.open(QStringLiteral("alice"), c.path());
        seed(st, MsgStoreKind::Group, k("7"), 2);
        bool one = st.advanceWater(MsgStoreKind::Group, k("7"), 3);     // 系统消息占号
        bool jump = st.advanceWater(MsgStoreKind::Group, k("7"), 9);    // 跳号必须拒绝
        bool back = st.advanceWater(MsgStoreKind::Group, k("7"), 1);    // 倒退必须拒绝
        bool again = st.advanceWater(MsgStoreKind::Group, k("7"), 3);   // 重复推进拒绝
        quint64 water = st.waterSeq(MsgStoreKind::Group, k("7"));
        int rows = st.loadRecent(MsgStoreKind::Group, k("7"), 100).size();
        bool textOk = put(st, MsgStoreKind::Group, k("7"), 4, 4);       // 占号后文本仍能落库
        check("S26_advance_water_single_step_only",
              one && !jump && !back && !again && water == 3 && rows == 2 && textOk,
              "one=" + n(one ? 1 : 0) + " jump=" + n(jump ? 1 : 0) + " back=" + n(back ? 1 : 0)
                  + " again=" + n(again ? 1 : 0) + " water=" + n((long long)water) + " rows=" + n(rows));
    }

    std::printf("RESULT %d/%d %s\n", g_passed, g_total, g_passed == g_total ? "PASS" : "FAIL");
    return g_passed == g_total ? 0 : 1;
}
