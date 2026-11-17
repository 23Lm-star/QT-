# -*- coding: utf-8 -*-
"""REQ-PAGE-FETCH-002 定向探针：所有用户搜索 + 查询结果总量上限 + 好友真分页排序。

覆盖（对应 PRD「验收标准」可自动化项）：
  搜索一/二/三：关键词子串匹配、排除自己与好友、通配符字面转义、超限关键词、
                大小写不敏感、搜索分页并集=期望全集
  上限一/二   ：>1000 符合条件用户 total=1000/capped=1、前 1000 可翻达、
                offset>=1000 空页、limit=0 全量兼容路径同样封顶
  好友一/二/四：11/12 真分页（limit/offset/total/has_more）、名字字典序、
                UNION 去重、>200 不截断（truncated 恒 0）、limit=0 旧全量语义、
                limit>200 钳制、好友总量上限
  兼容        ：空 caData 旧端请求（7/11）功能可用

用法：
    py -3.13 -X utf8 project-docs/evidence/REQ-PAGE-FETCH-002/verify.py

前置：MySQL 可达（读 Server/connect.config）；dist/Server.exe 为当前分支产物；
      已执行 Server/migrate_friend_req_001.sql。夹具账号 p2a/p2b 与
      zzs*/zqt*/zz*spec/zzplain/ZZSUPPER/zc*/zf*/zg* 自建自清；TCP 5000 独占，串行。
"""
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
CAP = 1000          # LIST_TOTAL_CAP（与两端 protocol.h 一致）
PAGE_MAX = 200      # PAGE_MAX_LIMIT

T_REG, T_REG_R, T_LOGIN, T_LOGIN_R, T_CAP, T_CAP_R = 1, 2, 3, 4, 44, 45
T_ONLINEUSER, T_ONLINEUSER_R = 7, 8
T_FLUSHFRIEND, T_FLUSHFRIEND_R = 11, 12

A, B = 'p2a', 'p2b'
SEARCH_MATCH = ['zzs%02d' % i for i in range(1, 61)]      # 60 个含 zzs
NOISE = ['zqt%02d' % i for i in range(1, 11)]             # 10 个不含 zzs
SPEC = ['zz%spec', 'zz_spec', 'zzplain', 'ZZSUPPER']      # 通配符/大小写样本
CAP_USERS = ['zc%04d' % i for i in range(1, 1051)]        # 1050 个触发总量上限
FRIENDS = ['zf%03d' % i for i in range(1, 251)]           # 250 个好友（>200）
FRIENDS_CAP = ['zg%04d' % i for i in range(1, 801)]       # 再加 800 → 好友 1050
ALL_FIXTURES = [A, B] + SEARCH_MATCH + NOISE + SPEC + CAP_USERS + FRIENDS + FRIENDS_CAP
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
    r = c.ask(pdu(T_LOGIN, msg=code + b'\0', dat=f32(name) + PWD.encode()), T_LOGIN_R)
    rc = struct.unpack('<i', r[1][:4])[0] if r else -99
    if rc != 0:
        c.close()
        raise RuntimeError('登录失败 ' + name + ' code=' + str(rc))
    return c


def ensure_user(name):
    c = Conn()
    code = c.ask(pdu(T_CAP), T_CAP_R)[2].split(b'\0')[0]
    r = c.ask(pdu(T_REG, msg=code + b'\0', dat=f32(name) + PWD.encode()), T_REG_R)
    rc = struct.unpack('<i', r[1][:4])[0] if r else -99
    c.close()
    if rc not in (0, -2012):
        raise RuntimeError('注册失败 ' + name + ' code=' + str(rc))
    cap_login(name).close()


def like_escape(kw):
    return kw.replace('!', '!!').replace('%', '!%').replace('_', '!_')


