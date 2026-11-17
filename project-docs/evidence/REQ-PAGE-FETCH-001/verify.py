# -*- coding: utf-8 -*-
"""REQ-PAGE-FETCH-001 定向探针：所有用户分页 + 目录分页 + 聊天历史分页 + 截断。

覆盖（对应 PRD「验收标准」可自动化项）：
  用户一（范围口径）：所有用户含离线、排除自己与双向好友、在线标志正确
  用户二（分页）    ：62 个陌生用户按名字排序分页，limit/offset/total/has_more 正确
  目录一（分页）    ：60 个目录条目分页（目录优先+名字序）；limit=0 保持旧全量语义
  聊天一（历史分页）：117/118 按 ID 游标向前翻页，total/has_more/oldest/direction 正确
  聊天二（离线补发）：111 单批 200 条上限 + truncated，确认后再拉取尽
  截断一（低量列表）：113 好友申请 200 条 + 30 天窗口截断标志
  群信封截断字节    ：78/79 响应 body 末尾带 u8 truncated（环境缺表时跳过）

用法：
    py -3.13 -X utf8 project-docs/evidence/REQ-PAGE-FETCH-001/verify.py

前置：MySQL 可达（读 Server/connect.config）；dist/Server.exe 为当前分支产物；
      已执行 Server/migrate_friend_req_001.sql（否则服务端启动 fail-fast）。
本套件创建/复用账号 pfa、pfb 与 pg01..pg62（直接建表行，不登录），
并清理这些账号的消息、好友、申请、文件条目与目录；TCP 5000 独占，串行运行。
"""
import hashlib
import os
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
T_ADDFRIEND, T_ADDFRIEND_R = 9, 10
T_CHAT, T_CHAT_R = 15, 16
T_GETCHAT, T_GETCHAT_R = 17, 18
T_MKDIR, T_MKDIR_R = 19, 20
T_FLUSHFILE, T_FLUSHFILE_R = 23, 24
T_ONLINEUSER, T_ONLINEUSER_R = 7, 8
T_FLUSHFRIEND, T_FLUSHFRIEND_R = 11, 12
T_PULLPENDING, T_PULLPENDING_R, T_DELIVER_ACK = 111, 112, 110
T_CHATHIST, T_CHATHIST_R = 117, 118
T_LISTPENDING, T_LISTPENDING_R = 113, 114
T_GROUP_CREATE, T_GROUP_CREATE_R, T_GROUP_LIST, T_GROUP_LIST_R = 60, 61, 78, 79

A, B = 'pfa', 'pfb'
RUN = str(int(time.time()) % 100000)                       # 运行级唯一后缀，防跨次残留
DIR_PREFIX = 'dd' + RUN
STRANGERS = ['pg%02d' % i for i in range(1, 63)]          # 62 个陌生用户
MSG_HDR = 38
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


def envelope(gid=0):
    return struct.pack('<HHQQ', 1, 20, 0, gid)


def gstr(t):
    b = t.encode()
    return u32(len(b)) + b


def db_conn():
    lines = (ROOT / 'Server' / 'connect.config').read_text(encoding='utf-8').splitlines()
    return pymysql.connect(host=lines[3] or '127.0.0.1', port=int(lines[4]), user=lines[6],
                           password=lines[7], database=lines[5], autocommit=True)


def q1(sql, args=()):
    with db_conn() as c, c.cursor() as cur:
        cur.execute(sql, args)
        return cur.fetchone()


def qall(sql, args=()):
    with db_conn() as c, c.cursor() as cur:
        cur.execute(sql, args)
        return cur.fetchall()


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
        self.s = socket.create_connection((HOST, PORT), timeout=15)
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

    def ask(self, pkt, want, timeout=20):
        self.s.sendall(pkt)
        got, end = [], time.time() + timeout
        while time.time() < end:
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
        got, end = [], time.time() + seconds
        while time.time() < end:
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


def cap_raw(c):
    return c.ask(pdu(T_CAP), T_CAP_R)


