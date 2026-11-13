# -*- coding: utf-8 -*-
"""REQ-CHAT-ACK-001 专项探针：私聊接收确认 + 好友申请离线持久化。

覆盖：
  C1 实时投递走 109 且带消息 ID，服务端不再于写出时置已送达
  C2 客户端 110 确认后 issuccess 由 0 变 1
  C3 未确认的消息在断线重连后由 111/112 重投，仍保持未送达
  C4 用他人消息 ID 伪造 110 不生效
  C5 好友申请目标离线时仍登记 pending，请求方收到 ret=2
  C6 目标登录后 113/114 能取到该申请
  C7 115 拒绝后 pending 转 rejected，不再出现在 113 列表

用法：
    py -3.13 -X utf8 project-docs/evidence/REQ-CHAT-ACK-001/verify.py

前置：MySQL 可达（读 Server/connect.config）；dist/Server.exe 为当前分支产物。
本套件复用账号 ack1、ack2，并清理这两者的相关数据行；TCP 5000 独占，串行运行。
"""
import hashlib
import datetime
import os
import socket
import struct
import sys
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
MSG_HDR = 38

T_REG, T_REG_R, T_LOGIN, T_LOGIN_R, T_CAP, T_CAP_R = 1, 2, 3, 4, 44, 45
T_ADDFRIEND, T_ADDFRIEND_R, T_DELETEFRIEND = 9, 10, 13
T_CHAT, T_CHAT_R, T_GETCHAT, T_GETCHAT_R = 15, 16, 17, 18
T_DELIVER, T_ACK, T_PULL, T_PULL_R = 109, 110, 111, 112
T_LIST_APPLY, T_LIST_APPLY_R, T_REJECT_APPLY = 113, 114, 115
T_ACK_CAP = 116

A, B = 'ack1', 'ack2'
RESULTS = []


def check(label, cond, detail=''):
    RESULTS.append((label, bool(cond)))
    print(('[PASS] ' if cond else '[FAIL] ') + label + (' :: ' + str(detail) if detail else ''))
    sys.stdout.flush()


def f32(s):
    return s.encode()[:31].ljust(32, b'\0')


def pdu(t, msg=b'', dat=b''):
    return struct.pack('<III', HDR + len(msg), len(msg), t) + dat[:64].ljust(64, b'\0') + msg


def u32(v):
    return struct.pack('<I', v)


def u64(v):
    return struct.pack('<Q', v)


def db_conn():
    lines = (ROOT / 'Server' / 'connect.config').read_text(encoding='utf-8').splitlines()
    return pymysql.connect(host=lines[3] or '127.0.0.1', port=int(lines[4]), user=lines[6],
                           password=lines[7], database=lines[5], autocommit=True)


def q1(sql, args=()):
    with db_conn() as c, c.cursor() as cur:
        cur.execute(sql, args)
        return cur.fetchone()


def qexec(sql, args=()):
    with db_conn() as c, c.cursor() as cur:
        cur.execute(sql, args)


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
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=10)
        self.buf = b''

    def pump(self, timeout=.2):
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

    def ask(self, pkt, want, timeout=10):
        self.s.sendall(pkt)
        got, end = [], __import__('time').time() + timeout
        while __import__('time').time() < end:
            try:
                self.pump(.2)
            except ConnectionError:
                break
            got.extend(self.packets())
            if any(g[0] == want for g in got):
                break
        for g in got:
            if g[0] == want:
                return g
        return None

    def drain(self, seconds=.6):
        got, end = [], __import__('time').time() + seconds
        while __import__('time').time() < end:
            try:
                self.pump(.15)
            except ConnectionError:
                break
            got.extend(self.packets())
        return got

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


def cap_login(name):
    c = Conn()
    r = c.ask(pdu(T_CAP), T_CAP_R)
    if r is None:
        raise RuntimeError('验证码响应缺失')
    code = r[2].split(b'\0')[0]
    r = c.ask(pdu(T_LOGIN, msg=code + b'\0',
                  dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()), T_LOGIN_R)
    ret = struct.unpack('<i', r[1][:4])[0] if r else -99
    if ret != 1:
        c.close()
        raise RuntimeError('登录失败 ' + name + ' ret=' + str(ret))
    return c


def ensure_user(name):
    c = Conn()
    code = c.ask(pdu(T_CAP), T_CAP_R)[2].split(b'\0')[0]
    c.ask(pdu(T_REG, msg=code + b'\0',
              dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()), T_REG_R)
    c.close()


def now_text():
    return datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')


def chat_pdu(sender, target, body):
    b = body.encode()
    rec = struct.pack('<II', MSG_HDR + len(b) + 1, len(b) + 1) + now_text().encode().ljust(30, b'\0') + b + b'\0'
    return pdu(T_CHAT, msg=rec, dat=f32(sender) + f32(target))


def parse_pull(msg):
    """112：qint64 id + qint32 正文长 + qint32 时间长 + 正文 + 时间"""
    out, dis = [], 0
    while dis + 16 <= len(msg):
        mid, cl, tl = struct.unpack('<qii', msg[dis:dis + 16])
        dis += 16
        if cl < 0 or tl < 0 or dis + cl + tl > len(msg):
            break
        body = msg[dis:dis + cl].decode('utf-8', 'replace')
        dis += cl
        tm = msg[dis:dis + tl].decode('utf-8', 'replace')
        dis += tl
        out.append((mid, body, tm))
    return out


