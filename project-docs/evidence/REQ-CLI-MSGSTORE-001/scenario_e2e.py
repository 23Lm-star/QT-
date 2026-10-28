# -*- coding: utf-8 -*-
"""REQ-CLI-MSGSTORE-001 完整功能场景回归（群消息 + 私聊，协议层）。

为什么要它：既有的 chat16_e2e 只钉住 16 应答本身，群侧与私聊侧混在一起的真实场景
（离线补发、成员事件占号、断线洞补齐、上翻翻页、已读游标）没有一条端到端串起来跑过。
本套件按用户实际操作顺序走一遍，并把客户端本地存储赖以工作的每一条服务端契约都断言住。

本地存储的落库语义（水位、越界、占号、去重、截断、清空、重启恢复）由同目录
verify.py 的断言探针直接编译产品源码 Client/messagestore.cpp 覆盖；本套件负责
"服务端确实按客户端假设的方式回数据"这一半。两半合起来才是完整链路。

用法：
    py -3.13 -X utf8 project-docs/evidence/REQ-CLI-MSGSTORE-001/scenario_e2e.py

前置与数据域：MySQL 可达（读 Server/connect.config）；TCP 5000 空闲（服务端由本套件
自起自停，只按 PID 收尾，不杀别人的进程）。本套件创建/复用账号 sc_a、sc_b、sc_c，
删除群名前缀 "SC16 " 的群及其 group_message/成员/邀请行，删除这三个账号之间的
message_info 行；不触碰其它账号与群。
"""
import hashlib
import socket
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DIST = ROOT / 'dist'
EXE = DIST / 'Server.exe'

sys.path.insert(0, str(ROOT / 'project-docs' / 'evidence' / '_harness'))
import server_ctl   # noqa: E402
import pymysql      # noqa: E402

HOST, PORT, HDR = '127.0.0.1', 5000, 76
PWD = '123456'
A, B, C = 'sc_a', 'sc_b', 'sc_c'
GROUP_PREFIX = 'SC16 '

T_REG, T_REG_R, T_LOGIN, T_LOGIN_R, T_CAP, T_CAP_R = 1, 2, 3, 4, 44, 45
T_CHAT, T_CHAT_R, T_GETCHAT, T_GETCHAT_R = 15, 16, 17, 18
T_CREATE, T_CREATE_R = 60, 61
T_INVITE, T_INVITE_R = 62, 63
T_ACCEPT, T_ACCEPT_R = 64, 65
T_REMOVE, T_REMOVE_R = 70, 71
T_LIST, T_LIST_R = 78, 79
T_SEND, T_SEND_R, T_EVENT = 88, 89, 90
T_PULL, T_PULL_R = 91, 92
T_CONFIRM, T_CONFIRM_R = 93, 94
T_UNREAD, T_UNREAD_R = 95, 96

MSG_HDR, MSG_STRIDE = 38, 40
RESULTS = []


def check(label, cond, detail=''):
    RESULTS.append((label, bool(cond)))
    print(('[PASS] ' if cond else '[FAIL] ') + label + (' :: ' + str(detail) if detail else ''))
    sys.stdout.flush()


def f32(s):
    return s.encode()[:31].ljust(32, b'\0')


def u16(x):
    return struct.pack('<H', x)


def u32(x):
    return struct.pack('<I', x)


def u64(x):
    return struct.pack('<Q', x)


def string(x):
    b = x.encode() if isinstance(x, str) else x
    return u32(len(b)) + b


def pdu(t, msg=b'', dat=b''):
    return struct.pack('<III', HDR + len(msg), len(msg), t) + dat[:64].ljust(64, b'\0') + msg


def envelope(gid=0, body=b''):
    return u16(1) + u16(20) + u64(int(time.time_ns() & ((1 << 63) - 1))) + u64(gid) + body


def chat_body(content, time_text):
    body = content.encode() + b'\0'
    return struct.pack('<II', MSG_STRIDE + len(body), len(body)) + time_text.encode()[:29].ljust(30, b'\0') + body


def parse_chat(payload):
    if len(payload) < MSG_HDR:
        return None, None
    _total, msg_len = struct.unpack_from('<II', payload, 0)
    return (payload[8:38].split(b'\0')[0].decode('utf-8', 'replace'),
            payload[MSG_HDR:MSG_HDR + max(0, msg_len - 1)].decode('utf-8', 'replace'))


def unpack(buf):
    out = []
    while len(buf) >= HDR:
        tl, ml, ty = struct.unpack('<III', buf[:12])
        if tl < HDR or tl != HDR + ml or len(buf) < tl:
            break
        out.append((ty, buf[12:76], buf[76:tl]))
        buf = buf[tl:]
    return out, buf