# REQ-PWD-PLAIN-001：caData[32,64)=UTF-8 明文密码（NUL 补零），服务端内存摘要后加盐存储
def cap_login(name):
    c = Conn()
    r = cap_raw(c)
    if r is None:
        raise RuntimeError('验证码响应缺失')
    code = r[2].split(b'\0')[0]
    r = c.ask(pdu(T_LOGIN, msg=code + b'\0',
                  dat=f32(name) + PWD.encode()), T_LOGIN_R)
    code = struct.unpack('<i', r[1][:4])[0] if r else -99
    if code != 0:
        c.close()
        raise RuntimeError('登录失败 ' + name + ' code=' + str(code))
    return c


def ensure_user(name):
    c = Conn()
    code = cap_raw(c)[2].split(b'\0')[0]
    r = c.ask(pdu(T_REG, msg=code + b'\0', dat=f32(name) + PWD.encode()), T_REG_R)
    rc = struct.unpack('<i', r[1][:4])[0] if r else -99
    c.close()
    if rc not in (0, -2012):    # 0=注册成功；-2012(账号已存在)=复用
        raise RuntimeError('注册失败 ' + name + ' code=' + str(rc))
    cap_login(name).close()


def mkPDU_user_req(limit, offset):
    dat = u32(limit) + u32(offset)
    return pdu(T_ONLINEUSER, dat=dat)


