# -*- coding: utf-8 -*-
"""REQ-CLI-MSGSTORE-001 服务端私聊 16 应答 e2e（PRD 验收「应答九」「兼容十」）。

覆盖：
  M1 发送方收到 16，字段齐全（对方名、入库结果、原样回带的时间与正文）
  M2 接收方仍只收到 15，报文内容与改前一致（转发路径未被改动）
  M3 在线接收方的 17 不再返回该条（服务端实时投递已标记已送达），与 16 的入库口径一致
  M4 接收方离线时 16 仍回 ret=1（"发送成功"= 服务端已入库，与在线与否无关）
  M5 离线方登录后经 17 取到该条，且再取不重复（消费式补发，客户端据此不会二次落库）
  M6 未认证连接发 15 仍无应答（保持改前"无响应语义的投递型请求"拒绝分支）
  M7 收到 16 后同一连接后续请求正常（新增应答类型不破坏帧解析，等价于旧客户端的忽略行为）

未覆盖（如实说明）：
  「服务端入库失败 → 16 回 ret=0」这条分支需要让 seedchatMsg 失败（数据库故障注入），
  在本项目的隔离测试库里没有不侵入产品的制造手段；该分支由客户端代码路径保证
  （只有 stored=true 才落库），并记入变更记录的未验证项。

用法：
    py -3.13 -X utf8 project-docs/evidence/REQ-CLI-MSGSTORE-001/chat16_e2e.py

前置：MySQL 可达（读 Server/connect.config）；dist/Server.exe 为当前分支构建产物。
本套件会：创建/复用账号 ms16a、ms16b、ms16c；删除这三者之间的 message_info 行；
不触碰其它账号的数据。服务端端口 TCP 5000 独占，不与其他套件并行运行。
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

T_REG, T_REG_R, T_LOGIN, T_LOGIN_R, T_CAP, T_CAP_R = 1, 2, 3, 4, 44, 45
T_CHAT, T_CHAT_R, T_GETCHAT, T_GETCHAT_R = 15, 16, 17, 18
T_FLUSHFRIEND, T_FLUSHFRIEND_R = 11, 12

A, B, C = 'ms16a', 'ms16b', 'ms16c'     # A 发送方；B 在线接收方；C 离线接收方

# Msg 结构（两端 protocol.h 一致）：u32 totalLen + u32 msgLen + char Time[30] + char msg[]
# sizeof(Msg) 因 4 字节对齐为 40，柔性数组从偏移 38 起
MSG_HDR = 38
MSG_STRIDE = 40

RESULTS = []


def check(label, cond, detail=''):
    RESULTS.append((label, bool(cond)))
    print(('[PASS] ' if cond else '[FAIL] ') + label + (' :: ' + str(detail) if detail else ''))
    sys.stdout.flush()


def f32(s):
    return s.encode()[:31].ljust(32, b'\0')


def pdu(t, msg=b'', dat=b''):
    return struct.pack('<III', HDR + len(msg), len(msg), t) + dat[:64].ljust(64, b'\0') + msg


def chat_payload(content, time_text):
    """构造私聊报文的 caMsg：一个 Msg 记录（时间串 + 正文，正文含结尾 NUL）。"""
    body = content.encode() + b'\0'
    header = struct.pack('<II', MSG_STRIDE + len(body), len(body))
    return header + time_text.encode()[:29].ljust(30, b'\0') + body


def read_msg_record(payload):
    """解析一条 Msg，返回 (时间串, 正文)。"""
    if len(payload) < MSG_HDR:
        return None, None
    _total, msg_len = struct.unpack_from('<II', payload, 0)
    time_text = payload[8:38].split(b'\0')[0].decode('utf-8', 'replace')
    content = payload[MSG_HDR:MSG_HDR + max(0, msg_len - 1)].decode('utf-8', 'replace')
    return time_text, content


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


def getchat(c, self_name, peer_name):
    """发 17 拉取离线补发，返回 [(时间, 正文)]。响应 caMsg 是连续 Msg 记录。"""
    ps = c.ask(pdu(T_GETCHAT, dat=f32(self_name) + f32(peer_name)), T_GETCHAT_R)
    if not ps:
        return []
    payload = ps[0][2]
    out, off = [], 0
    while off + MSG_HDR <= len(payload):
        total, msg_len = struct.unpack_from('<II', payload, off)
        if total <= 0 or off + total > len(payload) + 4:
            break
        time_text = payload[off + 8:off + 38].split(b'\0')[0].decode('utf-8', 'replace')
        content = payload[off + MSG_HDR:off + MSG_HDR + max(0, msg_len - 1)].decode('utf-8', 'replace')
        out.append((time_text, content))
        off += total
    return out


def db_conn():
    lines = (ROOT / 'Server' / 'connect.config').read_text(encoding='utf-8').splitlines()
    return pymysql.connect(host=lines[3], port=int(lines[4]), user=lines[6],
                           password=lines[7], database=lines[5], autocommit=True)


def cleanup_domain():
    with db_conn() as c, c.cursor() as cur:
        cur.execute("DELETE FROM message_info WHERE seed_name IN (%s,%s,%s) "
                    "OR rev_name IN (%s,%s,%s)", ((A, B, C) * 2))


def main():
    if not EXE.exists():
        print('FATAL: 未找到 ' + str(EXE) + '，先构建 Release')
        return 2

    print('=== 环境准备 ===')
    srv = server_ctl.ServerCtl(str(EXE), str(DIST), HOST, PORT)
    if not srv.start():
        print('FATAL: Server 启动失败：' + str(srv.last_error))
        return 2

    # 注册要在服务端起来之后做；账号已存在时注册返回失败，直接复用（密码同为 123456）
    ensure_user(A)
    ensure_user(B)
    ensure_user(C)
    cleanup_domain()

    try:
        # ---- M1/M2/M3：双端在线 ----
        ca, cb = login(A), login(B)
        text = 'ms16-online-' + str(int(time.time() * 1000) % 1000000)
        stamp = time.strftime('%Y-%m-%d %H:%M:%S')
        ca.send(pdu(T_CHAT, msg=chat_payload(text, stamp), dat=f32(A) + f32(B)))

        ack = ca.collect(T_CHAT_R, 1, 10)
        check('M1_sender_gets_16', len(ack) == 1, 'packets=' + str([p[0] for p in ack]))
        if ack:
            t, dat, msg = ack[0]
            peer = dat[:32].split(b'\0')[0].decode()
            ret = bool(dat[32])
            a_time, a_text = read_msg_record(msg)
            check('M1b_16_fields_match',
                  peer == B and ret is True and a_text == text and a_time == stamp,
                  'peer=' + peer + ' ret=' + str(ret) + ' text=' + a_text + ' time=' + a_time)
        else:
            check('M1b_16_fields_match', False, '没有 16 可解析')

        recv = cb.collect(T_CHAT, 1, 10)
        check('M2_receiver_gets_15_only', len(recv) == 1 and recv[0][0] == T_CHAT,
              'types=' + str([p[0] for p in recv]))
        if recv:
            seed = recv[0][1][:32].split(b'\0')[0].decode()
            r_time, r_text = read_msg_record(recv[0][2])
            check('M2b_15_body_unchanged', seed == A and r_text == text and r_time == stamp,
                  'seed=' + seed + ' text=' + r_text)
        else:
            check('M2b_15_body_unchanged', False, '接收方没收到 15')

        # 在线投递已被服务端标记已送达：B 再拉 17 不该拿到同一条（否则客户端会二次落库）
        b_pulled = getchat(cb, B, A)
        check('M3_online_delivery_consumed', all(x[1] != text for x in b_pulled),
              'pulled=' + str(b_pulled))

        # ---- M7：16 之后同连接继续正常收发 ----
        ca.send(pdu(T_FLUSHFRIEND, dat=f32(A)))
        fl = ca.collect(T_FLUSHFRIEND_R, 1, 10)
        check('M7_connection_survives_new_respond', len(fl) == 1,
              'types=' + str([p[0] for p in fl]))

        text2 = 'ms16-after-ack'
        stamp2 = time.strftime('%Y-%m-%d %H:%M:%S')
        ca.send(pdu(T_CHAT, msg=chat_payload(text2, stamp2), dat=f32(A) + f32(B)))
        ack2 = ca.collect(T_CHAT_R, 1, 10)
        got2 = cb.collect(T_CHAT, 1, 10)
        check('M7b_second_round_still_works',
              len(ack2) == 1 and len(got2) == 1
              and bool(ack2[0][1][32]) and read_msg_record(got2[0][2])[1] == text2,
              'ack=' + str(len(ack2)) + ' push=' + str(len(got2)))

        # ---- M4/M5：接收方离线（C 全程未登录） ----
        ca.send(pdu(T_CHAT, msg=chat_payload('ms16-offline-target', stamp),
                    dat=f32(A) + f32(C)))
        ack3 = ca.collect(T_CHAT_R, 1, 10)
        offline_ok = (len(ack3) == 1 and bool(ack3[0][1][32])
                      and ack3[0][1][:32].split(b'\0')[0].decode() == C)
        check('M4_offline_send_acked_stored', offline_ok,
              'ack=' + str([(p[0], p[1][:33]) for p in ack3]))

        # A 断开前把 B 的连接留着；C 此时尚未登录
        cc = login(C)
        pulled = getchat(cc, C, A)
        check('M5_offline_redelivered_once',
              any(x[1] == 'ms16-offline-target' for x in pulled), 'pulled=' + str(pulled))
        again = getchat(cc, C, A)
        check('M5b_redelivery_is_consumed', all(x[1] != 'ms16-offline-target' for x in again),
              'second=' + str(again))

        # ---- M6：未认证连接发 15 不应得到 16 ----
        anon = Conn('anon')
        anon.send(pdu(T_CHAT, msg=chat_payload('ms16-anon', stamp), dat=f32(A) + f32(B)))
        quiet = anon.drain(1.5)
        check('M6_unauthenticated_no_16', all(p[0] != T_CHAT_R for p in quiet),
              'types=' + str([p[0] for p in quiet]))
        anon.close()

    finally:
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