class Rd:
    def __init__(self, d):
        self.d, self.o = d, 0

    def u8(self):
        v = self.d[self.o]; self.o += 1; return v

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.o)[0]; self.o += 4; return v

    def u64(self):
        v = struct.unpack_from('<Q', self.d, self.o)[0]; self.o += 8; return v

    def string(self):
        n = self.u32(); v = self.d[self.o:self.o + n]; self.o += n; return v


class Conn:
    def __init__(self, name=None):
        self.s = socket.create_connection((HOST, PORT), timeout=10)
        self.buf = b''
        self.name = name

    def pump(self, timeout=.25):
        self.s.settimeout(timeout)
        try:
            d = self.s.recv(262144)
            if not d:
                raise ConnectionError('peer closed')
            self.buf += d
        except socket.timeout:
            pass

    def packets(self):
        ps, self.buf = unpack(self.buf)
        return ps

    def send(self, pkt):
        self.s.sendall(pkt)

    def take(self, want, timeout=12):
        """按类型取包：本需求之后发送方连接上会多出 16，任何按位置取包的写法都会串。"""
        got, end = [], time.time() + timeout
        while time.time() < end:
            hit = [p for p in got if p[0] == want]
            if hit:
                return hit
            try:
                self.pump(.2)
            except ConnectionError:
                break
            got.extend(self.packets())
        return [p for p in got if p[0] == want]

    def drain(self, seconds=1.0):
        got, end = [], time.time() + seconds
        while time.time() < end:
            try:
                self.pump(min(.25, max(.05, end - time.time())))
            except ConnectionError:
                break
            got.extend(self.packets())
        return got + self.packets()

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


def cap_code(c):
    c.send(pdu(T_CAP))
    r = c.take(T_CAP_R)
    return r[0][2].split(b'\0')[0] if r else b''


def ensure_user(name):
    c = Conn(name)
    code = cap_code(c)
    if code:
        c.send(pdu(T_REG, msg=code + b'\0',
                   dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()))
        c.take(T_REG_R)
    c.close()


def login(name):
    c = Conn(name)
    code = cap_code(c)
    c.send(pdu(T_LOGIN, msg=code + b'\0',
               dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()))
    r = c.take(T_LOGIN_R)
    ret = struct.unpack('<i', r[0][1][:4])[0] if r else -99
    if ret != 1:
        c.close()
        raise RuntimeError('登录失败 ' + name + ' ret=' + str(ret))
    return c


def greq(c, typ, gid=0, body=b'', timeout=12):
    c.send(pdu(typ, msg=envelope(gid, body)))
    ps = c.take(typ + 1, timeout)
    if not ps:
        return None, b''
    payload = ps[0][2]
    code = struct.unpack('<i', payload[20:24])[0] if len(payload) >= 24 else None
    return code, payload[24:]


def send_group(c, gid, key, content):
    code, body = greq(c, T_SEND, gid, string(key) + string(content))
    out = {'code': code}
    if code == 0 and len(body) >= 24:
        r = Rd(body)
        r.string(); out['seq'] = r.u64(); r.u64(); out['created'] = r.u64()
    return out


def pull(c, gid, mode, cursor, limit=100):
    code, body = greq(c, T_PULL, gid, bytes([mode]) + u64(cursor) + u16(limit))
    out = {'code': code, 'records': []}
    if code != 0 or not body:
        return out
    r = Rd(body)
    r.u8(); r.u8(); r.u64(); r.u64(); r.u64()
    n = r.u32()
    for _ in range(n):
        rec = {'seq': r.u64()}
        r.u64()
        rec['sender'] = r.string().decode('utf-8', 'replace')
        rec['type'] = r.u8()
        r.u64()
        rec['content'] = r.string().decode('utf-8', 'replace')
        out['records'].append(rec)
    return out


def unread(c):
    code, body = greq(c, T_UNREAD, 0)
    out = {'code': code, 'rows': {}}
    if code != 0 or not body:
        return out
    r = Rd(body)
    n = r.u32()
    for _ in range(n):
        gid = r.u64(); last_seq = r.u64(); last_read = r.u64(); unread_cnt = r.u64()
        out['rows'][gid] = {'last_seq': last_seq, 'last_read': last_read, 'unread': unread_cnt}
    return out


def db_conn():
    lines = (ROOT / 'Server' / 'connect.config').read_text(encoding='utf-8').splitlines()
    return pymysql.connect(host=lines[3], port=int(lines[4]), user=lines[6],
                           password=lines[7], database=lines[5], autocommit=True)