def main():
    if not EXE.exists():
        print('FATAL: 未找到 ' + str(EXE))
        return 2

    os.environ['CLOUDDISK_CAPTCHA_TEXT_FOR_TESTS'] = '1'

    # 清理上次残留
    for name in (A, B):
        qexec("delete from message_info where seed_name=%s or rev_name=%s", (name, name))
        qexec("delete from friend_request where requester_name=%s or target_name=%s", (name, name))
        qexec("delete from friend where name=%s or friendName=%s", (name, name))
        qexec("delete from user_info where name=%s", (name,))

    srv = server_ctl.ServerCtl(str(EXE), str(DIST), HOST, PORT)
    if not srv.start():
        print('FATAL: Server 启动失败：' + str(srv.last_error))
        return 2
    try:
        ensure_user(A)
        ensure_user(B)

        print('=== C0 未声明能力的老客户端走 15 旧语义 ===')
        ca, cb = cap_login(A), cap_login(B)
        ca.ask(chat_pdu(A, B, 'legacy-0'), T_CHAT_R)
        got = cb.drain(1.0)
        check('C0-1 未声明时收到 15', any(g[0] == T_CHAT for g in got) and not any(g[0] == T_DELIVER for g in got),
              [g[0] for g in got])
        row = q1("select issuccess from message_info where msg=%s", ('legacy-0',))
        check('C0-2 旧语义写出即已送达', row and row[0] == 1, row)
        # 本端声明支持 109/110 后，后续投递改走 ACK 模式
        cb.ask(pdu(T_ACK_CAP), 0, timeout=0.3)

        print('=== C1/C2 实时投递与确认 ===')
        ca.ask(chat_pdu(A, B, 'hello-1'), T_CHAT_R)
        got = cb.drain(1.0)
        deliver = [g for g in got if g[0] == T_DELIVER]
        check('C1-1 实时投递类型为 109', len(deliver) == 1, [g[0] for g in got])
        if deliver:
            mid = struct.unpack('<q', deliver[0][1][32:40])[0]
            row = q1("select issuccess from message_info where id=%s", (mid,))
            check('C1-2 写出后仍为未送达', row and row[0] == 0, row)
            cb.ask(pdu(T_ACK, msg=u64(mid)), 0, timeout=0.4)
            row = q1("select issuccess from message_info where id=%s", (mid,))
            check('C2 110 确认后置已送达', row and row[0] == 1, row)

        print('=== C3 未确认消息重投 ===')
        ca.ask(chat_pdu(A, B, 'hello-2'), T_CHAT_R)
        got = cb.drain(1.0)
        deliver = [g for g in got if g[0] == T_DELIVER]
        pending_id = struct.unpack('<q', deliver[0][1][32:40])[0] if deliver else -1
        check('C3-1 第二条已实时投递', pending_id > 0, pending_id)
        cb.close()   # 不确认直接断线
        cb = cap_login(B)
        cb.ask(pdu(T_ACK_CAP), 0, timeout=0.3)   # 重连后重新声明能力
        r = cb.ask(pdu(T_PULL, dat=f32(A)), T_PULL_R)
        rows = parse_pull(r[2]) if r else []
        check('C3-2 111 拉回该条', any(mid == pending_id for mid, _, _ in rows), rows)
        row = q1("select issuccess from message_info where id=%s", (pending_id,))
        check('C3-3 拉取后仍未送达', row and row[0] == 0, row)
        cb.ask(pdu(T_ACK, msg=u64(pending_id)), 0, timeout=0.4)
        row = q1("select issuccess from message_info where id=%s", (pending_id,))
        check('C3-4 确认后置已送达', row and row[0] == 1, row)

        print('=== C4 伪造确认 ===')
        ca.ask(chat_pdu(A, B, 'forge-target'), T_CHAT_R)
        got = cb.drain(1.0)
        deliver = [g for g in got if g[0] == T_DELIVER]
        victim = struct.unpack('<q', deliver[0][1][32:40])[0] if deliver else -1
        ca.ask(pdu(T_ACK, msg=u64(victim)), 0, timeout=0.4)   # A 冒充 B 确认
        row = q1("select issuccess from message_info where id=%s", (victim,))
        check('C4 他人确认无效', row and row[0] == 0, row)

        print('=== C5/C6/C7 好友申请离线持久化 ===')
        cb.close()   # B 离线
        r = ca.ask(pdu(T_ADDFRIEND, dat=f32(A) + f32(B)), T_ADDFRIEND_R)
        ret = struct.unpack('<i', r[1][:4])[0] if r else -99
        check('C5-1 离线申请返回 ret=2', ret == 2, ret)
        row = q1("select status from friend_request where requester_name=%s and target_name=%s", (A, B))
        check('C5-2 离线申请已登记 pending', row and row[0] == 'pending', row)
        cb = cap_login(B)
        r = cb.ask(pdu(T_LIST_APPLY), T_LIST_APPLY_R)
        names = []
        if r:
            names = [r[2][i:i + 32].split(b'\0')[0].decode() for i in range(0, len(r[2]), 32)]
        check('C6 登录后可取到待处理申请', A in names, names)
        cb.ask(pdu(T_REJECT_APPLY, dat=f32(A)), 0, timeout=0.4)
        row = q1("select status from friend_request where requester_name=%s and target_name=%s", (A, B))
        check('C7-1 拒绝后转 rejected', row and row[0] == 'rejected', row)
        r = cb.ask(pdu(T_LIST_APPLY), T_LIST_APPLY_R)
        names = []
        if r:
            names = [r[2][i:i + 32].split(b'\0')[0].decode() for i in range(0, len(r[2]), 32)]
        check('C7-2 拒绝后不再出现在列表', A not in names, names)

        ca.close()
        cb.close()
    finally:
        srv.stop()

    total = len(RESULTS)
    passed = sum(1 for _, ok in RESULTS if ok)
    print('RESULT %d/%d' % (passed, total))
    return 0 if passed == total else 1


if __name__ == '__main__':
    sys.exit(main())
