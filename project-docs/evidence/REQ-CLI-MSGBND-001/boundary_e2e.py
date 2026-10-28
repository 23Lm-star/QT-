# -*- coding: utf-8 -*-
"""REQ-CLI-MSGBND-001 私聊消息长度与边界校验 e2e。

覆盖 PRD 验收项（REQ-CLI-MSGBND-001）：
  B1 正文恰好 2000 字节 → 入库、转发、16 回 stored=true（边界一）
  B2 正文 2001 字节 → 拒绝：不入库、不转发、16 回 stored=false（边界二）
  B3 msgLen 声明值越出报文实际携带字节 → 拒绝且不越界读写、服务存活（边界三）
  B4 msgLen=0 / B5 正文 0 字节 / B6 报文不足一条 Msg 头 → 全部拒绝（边界四）
  B7 中文 1998 字节原样往返（多字节五，服务端侧）
  B8 时间串填满 30 字节 → 16 回带与取值都恰好是该串，不外溢（时间七）
  B9 2000 字节消息离线后经 17/18 补发，内容完整（离线六，服务端打包侧）
  B10 被拒后同连接继续正常收发（不踢连接）
  B11 未认证连接发超长仍按既有语义无应答（兼容）
  B12 被拒正文确实没有进库（数据库复核）

用例相互隔离：某个用例把连接打死时会重连重登再继续；连不上即判定服务端进程已死，
后续用例如实记为未执行原因，不改写成通过。修复前的实测形态就是「第一条 2000 字节
消息即连接被重置」，因此这条容错路径是套件的一部分，不是可选装饰。

前置：
  1) MySQL 可达（读 Server/connect.config），且已执行
     Server/migrate_msgbnd_001_widen_msg.sql —— message_info.msg 需 varchar(2000)；
     未扩列时 2000 字节正文会在 INSERT 处失败，本套件按前置条件不满足直接退出（FATAL），
     不把环境问题写成业务失败。
  2) dist/Server.exe 为当前分支构建产物；TCP 5000 独占，不与其他套件并行运行。

本套件会：创建/复用账号 mbna、mbnb、mbnc；只删除这三者之间的 message_info 行；
不触碰其它账号的数据。测试进程只清理自己启动的服务端。

用法：
    py -3.13 -X utf8 project-docs/evidence/REQ-CLI-MSGBND-001/boundary_e2e.py
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
MAX_BODY = 2000                 # 与两端 protocol.h 的 CHAT_MAX_BODY_BYTES 同值

T_REG, T_REG_R, T_LOGIN, T_LOGIN_R, T_CAP, T_CAP_R = 1, 2, 3, 4, 44, 45
T_CHAT, T_CHAT_R, T_GETCHAT, T_GETCHAT_R = 15, 16, 17, 18
T_FLUSHFRIEND, T_FLUSHFRIEND_R = 11, 12

A, B, C = 'mbna', 'mbnb', 'mbnc'   # A 发送方；B 在线接收方；C 离线接收方

MSG_HDR = 38          # Msg.msg 的实际偏移：totalLen@0 + msgLen@4 + Time@8..37
MSG_STRIDE = 40       # sizeof(Msg)：4 字节对齐后的记录头

RESULTS = []
STATE = {'server_dead': False, 'note': ''}


def check(label, cond, detail=''):
    RESULTS.append((label, bool(cond)))
    print(('[PASS] ' if cond else '[FAIL] ') + label + (' :: ' + str(detail) if detail else ''))
    sys.stdout.flush()


def f32(s):
    return s.encode()[:31].ljust(32, b'\0')


def pdu(t, msg=b'', dat=b''):
    return struct.pack('<III', HDR + len(msg), len(msg), t) + dat[:64].ljust(64, b'\0') + msg


def chat_payload(body, time_text, declared_msg_len=None, declared_total=None, trailer=b'\0'):
    """构造私聊 caMsg：一条 Msg 记录。body 为不含结尾 NUL 的正文字节。

    declared_msg_len / declared_total 用于制造畸形报文。
    正常形态：msgLen = len(body)+1，totalLen = 40 + len(body) + 1。
    """
    ml = len(body) + 1 if declared_msg_len is None else declared_msg_len
    tl = MSG_STRIDE + len(body) + 1 if declared_total is None else declared_total
    header = struct.pack('<II', tl, ml)
    return header + time_text.encode()[:30].ljust(30, b'\0') + body + trailer


def unpack(buf):
    out = []
    while len(buf) >= HDR:
        tl, ml, ty = struct.unpack('<III', buf[:12])
        if tl < HDR or tl != HDR + ml or len(buf) < tl:
            break
        out.append((ty, buf[12:76], buf[76:tl]))
        buf = buf[tl:]
    return out, buf


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

    def collect(self, want_type, count=1, timeout=12):
        got, end = [], time.time() + timeout
        while len([g for g in got if g[0] == want_type]) < count and time.time() < end:
            try:
                self.pump(.2)
            except ConnectionError:
                break
            got.extend(self.packets())
        return got

    def drain(self, seconds=1.0):
        got, end = [], time.time() + seconds
        while time.time() < end:
            try:
                self.pump(min(.25, max(.05, end - time.time())))
            except ConnectionError:
                break
            got.extend(self.packets())
        return got

    def ask(self, pkt, want, timeout=10):
        self.send(pkt)
        return self.collect(want, 1, timeout)

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


def cap_code(c):
    r = c.ask(pdu(T_CAP), T_CAP_R)
    return r[0][2].split(b'\0')[0] if r else b''


def ensure_user(name):
    c = Conn(name)
    code = cap_code(c)
    if code:
        c.ask(pdu(T_REG, msg=code + b'\0',
                  dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()), T_REG_R)
    c.close()


def login(name):
    c = Conn(name)
    code = cap_code(c)
    if not code:
        raise RuntimeError('验证码获取失败，无法登录 ' + name)
    r = c.ask(pdu(T_LOGIN, msg=code + b'\0',
                  dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()), T_LOGIN_R)
    ret = struct.unpack('<i', r[0][1][:4])[0] if r else -99
    if ret != 1:
        c.close()
        raise RuntimeError('登录失败 ' + name + ' ret=' + str(ret))
    return c


def ack_fields(ps):
    """解析 16：caData[0,32)=对方名、[32)=bool 入库结果；caMsg=回带的 Msg 记录。"""
    if not ps:
        return None
    _t, dat, msg = ps[0]
    peer = dat[:32].split(b'\0')[0].decode('utf-8', 'replace')
    stored = bool(dat[32]) if len(dat) > 32 else None
    body, stamp = read_record(msg)
    return {'peer': peer, 'stored': stored, 'body': body, 'time': stamp}


def read_record(payload):
    """解析一条 Msg，返回 (正文字节, 时间串)。"""
    if len(payload) < MSG_HDR:
        return None, None
    _total, msg_len = struct.unpack_from('<II', payload, 0)
    stamp = payload[8:38].split(b'\0')[0].decode('utf-8', 'replace')
    end = MSG_HDR + max(0, msg_len - 1)
    return payload[MSG_HDR:end], stamp


def getchat(c, self_name, peer_name):
    """发 17 拉离线补发，返回 [(时间串, 正文字节)]；畸形步进即止。"""
    ps = c.ask(pdu(T_GETCHAT, dat=f32(self_name) + f32(peer_name)), T_GETCHAT_R)
    if not ps:
        return []
    payload = ps[0][2]
    out, off = [], 0
    while off + MSG_STRIDE <= len(payload):
        total, msg_len = struct.unpack_from('<II', payload, off)
        if total < MSG_STRIDE or msg_len < 1 or msg_len > total or off + total > len(payload):
            break
        stamp = payload[off + 8:off + 38].split(b'\0')[0].decode('utf-8', 'replace')
        body = payload[off + MSG_HDR:off + MSG_HDR + msg_len - 1]
        out.append((stamp, body))
        off += total
    return out


def db_conn():
    lines = (ROOT / 'Server' / 'connect.config').read_text(encoding='utf-8').splitlines()
    return pymysql.connect(host=lines[3], port=int(lines[4]), user=lines[6],
                           password=lines[7], database=lines[5], autocommit=True)


def column_width(cur):
    cur.execute('SELECT CHARACTER_MAXIMUM_LENGTH FROM information_schema.COLUMNS '
                'WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = "message_info" '
                'AND COLUMN_NAME = "msg"')
    r = cur.fetchone()
    return int(r[0]) if r else -1


def cleanup_domain():
    with db_conn() as c, c.cursor() as cur:
        cur.execute("DELETE FROM message_info WHERE seed_name IN (%s,%s,%s) "
                    "OR rev_name IN (%s,%s,%s)", ((A, B, C) * 2))


def db_rows_by_time(cur, stamp):
    cur.execute('SELECT CHAR_LENGTH(msg), LENGTH(msg), msg FROM message_info WHERE seed_time=%s',
                (stamp,))
    return cur.fetchall()


def fresh_stamp(tag):
    # seed_time 为 varchar(30)：固定前缀 + 序号，兼作数据库复核的标记
    return '2026-10-28 mbnd-%s' % tag


class Session:
    """维护 A/B 两条已登录连接；任一连接被打死时重连重登，服务端进程死亡则置旗。"""

    def __init__(self):
        self.ca = None
        self.cb = None

    def _connect(self, name):
        try:
            return login(name)
        except (ConnectionError, socket.timeout, OSError) as e:
            STATE['server_dead'] = True
            STATE['note'] = '连接/登录失败：' + repr(e)
            return None

    def ensure(self):
        if STATE['server_dead']:
            return False
        if self.ca is None:
            self.ca = self._connect(A)
        if self.cb is None:
            self.cb = self._connect(B)
        return bool(self.ca) and bool(self.cb) and not STATE['server_dead']

    def drop(self):
        for c in (self.ca, self.cb):
            if c:
                c.close()
        self.ca = self.cb = None


def probe_process_alive(timeout=8):
    """新连接能否拿到验证码应答：区分「这条连接被打死」与「服务端进程没了」。"""
    try:
        p = Conn('probe')
        ok = bool(p.ask(pdu(T_CAP), T_CAP_R, timeout))
        p.close()
        return ok
    except Exception:
        return False


def run_case(sess, label, fn):
    """逐例隔离：异常（含连接被重置）记为失败；进程若已死则后续用例如实标为未执行。"""
    if STATE['server_dead']:
        check(label, False, '前置用例已使服务端进程消失，本例未执行：' + STATE['note'])
        return
    if not sess.ensure():
        check(label, False, '无法建立已登录连接：' + STATE['note'])
        return
    try:
        ok, detail = fn(sess)
        check(label, ok, detail)
    except (ConnectionError, ConnectionResetError, BrokenPipeError, socket.timeout, OSError) as e:
        sess.drop()
        alive = probe_process_alive()
        if not alive:
            STATE['server_dead'] = True
            STATE['note'] = repr(e)
            check(label, False, '连接被重置且服务端进程已不可达：' + repr(e))
        else:
            check(label, False, '连接被重置/异常（未修复形态：超长正文打爆服务端）：' + repr(e))
    except Exception as e:                       # 其它异常不污染服务端状态判定
        check(label, False, '用例异常：' + repr(e))


def send_chat(sess, peer, body, stamp, **kw):
    sess.ca.send(pdu(T_CHAT, msg=chat_payload(body, stamp, **kw), dat=f32(A) + f32(peer)))


def b1(sess):
    s = fresh_stamp('01')
    send_chat(sess, B, b'a' * MAX_BODY, s)
    a1 = ack_fields(sess.ca.collect(T_CHAT_R, 1, 10))
    if not (a1 and a1['stored'] is True and a1['body'] == b'a' * MAX_BODY):
        return False, 'ack=' + str(a1 and {k: a1[k] for k in ('peer', 'stored')}) + \
               ' bodylen=' + str(len(a1['body']) if a1 and a1['body'] else -1)
    r1 = sess.cb.collect(T_CHAT, 1, 10)
    p1 = read_record(r1[0][2]) if r1 else (None, None)
    return (p1[0] == b'a' * MAX_BODY), 'pushlen=' + str(len(p1[0]) if p1[0] else -1)


def b2(sess):
    send_chat(sess, B, b'b' * (MAX_BODY + 1), fresh_stamp('02'))
    a2 = ack_fields(sess.ca.collect(T_CHAT_R, 1, 10))
    if not (a2 and a2['stored'] is False):
        return False, '16 的 stored=' + str(a2 and a2['stored']) + '（期望 False=拒绝入库）'
    pushed = sess.cb.drain(1.2)
    return all(p[0] != T_CHAT for p in pushed), '接收方收到类型=' + str([p[0] for p in pushed])


def _declared_overrun(sess):
    send_chat(sess, B, b'c' * 100, fresh_stamp('03'), declared_msg_len=4000)
    a3 = ack_fields(sess.ca.collect(T_CHAT_R, 1, 10))
    return bool(a3 and a3['stored'] is False), '16 的 stored=' + str(a3 and a3['stored'])


def b3(sess):
    ok, d = _declared_overrun(sess)
    if not ok:
        return ok, d
    probe = Conn('probe')          # 另开连接探活：进程仍在监听即未崩
    probe_alive = bool(probe.ask(pdu(T_CAP), T_CAP_R, 8))
    probe.close()
    return probe_alive, '探活连接' + ('可用' if probe_alive else '拿不到应答')


def b4(sess):
    send_chat(sess, B, b'', fresh_stamp('04'), declared_msg_len=0, trailer=b'')
    a = ack_fields(sess.ca.collect(T_CHAT_R, 1, 10))
    return bool(a and a['stored'] is False), '16 的 stored=' + str(a and a['stored'])


def b5(sess):
    send_chat(sess, B, b'', fresh_stamp('05'))      # msgLen=1，正文 0 字节
    a = ack_fields(sess.ca.collect(T_CHAT_R, 1, 10))
    return bool(a and a['stored'] is False), '16 的 stored=' + str(a and a['stored'])


def b6(sess):
    short = struct.pack('<II', 20, 1) + b'x' * 11   # uiMsgLen=20 < sizeof(Msg)
    sess.ca.send(pdu(T_CHAT, msg=short, dat=f32(A) + f32(B)))
    a = ack_fields(sess.ca.collect(T_CHAT_R, 1, 10))
    if not (a and a['stored'] is False):
        return False, '16 的 stored=' + str(a and a['stored'])
    probe = Conn('probe')
    ok = bool(probe.ask(pdu(T_CAP), T_CAP_R, 8))
    probe.close()
    return ok, '畸形报文后探活' + ('可用' if ok else '失败')


def b7(sess):
    cn = '中' * 666                                  # 666 * 3 = 1998 字节
    send_chat(sess, B, cn.encode(), fresh_stamp('07'))
    a = ack_fields(sess.ca.collect(T_CHAT_R, 1, 10))
    if not (a and a['stored'] is True and a['body'] == cn.encode('utf-8')):
        return False, 'ack bodylen=' + str(len(a['body']) if a and a['body'] else -1)
    p = sess.cb.collect(T_CHAT, 1, 10)
    got = read_record(p[0][2])[0] if p else None
    return got == cn.encode('utf-8'), '投递字节数=' + str(len(got) if got else -1)


def b8(sess):
    full_time = ('2026-10-28T00:00:0001234567' + 'x' * 30)[:30]   # 恰好填满 30 字节
    sess.ca.send(pdu(T_CHAT, msg=chat_payload(b'timeprobe', full_time), dat=f32(A) + f32(B)))
    a = ack_fields(sess.ca.collect(T_CHAT_R, 1, 10))
    return (a and a['time'] == full_time), \
           'got=' + repr(a and a['time']) + ' want=' + repr(full_time)


def b9(sess):
    send_chat(sess, C, b'd' * MAX_BODY, fresh_stamp('09'))
    a = ack_fields(sess.ca.collect(T_CHAT_R, 1, 10))
    if not (a and a['stored'] is True):
        return False, '离线目标入库结果=' + str(a and a['stored'])
    cc = login(C)
    try:
        pulled = getchat(cc, C, A)
        return any(x[1] == b'd' * MAX_BODY for x in pulled), \
               '补发长度=' + str([len(x[1]) for x in pulled])
    finally:
        cc.close()


def b10(sess):
    sess.cb.drain(0.6)        # 先清掉前面用例（如 B8 时间探针）留在接收方队列里的 15
    send_chat(sess, B, b'after-reject', fresh_stamp('10'))
    a = ack_fields(sess.ca.collect(T_CHAT_R, 1, 10))
    p = sess.cb.collect(T_CHAT, 1, 10)
    got = [read_record(x[2])[0] for x in p if x[0] == T_CHAT]
    ok = bool(a and a['stored'] is True) and (b'after-reject' in got)
    return ok, 'stored=' + str(a and a['stored']) + ' 收到正文=' + str([len(g or b'') for g in got])


def b11(sess):
    anon = Conn('anon')
    try:
        anon.send(pdu(T_CHAT, msg=chat_payload(b'e' * (MAX_BODY + 1), fresh_stamp('11')),
                      dat=f32(A) + f32(B)))
        quiet = anon.drain(1.5)
    finally:
        anon.close()
    return all(p[0] != T_CHAT_R for p in quiet), '未认证连接收到类型=' + str([p[0] for p in quiet])


def b12(sess):
    with db_conn() as dbc, dbc.cursor() as cur:
        bad = []
        for tag in ('02', '03', '04', '05'):
            rows = db_rows_by_time(cur, fresh_stamp(tag))
            if rows:
                bad.append((tag, len(rows)))
        if bad:
            return False, '被拒正文竟入库：' + str(bad)
        ok_rows = db_rows_by_time(cur, fresh_stamp('01'))
        return (len(ok_rows) == 1 and ok_rows[0][1] == MAX_BODY), \
               '2000 字节行的字节长度=' + str(ok_rows and ok_rows[0][1])


def main():
    if not EXE.exists():
        print('FATAL: 未找到 ' + str(EXE) + '，先构建 Release')
        return 2

    with db_conn() as probe, probe.cursor() as cur:
        w = column_width(cur)
        print('[pre] message_info.msg 列宽 =', w)
        if w < MAX_BODY:
            print('FATAL: 列宽不足 %d，需先执行 Server/migrate_msgbnd_001_widen_msg.sql' % MAX_BODY)
            return 2

    print('=== 环境准备 ===')
    srv = server_ctl.ServerCtl(str(EXE), str(DIST), HOST, PORT)
    if not srv.start():
        print('FATAL: Server 启动失败：' + str(srv.last_error))
        return 2

    ensure_user(A)
    ensure_user(B)
    ensure_user(C)
    cleanup_domain()

    sess = Session()
    try:
        for label, fn in (
            ('B1_2000_accepted_and_delivered', b1),
            ('B2_2001_rejected', b2),
            ('B3_declared_overrun_rejected', b3),
            ('B4_zero_msglen_rejected', b4),
            ('B5_empty_body_rejected', b5),
            ('B6_short_header_rejected', b6),
            ('B7_cjk_roundtrip', b7),
            ('B8_time30_no_spill', b8),
            ('B9_offline_redelivered_full', b9),
            ('B10_connection_survives_reject', b10),
            ('B12_no_db_row_for_rejects', b12),
            ('B11_unauthenticated_still_silent', b11),
        ):
            run_case(sess, label, fn)
        check('Z0_server_alive_after_all_cases', not STATE['server_dead'], STATE['note'])
    finally:
        sess.drop()
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