def cleanup_domain():
    with db_conn() as c, c.cursor() as cur:
        cur.execute("SELECT group_id FROM group_info WHERE group_name LIKE %s", (GROUP_PREFIX + '%',))
        gids = [row[0] for row in cur.fetchall()]
        if gids:
            marks = ','.join(['%s'] * len(gids))
            cur.execute("DELETE FROM group_message WHERE group_id IN (" + marks + ")", gids)
            cur.execute("DELETE FROM group_member WHERE group_id IN (" + marks + ")", gids)
            cur.execute("DELETE FROM group_invitation WHERE group_id IN (" + marks + ")", gids)
            cur.execute("DELETE FROM group_info WHERE group_id IN (" + marks + ")", gids)
        cur.execute("DELETE FROM message_info WHERE seed_name IN (%s,%s,%s) "
                    "OR rev_name IN (%s,%s,%s)", ((A, B, C) * 2))


def main():
    if not EXE.exists():
        print('FATAL: 未找到 ' + str(EXE))
        return 2
    srv = server_ctl.ServerCtl(str(EXE), str(DIST), HOST, PORT)
    if not srv.start():
        print('FATAL: Server 启动失败：' + str(srv.last_error))
        return 2

    conns = []
    try:
        for name in (A, B, C):
            ensure_user(name)
        cleanup_domain()

        # ================= 阶段一：私聊全链路 =================
        ca, cb = login(A), login(B)
        conns += [ca, cb]
        stamp = time.strftime('%Y-%m-%d %H:%M:%S')

        ca.send(pdu(T_CHAT, msg=chat_body('sc-p2p-1', stamp), dat=f32(A) + f32(B)))
        ack = ca.take(T_CHAT_R)
        check('P1_sender_ack_stored', bool(ack) and bool(ack[0][1][32])
              and ack[0][1][:32].split(b'\0')[0].decode() == B, 'ack=' + str(len(ack)))
        got = cb.take(T_CHAT)
        check('P2_receiver_payload', bool(got) and parse_chat(got[0][2])[1] == 'sc-p2p-1',
              'got=' + str(len(got)))

        cb.send(pdu(T_CHAT, msg=chat_body('sc-p2p-reply', stamp), dat=f32(B) + f32(A)))
        ack2 = cb.take(T_CHAT_R)
        got2 = ca.take(T_CHAT)
        check('P3_reverse_direction_works', bool(ack2) and bool(got2)
              and parse_chat(got2[0][2])[1] == 'sc-p2p-reply', '')

        # 离线补发：C 全程不登录
        ca.send(pdu(T_CHAT, msg=chat_body('sc-offline-1', stamp), dat=f32(A) + f32(C)))
        ack3 = ca.take(T_CHAT_R)
        check('P4_offline_stored_ack', bool(ack3) and bool(ack3[0][1][32]), '')
        for text in ('sc-offline-2', 'sc-offline-3'):
            ca.send(pdu(T_CHAT, msg=chat_body(text, stamp), dat=f32(A) + f32(C)))
            ca.take(T_CHAT_R)
        cc = login(C)
        conns.append(cc)
        cc.send(pdu(T_GETCHAT, dat=f32(C) + f32(A)))
        ps = cc.take(T_GETCHAT_R)
        payload = ps[0][2] if ps else b''
        items, off = [], 0
        while off + MSG_HDR <= len(payload):
            total, msg_len = struct.unpack_from('<II', payload, off)
            if total <= 0:
                break
            items.append(payload[off + MSG_HDR:off + MSG_HDR + max(0, msg_len - 1)].decode('utf-8', 'replace').rstrip('\0'))
            off += total
        check('P5_offline_batch_delivered',
              all(x in items for x in ('sc-offline-1', 'sc-offline-2', 'sc-offline-3')),
              'items=' + str(items))
        cc.send(pdu(T_GETCHAT, dat=f32(C) + f32(A)))
        again = cc.take(T_GETCHAT_R)
        check('P6_redelivery_consumed', again and not any(
            'sc-offline' in parse_chat(p[2])[1] for p in again if len(p[2]) >= MSG_HDR),
            'second=' + str(len(again)))

        # ================= 阶段二：群全链路（含成员事件占号） =================
        gid = None
        code, body = greq(ca, T_CREATE, 0, string(GROUP_PREFIX + 'main'))
        gid = struct.unpack('<Q', body[:8])[0] if code == 0 and body else 0
        check('G1_create_group', gid > 0, 'code=' + str(code))
        code, body = greq(ca, T_INVITE, gid, string(B) + string('k-b'))
        iid = struct.unpack('<Q', body[:8])[0] if code == 0 and body else 0
        cb.take(T_EVENT)          # 85 邀请事件（同类型不同信封），这里只保证不残留
        a_code, _ = greq(cb, T_ACCEPT, 0, u64(iid))
        check('G2_invite_accepted', a_code == 0, 'code=' + str(a_code))

        seqs = []
        for i in range(3):
            r = send_group(ca, gid, 'k-%d' % i, 'g-text-%d' % i)
            if 'seq' in r:
                seqs.append(r['seq'])
        check('G3_three_texts_sequential', len(seqs) == 3 and seqs == sorted(seqs)
              and seqs[2] - seqs[0] == 2, 'seqs=' + str(seqs))

        # 成员事件：建群与入群都会写入占号的 system 行（本地存储必须跨过它）。
        # 这里只加成员不移除 B——B 后面还要作为成员做断线补齐，移除它会让那几步变成
        # 「非成员被拒」的正确行为而不是本套件的断言目标。
        code, body = greq(ca, T_INVITE, gid, string(C))
        iid2 = struct.unpack('<Q', body[:8])[0] if code == 0 and body else 0
        cc.take(T_EVENT)
        greq(cc, T_ACCEPT, 0, u64(iid2))
        after = pull(ca, gid, 1, seqs[2] + 50, 100)
        kinds = [(r['seq'], r['type']) for r in after['records']]
        sys_seqs = [s for s, ty in kinds if ty == 1]
        check('G4_system_events_occupy_seq', len(sys_seqs) >= 1, 'kinds=' + str(kinds))
        gaps = [kinds[i][0] - kinds[i - 1][0] for i in range(1, len(kinds))]
        check('G5_seq_dense_across_system', all(g == 1 for g in gaps) or not kinds,
              'gaps=' + str(gaps))

        # 断线洞补齐：B 断开期间 A 再发三条
        b_before = pull(cb, gid, 0, 0, 100)['records']
        cb.close()
        hole_seqs = []
        for i in range(3):
            r = send_group(ca, gid, 'h-%d' % i, 'hole-%d' % i)
            if 'seq' in r:
                hole_seqs.append(r['seq'])
        cb2 = login(B)
        conns.append(cb2)
        greq(cb2, T_UNREAD, 0)
        filled = pull(cb2, gid, 0, 0, 100)
        got_seqs = [r['seq'] for r in filled['records']]
        check('G6_hole_forward_fill_returns_missing_range',
              all(s in got_seqs for s in hole_seqs), 'want=' + str(hole_seqs) + ' got=' + str(got_seqs))
        dense = sorted(set(got_seqs))
        check('G7_forward_fill_dense', dense == list(range(dense[0], dense[-1] + 1)) if dense else False,
              'dense=' + str(dense))

        # 已读游标：推进后不得回退
        code, body = greq(cb2, T_CONFIRM, gid, u64(hole_seqs[-1]))
        eff = struct.unpack('<Q', body[:8])[0] if code == 0 and body else None
        code2, body2 = greq(cb2, T_CONFIRM, gid, u64(1))
        eff2 = struct.unpack('<Q', body2[:8])[0] if code2 == 0 and body2 else None
        check('G8_read_cursor_monotonic_clamp', eff == hole_seqs[-1] and eff2 == hole_seqs[-1],
              'eff=' + str(eff) + ' clamp=' + str(eff2))

        # 未读摘要与水位/游标口径一致
        rows = unread(ca)['rows']
        ca_last = rows.get(gid, {})
        check('G9_unread_matches_water_minus_cursor',
              ca_last.get('last_seq', 0) >= ca_last.get('last_read', 0)
              and ca_last.get('unread') == ca_last.get('last_seq', 0) - ca_last.get('last_read', 0),
              'row=' + str(ca_last))

        # 上翻翻页：seq<=cursor 含边界，不重复返回游标之后的
        page = pull(ca, gid, 1, hole_seqs[0], 50)
        check('G10_backward_page_bounded',
              all(r['seq'] <= hole_seqs[0] for r in page['records']),
              'n=' + str(len(page['records'])))

        # ================= 阶段三：交叉不干扰 =================
        ca.send(pdu(T_CHAT, msg=chat_body('sc-mix', stamp), dat=f32(A) + f32(B)))
        mixed = ca.drain(1.5)
        check('G11_private_ack_not_confused_with_group',
              any(p[0] == T_CHAT_R for p in mixed) and not any(p[0] == T_EVENT for p in mixed),
              'types=' + str(sorted(set(p[0] for p in mixed))))
        b_mixed = cb2.take(T_CHAT)
        check('G12_private_push_arrives', bool(b_mixed) and parse_chat(b_mixed[0][2])[1] == 'sc-mix', '')

    finally:
        for c in conns:
            c.close()
        try:
            cleanup_domain()
        except Exception as e:
            print('[warn] 数据域清理失败：' + str(e))
        srv.stop()

    failed = [n for n, ok in RESULTS if not ok]
    print('\nSUMMARY ' + str(len(RESULTS) - len(failed)) + '/' + str(len(RESULTS)) +
          (' PASS' if not failed else ' FAIL: ' + ', '.join(failed)))
    return 0 if not failed else 1


if __name__ == '__main__':
    sys.exit(main())