def expected_users(keyword=''):
    """DB 期望全集：排除自己与双向好友；keyword 非空时子串匹配（! 转义，与 R1 同口径）。"""
    sql = ("SELECT name, online FROM user_info WHERE name != %s "
           "AND name NOT IN (SELECT friendName FROM friend WHERE name=%s "
           "UNION SELECT name FROM friend WHERE friendName=%s)")
    args = [A, A, A]
    if keyword:
        sql += " AND name LIKE %s ESCAPE '!'"
        args.append('%' + like_escape(keyword) + '%')
    sql += " ORDER BY name ASC"
    return [(r[0], bool(r[1])) for r in qall(sql, args)]


def expected_friends():
    rows = qall("SELECT friendName AS n FROM friend WHERE name=%s "
                "UNION SELECT name AS n FROM friend WHERE friendName=%s ORDER BY n ASC", (A, A))
    return [r[0] for r in rows]


def req7(limit, offset, keyword=''):
    dat = u32(limit) + u32(offset) + f32(keyword)
    return pdu(T_ONLINEUSER, dat=dat)


def parse8(r):
    ca, msg = r[1], r[2]
    code = struct.unpack('<i', ca[0:4])[0]
    total = struct.unpack('<I', ca[4:8])[0]
    has_more = ca[8]
    count = struct.unpack('<I', ca[12:16])[0]
    capped = ca[16]
    entries = []
    for i in range(min(count, len(msg) // 33)):
        base = i * 33
        name = msg[base:base + 32].split(b'\0')[0].decode('utf-8', 'replace')
        entries.append((name, msg[base + 32] == 1))
    return code, total, has_more, capped, entries


def req11(limit, offset):
    return pdu(T_FLUSHFRIEND, dat=u32(limit) + u32(offset))


def parse12(r):
    ca, msg = r[1], r[2]
    code = struct.unpack('<i', ca[0:4])[0]
    truncated = ca[4]
    count = struct.unpack('<I', ca[8:12])[0]
    total = struct.unpack('<I', ca[12:16])[0]
    has_more = ca[16]
    capped = ca[17]
    names = [msg[i * 32:i * 32 + 32].split(b'\0')[0].decode('utf-8', 'replace')
             for i in range(min(count, len(msg) // 32))]
    return code, truncated, count, total, has_more, capped, names


def seed_users(cur, names):
    cur.executemany("INSERT INTO user_info (name, pwd, salt, online) VALUES (%s,%s,%s,0)",
                    [(n, '0' * 32, 'seed' + n) for n in names])


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
        # ---------- 夹具 ----------
        ensure_user(A)
        ensure_user(B)
        with db_conn() as db, db.cursor() as cur:
            for n in ALL_FIXTURES:
                cur.execute("DELETE FROM friend WHERE name=%s OR friendName=%s", (n, n))
                cur.execute("DELETE FROM friend_request WHERE requester_name=%s OR target_name=%s", (n, n))
                cur.execute("DELETE FROM message_info WHERE seed_name=%s OR rev_name=%s", (n, n))
                cur.execute("DELETE FROM user_file WHERE owner_name=%s", (n,))
                if n not in (A, B):
                    cur.execute("DELETE FROM user_info WHERE name=%s", (n,))
            seed_users(cur, SEARCH_MATCH + NOISE + SPEC + CAP_USERS + FRIENDS + FRIENDS_CAP)
            # A-B 双向好友（搜索应排除）；A 与 250 个 zf 好友（奇数正向、偶数反向，验证 UNION 去重）
            cur.execute("INSERT INTO friend (name, friendName) VALUES (%s,%s)", (A, B))
            cur.execute("INSERT INTO friend (name, friendName) VALUES (%s,%s)", (B, A))
            cur.executemany("INSERT INTO friend (name, friendName) VALUES (%s,%s)",
                            [(A, n) if i % 2 else (n, A) for i, n in enumerate(FRIENDS)])
            cur.execute("UPDATE user_info SET online=1 WHERE name IN (%s,%s)", (A, B))
        ca = cap_login(A)

        # ---------- 搜索 ----------
        print('=== 搜索：所有用户关键词 ===')
        exp = expected_users('zzs')
        r = ca.ask(req7(50, 0, 'zzs'), T_ONLINEUSER_R)
        code, total, more, capped, entries = parse8(r)
        names1 = [n for n, _ in entries]
        check('S-1 标准码=0、首页 50 条、total=期望命中数、has_more=1、capped=0',
              code == 0 and len(entries) == 50 and total == len(exp) and more == 1 and capped == 0,
              (code, len(entries), total, len(exp), more, capped))
        check('S-2 首页名字升序且均属期望集', names1 == sorted(names1) and set(names1) <= {n for n, _ in exp})
        r = ca.ask(req7(50, 50, 'zzs'), T_ONLINEUSER_R)
        _, total2, more2, _, entries2 = parse8(r)
        union = names1 + [n for n, _ in entries2]
        check('S-3 次页收尽：并集=期望全集、has_more=0',
              union == [n for n, _ in exp] and more2 == 0, (len(union), len(exp), more2))
        check('S-4 大小写不敏感：ZZSUPPER 被 zzs 命中', 'ZZSUPPER' in union)

        r = ca.ask(req7(50, 0, 'zz%'), T_ONLINEUSER_R)
        _, totalw, _, _, ew = parse8(r)
        check('S-5 通配符 % 按字面：zz% 只命中 zz%spec',
              totalw == 1 and [n for n, _ in ew] == ['zz%spec'], (totalw, [n for n, _ in ew]))
        r = ca.ask(req7(50, 0, 'zz_'), T_ONLINEUSER_R)
        _, totalu, _, _, eu = parse8(r)
        check('S-6 通配符 _ 按字面：zz_ 只命中 zz_spec',
              totalu == 1 and [n for n, _ in eu] == ['zz_spec'], (totalu, [n for n, _ in eu]))

        r = ca.ask(req7(50, 0, 'z' * 32), T_ONLINEUSER_R)
        _, totalo, _, _, eo = parse8(r)
        check('S-7 超限关键词（32 字节）回空页不崩溃', totalo == 0 and len(eo) == 0, (totalo, len(eo)))
        r = ca.ask(req7(50, 0, 'nomatchxyz'), T_ONLINEUSER_R)
        _, totaln, moren, _, en = parse8(r)
        check('S-8 无命中关键词 total=0/has_more=0', totaln == 0 and moren == 0 and len(en) == 0)
        exp_p2 = expected_users('p2')
        r = ca.ask(req7(50, 0, 'p2'), T_ONLINEUSER_R)
        _, totalp, _, _, ep = parse8(r)
        namesp = [n for n, _ in ep]
        check('S-9 搜索排除自己与好友：p2 命中=DB 期望且不含 p2a/p2b',
              totalp == len(exp_p2) and A not in namesp and B not in namesp, (totalp, len(exp_p2)))
        r = ca.ask(req7(50, 0, 'zf'), T_ONLINEUSER_R)
        _, totalf, _, _, ef = parse8(r)
        check('S-10 搜索排除好友：zf（250 个好友）命中 0', totalf == 0 and len(ef) == 0, totalf)

        exp_all = expected_users()
        r = ca.ask(req7(50, 0, ''), T_ONLINEUSER_R)
        _, totala, _, _, ea = parse8(r)
        check('S-11 空关键词=不过滤：total=min(无搜索期望全集, 上限)',
              totala == min(len(exp_all), CAP), (totala, len(exp_all)))

        # ---------- 总量上限（所有用户） ----------
        print('=== 上限：所有用户 total 封顶 ===')
        exp_cap = expected_users()
        check('K-0 夹具符合条件总数 > 1000', len(exp_cap) > CAP, len(exp_cap))
        got, offset = [], 0
        last_more, last_capped, last_total = 1, 0, 0
        while True:
            r = ca.ask(req7(200, offset), T_ONLINEUSER_R)
            _, last_total, last_more, last_capped, e = parse8(r)
            got.extend(n for n, _ in e)
            if not last_more or not e:
                break
            offset += 200
        check('K-1 total=1000、capped=1', last_total == CAP and last_capped == 1, (last_total, last_capped))
        check('K-2 逐页并集=期望名字序前 1000、互异',
              got == [n for n, _ in exp_cap][:CAP] and len(set(got)) == CAP, (len(got), CAP))
        r = ca.ask(req7(200, CAP), T_ONLINEUSER_R)
        _, t_at, m_at, c_at, e_at = parse8(r)
        check('K-3 offset=1000 回空页、has_more=0、total 仍=1000',
              len(e_at) == 0 and m_at == 0 and t_at == CAP, (len(e_at), m_at, t_at))
        r = ca.ask(req7(200, CAP + 200), T_ONLINEUSER_R)
        _, _, m_over, _, e_over = parse8(r)
        check('K-4 offset>1000 回空页不越界', len(e_over) == 0 and m_over == 0)
        r = ca.ask(req7(0, 0), T_ONLINEUSER_R)          # limit=0 旧全量兼容路径
        _, t0, m0, c0, e0 = parse8(r)
        check('K-5 limit=0 全量同样封顶：count=1000、capped=1、has_more=0',
              len(e0) == CAP and t0 == CAP and c0 == 1 and m0 == 0, (len(e0), t0, c0, m0))
        r = ca.ask(pdu(T_ONLINEUSER), T_ONLINEUSER_R)   # 空 caData 旧端请求
        _, t_old, _, c_old, e_old = parse8(r)
        check('K-6 空 caData 旧端 7 请求功能可用（全量≤1000）', len(e_old) == CAP and t_old == CAP and c_old == 1,
              (len(e_old), t_old, c_old))
        r = ca.ask(req7(200, 0, 'zc'), T_ONLINEUSER_R)
        _, ts, _, cs, es = parse8(r)
        check('K-7 搜索路径同样封顶：zc 命中 1050 → total=1000、capped=1、本页 200',
              ts == CAP and cs == 1 and len(es) == 200, (ts, cs, len(es)))

        # ---------- 好友真分页 ----------
        print('=== 好友：真分页与排序 ===')
        expf = expected_friends()
        check('FR-0 夹具好友 251（250 个 zf＋好友 p2b；UNION 去重、双向混合）', len(expf) == 251, len(expf))
        r = ca.ask(req11(50, 0), T_FLUSHFRIEND_R)
        code, tr, cnt, total, more, capped, names = parse12(r)
        check('FR-1 首页 50 条、total=251、has_more=1、truncated=0、capped=0、标准码=0',
              code == 0 and cnt == 50 and total == 251 and more == 1 and tr == 0 and capped == 0,
              (code, cnt, total, more, tr, capped))
        check('FR-2 首页=期望名字序前 50', names == expf[:50], names[:3])
        gotf, off = list(names), 50
        while True:
            r = ca.ask(req11(50, off), T_FLUSHFRIEND_R)
            _, tr2, cnt2, _, more2, _, nf = parse12(r)
            gotf.extend(nf)
            if not more2 or not nf:
                break
            off += 50
        check('FR-3 逐页并集=期望全集 251、互异、末页 has_more=0',
              gotf == expf and len(set(gotf)) == 251 and more2 == 0, (len(gotf), more2))
        check('FR-4 全程 truncated=0（>200 不再截断）', tr == 0 and tr2 == 0)
        r = ca.ask(req11(0, 0), T_FLUSHFRIEND_R)
        _, tr0, cnt0, total0, more0, capped0, names0 = parse12(r)
        check('FR-5 limit=0 旧全量语义：251 条一次回齐、truncated=0、has_more=0',
              cnt0 == 251 and names0 == expf and tr0 == 0 and more0 == 0 and total0 == 251,
              (cnt0, tr0, more0, total0))
        r = ca.ask(pdu(T_FLUSHFRIEND), T_FLUSHFRIEND_R)   # 空 caData 旧端请求
        _, tr_old, cnt_old, _, _, _, names_old = parse12(r)
        check('FR-6 空 caData 旧端 11 请求=全量语义', cnt_old == 251 and names_old == expf and tr_old == 0,
              (cnt_old, tr_old))
        r = ca.ask(req11(300, 0), T_FLUSHFRIEND_R)
        _, _, cnt300, _, _, _, _ = parse12(r)
        check('FR-7 limit=300 钳制到 200', cnt300 == PAGE_MAX, cnt300)

        # ---------- 好友总量上限 ----------
        print('=== 上限：好友 total 封顶 ===')
        with db_conn() as db, db.cursor() as cur:
            cur.executemany("INSERT INTO friend (name, friendName) VALUES (%s,%s)",
                            [(A, n) for n in FRIENDS_CAP])
        expf2 = expected_friends()
        check('FC-0 好友总数 1051 > 1000', len(expf2) == 1051, len(expf2))
        r = ca.ask(req11(0, 0), T_FLUSHFRIEND_R)
        _, trc, cntc, totalc, morec, cappedc, namesc = parse12(r)
        check('FC-1 limit=0 好友全量封顶：count=1000、total=1000、capped=1、truncated=0',
              cntc == CAP and totalc == CAP and cappedc == 1 and trc == 0 and morec == 0,
              (cntc, totalc, cappedc, trc, morec))
        check('FC-2 封顶内容=期望名字序前 1000', namesc == expf2[:CAP])
        r = ca.ask(req11(200, 800), T_FLUSHFRIEND_R)
        _, _, cnt8, total8, more8, _, names8 = parse12(r)
        check('FC-3 末页 offset=800 回 200 条收尽（800+200=1000）',
              cnt8 == 200 and more8 == 0 and names8 == expf2[800:CAP], (cnt8, more8))
        r = ca.ask(req11(200, CAP), T_FLUSHFRIEND_R)
        _, _, cnt_at, _, more_at, _, names_at = parse12(r)
        check('FC-4 offset=1000 回空页', cnt_at == 0 and more_at == 0 and len(names_at) == 0)
        with db_conn() as db, db.cursor() as cur:
            cur.executemany("DELETE FROM friend WHERE name=%s AND friendName=%s",
                            [(A, n) for n in FRIENDS_CAP])

        # ---------- 汇总 ----------
        failed = [k for k, ok in RESULTS if not ok]
        print('\n==== 汇总：%d/%d 通过 ====' % (len(RESULTS) - len(failed), len(RESULTS)))
        for k in failed:
            print('  失败项：' + k)
        return 0 if not failed else 1
    finally:
        try:
            with db_conn() as db, db.cursor() as cur:
                for n in ALL_FIXTURES:
                    cur.execute("DELETE FROM friend WHERE name=%s OR friendName=%s", (n, n))
                    cur.execute("DELETE FROM friend_request WHERE requester_name=%s OR target_name=%s", (n, n))
                    cur.execute("DELETE FROM message_info WHERE seed_name=%s OR rev_name=%s", (n, n))
                    cur.execute("DELETE FROM user_file WHERE owner_name=%s", (n,))
                cur.executemany("DELETE FROM user_info WHERE name=%s", [(n,) for n in ALL_FIXTURES])
            import shutil
            shutil.rmtree(str(DIST / 'filesys' / A), ignore_errors=True)
            shutil.rmtree(str(DIST / 'filesys' / B), ignore_errors=True)
        except Exception as e:
            print('清理异常（不影响探针结论，但需人工核对残留）: %s' % e)
        srv.stop()


if __name__ == '__main__':
    sys.exit(main())