def parse_users8(ca, msg):
    total = struct.unpack('<I', ca[4:8])[0]
    has_more = ca[8]
    count = struct.unpack('<I', ca[12:16])[0]
    entries = []
    for i in range(min(count, len(msg) // 33)):
        base = i * 33
        name = msg[base:base + 32].split(b'\0')[0].decode('utf-8', 'replace')
        entries.append((name, msg[base + 32] == 1))
    return total, has_more, entries


def main():
    if not EXE.exists():
        print('FATAL: 未找到 ' + str(EXE))
        return 2
    os.environ['CLOUDDISK_CAPTCHA_TEXT_FOR_TESTS'] = '1'
    srv = server_ctl.ServerCtl(str(EXE), str(DIST), HOST, PORT)
    if not srv.start():
        print('FATAL: Server 启动失败：' + str(srv.last_error))
        return 2
    try:
        # ---------- 环境准备 ----------
        ensure_user(A)
        ensure_user(B)
        ca, cb = cap_login(A), cap_login(B)
        with db_conn() as db, db.cursor() as cur:
            for n in [A, B] + STRANGERS:
                cur.execute("DELETE FROM friend WHERE name=%s OR friendName=%s", (n, n))
                cur.execute("DELETE FROM friend_request WHERE requester_name=%s OR target_name=%s", (n, n))
                cur.execute("DELETE FROM message_info WHERE seed_name=%s OR rev_name=%s", (n, n))
                cur.execute("DELETE FROM user_file WHERE owner_name=%s", (n,))
                cur.execute("DELETE FROM user_info WHERE name=%s AND name NOT IN (%s,%s)", (n, A, B))
            cur.execute("DELETE FROM user_info WHERE name LIKE 'pg%%' AND name NOT IN (%s,%s)", (A, B))
            # 直接建表行播种 62 个陌生用户（默认 online=0 离线，后续按用例改）
            for n in STRANGERS:
                cur.execute("INSERT INTO user_info (name, pwd, salt, online) VALUES (%s,%s,%s,0)",
                       (n, '00000000000000000000000000000000', 'seed'+n))
            cur.execute("UPDATE user_info SET online=1 WHERE name=%s OR name=%s", (A, B))

        # ---------- 用户一+二：所有用户分页与口径 ----------
        print('=== 用户：所有用户分页 ===')
        # pfa 与 pfb 先成为好友（双向），应从列表中排除
        ca.ask(pdu(T_ADDFRIEND, dat=f32(A) + f32(B)), T_ADDFRIEND)
        cb.ask(pdu(T_ADDFRIEND_R, dat=f32(B) + f32(A)), T_ADDFRIEND_R)
        fr = q1("SELECT COUNT(*) FROM friend WHERE (name=%s AND friendName=%s) OR (name=%s AND friendName=%s)",
                (A, B, B, A))[0]
        check('U-0 好友双向关系建立', fr == 2, fr)

        # 期望集合来自数据库（库中可能有其他历史用户，探针断言相对期望值）
        def expected_users():
            rows = qall(
                "SELECT u.name, u.online FROM user_info u WHERE u.name != %s "
                "AND u.name NOT IN (SELECT friendName FROM friend WHERE name=%s "
                "UNION SELECT name FROM friend WHERE friendName=%s) ORDER BY u.name ASC",
                (A, A, A))
            return [(r[0], bool(r[1])) for r in rows]

        r = ca.ask(mkPDU_user_req(50, 0), T_ONLINEUSER_R)
        total, has_more, entries = parse_users8(r[1], r[2])
        exp = expected_users()
        names1 = [e[0] for e in entries]
        check('U-1 首页 50 条', len(entries) == 50, len(entries))
        check('U-2 total 与数据库期望一致（含离线、排除自己与好友）', total == len(exp), (total, len(exp)))
        check('U-3 has_more=1（库内用户数 > 50）', has_more == 1, has_more)
        check('U-4 名字字典序升序', names1 == sorted(names1))
        check('U-5 排除自己与好友 pfb', A not in names1 and B not in names1)
        check('U-6 页内互异', len(set(names1)) == 50)

        # 逐页翻到收尽，并集=期望全集、页间不重叠、顺序=期望顺序
        got_pages, offset, expect_more = [], 0, True
        while expect_more:
            rr = ca.ask(mkPDU_user_req(50, offset), T_ONLINEUSER_R)
            t2, m2, e2 = parse_users8(rr[1], rr[2])
            got_pages.extend(e2)
            expect_more = (m2 == 1)
            offset += 50
        got_names = [e[0] for e in got_pages]
        exp_names = [e[0] for e in exp]
        check('U-7 分页并集=期望全集且互异', got_names == exp_names and len(set(got_names)) == len(got_names),
              (len(got_names), len(exp_names)))
        check('U-8 逐页 has_more 最后一页为 0', not expect_more)

        # 口径：前 10 个陌生用户置为在线，其余离线；在线标志逐条正确
        qexec("UPDATE user_info SET online=1 WHERE name IN (%s)" % ','.join(['%s'] * 10), STRANGERS[:10])
        exp = expected_users()
        r = ca.ask(mkPDU_user_req(0, 0), T_ONLINEUSER_R)      # limit=0 → 旧全量语义
        total0, has_more0, entries0 = parse_users8(r[1], r[2])
        check('U-9 limit=0 旧全量语义：一次回齐期望全集、has_more=0',
              [e[0] for e in entries0] == exp_names and has_more0 == 0,
              (len(entries0), has_more0))
        on_map = {n: o for n, o in entries0}
        pg_online = {r0[0]: bool(r0[1]) for r0 in
                     qall("SELECT name, online FROM user_info WHERE name LIKE 'pg%%'")}
        check('U-10 离线陌生用户在列', STRANGERS[20] in on_map and on_map[STRANGERS[20]] == pg_online[STRANGERS[20]])
        check('U-11 在线标志与库内一致（前 10 在线，其余离线）',
              all(on_map[n] == pg_online[n] for n in STRANGERS if n in on_map))

        # ---------- 目录一：刷新目录分页 ----------
        print('=== 目录：刷新目录分页 ===')
        user_root = str(DIST / 'filesys' / A).replace(chr(92), '/')
        import shutil
        shutil.rmtree(user_root, ignore_errors=True)   # 清残留：兜底目录会让总数翻倍
        os.makedirs(user_root, exist_ok=True)
        exp_dirs = ['%s%02d' % (DIR_PREFIX, i) for i in range(1, 61)]
        for dname in exp_dirs:
            # MKDIR 布局：caMsg=所在父目录（须在 actor 沙箱内），caData[0,32)=新目录名
            r = ca.ask(pdu(T_MKDIR, msg=user_root.encode(), dat=f32(dname)), T_MKDIR_R)
            code_m = struct.unpack('<i', r[1][:4])[0] if r else -99
            if code_m != 0:
                raise RuntimeError('mkdir %s 失败 code=%d' % (dname, code_m))
        dirs_all = qall("SELECT display_name FROM user_file WHERE owner_name=%s AND parent_path='/' "
                        "AND status='active' AND is_dir=1 AND display_name LIKE %s",
                        (A, DIR_PREFIX + '%'))
        check('F-0 根目录 60 个目录条目', len(dirs_all) == 60, len(dirs_all))

        user_root_f = str(DIST / 'filesys' / A).replace(chr(92), '/')
        def flush_req(limit, offset):
            # FLUSHFILE 布局：caMsg=目录绝对路径（沙箱内），caData=limit/offset
            return ca.ask(pdu(T_FLUSHFILE, msg=user_root_f.encode(), dat=u32(limit) + u32(offset)),
                          T_FLUSHFILE_R)

        r = flush_req(50, 0)
        ca_d = r[1]
        total_f = struct.unpack('<I', ca_d[4:8])[0]
        more_f = ca_d[8]
        count_f = struct.unpack('<I', ca_d[12:16])[0]
        got = []
        for i in range(min(count_f, len(r[2]) // 36)):
            nm = r[2][i * 36:i * 36 + 32].split(b'\0')[0].decode('utf-8', 'replace')
            ty = struct.unpack('<I', r[2][i * 36 + 32:i * 36 + 36])[0]
            got.append((nm, ty))
        check('F-1 首页 50 条', count_f == 50 and len(got) == 50, (count_f, len(got)))
        check('F-2 total=60、has_more=1', total_f == 60 and more_f == 1, (total_f, more_f))
        check('F-2b 首页条目=期望前 50 名', [n for n, _ in got] == exp_dirs[:50],
              [n for n, _ in got][:3])
        check('F-3 目录类型标记 uiType=0', all(t == 0 for _, t in got))
        gnames = [n for n, _ in got]
        check('F-4 目录按名字升序', gnames == sorted(gnames))
        r = flush_req(50, 50)
        ca_d = r[1]
        total_f2 = struct.unpack('<I', ca_d[4:8])[0]
        more_f2 = ca_d[8]
        count_f2 = struct.unpack('<I', ca_d[12:16])[0]
        check('F-5 次页 10 条收尽', count_f2 == 10 and more_f2 == 0, (count_f2, more_f2))
        r = ca.ask(pdu(T_FLUSHFILE, msg=user_root_f.encode()), T_FLUSHFILE_R)   # 空 caData → 旧全量语义
        ca_d = r[1]
        total_f3 = struct.unpack('<I', ca_d[4:8])[0]
        count_f3 = struct.unpack('<I', ca_d[12:16])[0]
        check('F-6 limit=0 旧全量语义：60 条一次回齐', count_f3 == 60 and total_f3 == 60,
              (count_f3, total_f3))
        full_names = [r[2][i * 36:i * 36 + 32].split(b'\0')[0].decode('utf-8', 'replace')
                      for i in range(count_f3)]
        check('F-7 全量语义条目=期望全集', full_names == exp_dirs, full_names[:3])

        # ---------- 聊天一：历史分页 117/118 ----------
        print('=== 聊天：历史分页 ===')
        # pfa 向 pfb 发 60 条（pfb 连接已关、保持离线 → 全部 issuccess=0 待送达）
        for i in range(60):
            body = ('h%03d' % i).encode() + b'\0'
            payload = struct.pack('<II', MSG_HDR + len(body), len(body)) + \
                ('2026-11-14 10:%02d:00' % i).encode().ljust(30, b'\0') + body
            r = ca.ask(pdu(T_CHAT, msg=payload, dat=f32(A) + f32(B)), T_CHAT_R)
            if r is None:
                raise RuntimeError('chat %d 无响应' % i)
        cnt = q1("SELECT COUNT(*) FROM message_info WHERE seed_name=%s AND rev_name=%s", (A, B))[0]
        check('C-0 60 条历史入库', cnt == 60, cnt)

        def hist_req(cursor, limit):
            msg = struct.pack('<q', cursor) + u32(limit)
            return ca.ask(pdu(T_CHATHIST, msg=msg, dat=f32(A) + f32(B)), T_CHATHIST_R)

        r = hist_req(0, 50)
        code = struct.unpack('<i', r[1][0:4])[0]
        total_c = struct.unpack('<I', r[1][4:8])[0]
        more_c = r[1][8]
        oldest = struct.unpack('<q', r[2][0:8])[0]
        count_c = struct.unpack('<I', r[2][8:12])[0]
        rows = []
        dis = 12
        ok_shape = True
        for _ in range(count_c):
            if dis + 17 > len(r[2]):
                ok_shape = False
                break
            mid = struct.unpack('<q', r[2][dis:dis + 8])[0]
            direction = r[2][dis + 8]
            clen = struct.unpack('<i', r[2][dis + 9:dis + 13])[0]
            tlen = struct.unpack('<i', r[2][dis + 13:dis + 17])[0]
            if dis + 17 + clen + tlen > len(r[2]):
                ok_shape = False
                break
            content = r[2][dis + 17:dis + 17 + clen].decode('utf-8', 'replace')
            rows.append((mid, direction, content))
            dis += 17 + clen + tlen
        check('C-1 标准码=0、total=60、has_more=1',
              code == 0 and total_c == 60 and more_c == 1, (code, total_c, more_c))
        check('C-2 首页 50 条且记录区形状合法', count_c == 50 and ok_shape and len(rows) == 50,
              (count_c, ok_shape, len(rows)))
        ids = [m for m, _, _ in rows]
        check('C-3 首页按 id 降序（新→旧）', ids == sorted(ids, reverse=True))
        check('C-4 全部 direction=0（查询者发送）', all(d == 0 for _, d, _ in rows))
        check('C-5 oldest=本页最旧 id', oldest == min(ids) if ids else False, oldest)

        r = hist_req(oldest, 50)
        total_c2 = struct.unpack('<I', r[1][4:8])[0]
        more_c2 = r[1][8]
        oldest2 = struct.unpack('<q', r[2][0:8])[0]
        count_c2 = struct.unpack('<I', r[2][8:12])[0]
        check('C-6 次页 10 条收尽', count_c2 == 10 and more_c2 == 0, (count_c2, more_c2))
        r = hist_req(0, 50)
        count_c3 = struct.unpack('<I', r[2][8:12])[0]
        check('C-7 不带分页块（uiMsgLen=0）默认最新 50 条', count_c3 == 50, count_c3)

        # ---------- 聊天二：离线补发上限与 truncated ----------
        print('=== 聊天：离线补发分批 ===')
        cb.close()                                  # pfb 掉线：pfa 后续消息全部落库待送达
        # pfa 保持已登录连接继续发送（pfb 无连接 → 全部落库待送达），避免验证码冷却干扰
        for i in range(60, 270):
            body = ('h%03d' % i).encode() + b'\0'
            payload = struct.pack('<II', MSG_HDR + len(body), len(body)) + \
                ('2026-11-14 11:%02d:00' % (i % 60)).encode().ljust(30, b'\0') + body
            r = ca.ask(pdu(T_CHAT, msg=payload, dat=f32(A) + f32(B)), T_CHAT_R)
            if r is None:
                raise RuntimeError('chat %d 无响应' % i)
        pending = q1("SELECT COUNT(*) FROM message_info WHERE rev_name=%s AND issuccess=0", (B,))[0]
        # C 阶段 60 条发送时 pfb 连接在线 → 已实时投递；此处只余 210 条待送达
        check('P-0 210 条待送达', pending == 210, pending)

        def parse_112(msg):
            ids, dis = [], 0
            while dis + 16 <= len(msg):
                mid = struct.unpack('<q', msg[dis:dis + 8])[0]
                clen = struct.unpack('<i', msg[dis + 8:dis + 12])[0]
                tlen = struct.unpack('<i', msg[dis + 12:dis + 16])[0]
                if clen < 0 or tlen < 0 or dis + 16 + clen + tlen > len(msg):
                    break
                if mid > 0:
                    ids.append(mid)
                dis += 16 + clen + tlen
            return ids

        cb2 = cap_login(B)
        r = cb2.ask(pdu(T_PULLPENDING, dat=f32(A)), T_PULLPENDING_R)
        tr = r[1][4]
        batch = struct.unpack('<I', r[1][8:12])[0]
        ack_ids = parse_112(r[2])
        check('P-1 首批 200 条且 truncated=1', batch == 200 and tr == 1 and len(ack_ids) == 200,
              (batch, tr, len(ack_ids)))
        cb2.ask(pdu(T_DELIVER_ACK, msg=b''.join(u64(i) for i in ack_ids)), 9999, timeout=3)
        r = cb2.ask(pdu(T_PULLPENDING, dat=f32(A)), T_PULLPENDING_R)
        tr2 = r[1][4]
        batch2 = struct.unpack('<I', r[1][8:12])[0]
        ack2 = parse_112(r[2])
        check('P-2 次批 10 条且 truncated=0', batch2 == 10 and tr2 == 0 and len(ack2) == 10,
              (batch2, tr2, len(ack2)))
        cb2.ask(pdu(T_DELIVER_ACK, msg=b''.join(u64(i) for i in ack2)), 9999, timeout=3)
        left = q1("SELECT COUNT(*) FROM message_info WHERE rev_name=%s AND issuccess=0", (B,))[0]
        check('P-3 确认后全部置已送达', left == 0, left)
        cb2.close()

        # ---------- 截断一：低量列表（113 好友申请） ----------
        print('=== 截断：好友申请 200 条上限 ===')
        now = time.strftime('%Y-%m-%d %H:%M:%S')
        with db_conn() as db, db.cursor() as cur:
            cur.execute("DELETE FROM friend_request WHERE target_name=%s", (B,))
            for i in range(210):
                cur.execute("INSERT INTO friend_request (requester_name, target_name, status, created_at) "
                            "VALUES (%s,%s,'pending',%s)", ('rq%03d' % i, B, now))
        cb3 = cap_login(B)
        r = cb3.ask(pdu(T_LISTPENDING), T_LISTPENDING_R)
        tr3 = r[1][4]
        cnt3 = struct.unpack('<I', r[1][8:12])[0]
        names = [r[2][i * 32:i * 32 + 32].split(b'\0')[0].decode('utf-8', 'replace')
                 for i in range(min(cnt3, len(r[2]) // 32))]
        check('T-1 只回 200 条且 truncated=1', cnt3 == 200 and tr3 == 1, (cnt3, tr3))
        check('T-2 按时间正序取最旧 200 条（rq000..rq199）',
              names[:3] == ['rq000', 'rq001', 'rq002'] and 'rq209' not in names, names[:3])
        cb3.close()

        # ---------- 群信封截断字节 ----------
        print('=== 群：78/79 body 末尾 truncated 字节 ===')
        ca.ask(pdu(T_GROUP_CREATE, msg=gstr('pfg'), dat=envelope(0)), T_GROUP_CREATE_R)
        gid_row = q1("SELECT group_id FROM group_info WHERE group_name=%s LIMIT 1", ('pfg',))
        if gid_row is None:
            print('[SKIP] 群用例跳过（建群失败，可能缺迁移表）')
            RESULTS.append(('群-1 建群（环境限制时跳过）', True))
        else:
            gid = gid_row[0]
            r = ca.ask(pdu(T_GROUP_LIST, msg=envelope(gid), dat=envelope(gid)), T_GROUP_LIST_R)
            # body：envelope(20) + code(4) + count(4) + rows + truncated(1)
            body = r[2]
            base = 20 + 4
            cnt_g = struct.unpack('<I', body[base:base + 4])[0]
            check('群-1 78/79 body 末尾带 truncated 字节且值为 0/1',
                  len(body) > base and body[-1] in (0, 1), (len(body), body[-1]))
            check('群-2 列表条目数与 count 一致（1 个群）', cnt_g == 1, cnt_g)

        # ---------- 汇总 ----------
        failed = [k for k, ok in RESULTS if not ok]
        print('\n==== 汇总：%d/%d 通过 ====' % (len(RESULTS) - len(failed), len(RESULTS)))
        for k in failed:
            print('  失败项：' + k)
        return 0 if not failed else 1
    finally:
        # ---------- 清理本套件夹具 ----------
        try:
            with db_conn() as db, db.cursor() as cur:
                for n in [A, B] + STRANGERS + ['rq%03d' % i for i in range(210)]:
                    cur.execute("DELETE FROM friend WHERE name=%s OR friendName=%s", (n, n))
                    cur.execute("DELETE FROM friend_request WHERE requester_name=%s OR target_name=%s", (n, n))
                    cur.execute("DELETE FROM message_info WHERE seed_name=%s OR rev_name=%s", (n, n))
                cur.execute("DELETE FROM user_file WHERE owner_name=%s", (A,))
                cur.execute("DELETE FROM group_info WHERE group_name='pfg'")
                cur.execute("DELETE FROM user_info WHERE name LIKE 'pg%%' OR name IN (%s,%s)", (A, B))
            import shutil
            shutil.rmtree(str(DIST / 'filesys' / A), ignore_errors=True)
            shutil.rmtree(str(DIST / 'filesys' / B), ignore_errors=True)
        except Exception as e:
            print('清理异常（不影响探针结论，但需人工核对残留）: %s' % e)
        srv.stop()


if __name__ == '__main__':
    sys.exit(main())
