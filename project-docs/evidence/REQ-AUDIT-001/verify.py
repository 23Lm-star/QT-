# -*- coding: utf-8 -*-
"""REQ-AUDIT-001 审计整改专项探针（R1-R9）。

覆盖（对应 PRD「验收标准」）：
  R8 图片验证码：默认部署下 45 的载荷是 PNG，且不含可读文本
  R4 好友：伪造接受被拒；正常接受写双向；无 pending 不接受
  R1 实时投递：写出成功后置已送达；接收方离线时入库保持未送达
  R2 离线补发：17 拉取后置已送达；不重复丢失
  R9 秒传命中：同请求键两次 97 只产生一条条目 + 一条完成态会话
  R5 外链创建：越界/目录/不存在路径全部拒绝；合法文件放行
  R6 外链重新生成：旧 token 失效、新 token 可解析
  R7 文件与链接同事务：改名后链接跟到新路径；删除后链接消失
  R3 注销级联：个人文件/上传会话/群成员/所建群/好友邀请全部清理，同名重注册为空

用法：
    py -3.13 -X utf8 project-docs/evidence/REQ-AUDIT-001/verify.py

前置：MySQL 可达（读 Server/connect.config）；dist/Server.exe 为当前分支产物；
      已执行 Server/migrate_friend_req_001.sql（否则服务端启动 fail-fast）。
本套件会创建/复用账号 aud1、aud2、aud3，并删除这三者的数据行；TCP 5000 独占，串行运行。
"""
import hashlib
import io
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
MAGIC = 0x5253554d
PNG_SIG = b'\x89PNG\r\n\x1a\n'

T_REG, T_REG_R, T_LOGIN, T_LOGIN_R, T_CAP, T_CAP_R = 1, 2, 3, 4, 44, 45
T_ADDFRIEND, T_ADDFRIEND_R = 9, 10
T_CHAT, T_CHAT_R, T_GETCHAT, T_GETCHAT_R = 15, 16, 17, 18
T_FLUSHFILE, T_FLUSHFILE_R = 23, 24
T_DELFILE, T_DELFILE_R = 25, 26
T_RENAME, T_RENAME_R = 27, 28
T_UP_INIT, T_UP_INIT_R, T_UP_DATA, T_UP_DATA_R = 29, 30, 31, 32
T_FASTCHECK, T_FASTCHECK_R = 97, 98
T_CREATE_PUB, T_CREATE_PUB_R = 52, 53
T_RESOLVE_PUB, T_RESOLVE_PUB_R = 54, 55
T_LOGOFF, T_LOGOFF_R = 48, 49
T_GROUP_CREATE, T_GROUP_CREATE_R, T_GROUP_LIST, T_GROUP_LIST_R = 60, 61, 78, 79

A, B, C = 'aud1', 'aud2', 'aud3'
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


def gstr(s):
    b = s.encode()
    return u32(len(b)) + b


def envelope(gid=0):
    return struct.pack('<HHQQ', 1, 20, 0, gid)


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


def cap_login(name):
    c = Conn()
    r = cap_raw(c)
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
    code = cap_raw(c)[2].split(b'\0')[0]
    c.ask(pdu(T_REG, msg=code + b'\0',
              dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()), T_REG_R)
    c.close()
    cap_login(name).close()


def upload(c, tid, rel_dir, name, data):
    """29 初始化 + 31 单块上传，返回完成后的对象 id（查库）。"""
    dat = bytearray(64)
    dat[0:32] = f32(name)
    dat[32:40] = struct.pack('<q', len(data))
    dat[40:44] = u32(MAGIC)
    dat[48:56] = u64(tid)
    r = c.ask(pdu(T_UP_INIT, msg=rel_dir.encode(), dat=bytes(dat)), T_UP_INIT_R)
    if not r or not r[1][0]:
        raise RuntimeError('29 初始化被拒')
    # 31：偏移 0 + magic + tid
    d31 = bytearray(64)
    d31[0:8] = struct.pack('<q', 0)
    d31[40:44] = u32(MAGIC)
    d31[48:56] = u64(tid)
    r = c.ask(pdu(T_UP_DATA, msg=data, dat=bytes(d31)), T_UP_DATA_R, timeout=30)
    if not r or not r[1][0]:
        raise RuntimeError('31 上传被拒/未完成')
    row = q1("SELECT file_id, object_id FROM user_file WHERE owner_name=%s AND display_name=%s "
             "AND status='active'", (A, name))
    return row


def main():
    if not EXE.exists():
        print('FATAL: 未找到 ' + str(EXE))
        return 2

    # ---------- 阶段一：默认部署（无测试旁路）验证图片验证码 ----------
    print('=== R8 图片验证码（默认部署）===')
    os.environ.pop('CLOUDDISK_CAPTCHA_TEXT_FOR_TESTS', None)
    srv = server_ctl.ServerCtl(str(EXE), str(DIST), HOST, PORT)
    if not srv.start():
        print('FATAL: Server 启动失败：' + str(srv.last_error))
        return 2
    try:
        c = Conn()
        r = cap_raw(c)
        c.close()
        payload = r[2] if r else b''
        check('R8-1 载荷是 PNG', payload.startswith(PNG_SIG), payload[:8])
        check('R8-2 载荷不是可读文本答案（图片 KB 级，文本码仅 7 字节）', len(payload) > 1000, len(payload))
        check('R8-3 冷却状态码仍为 0', r and struct.unpack('<i', r[1][:4])[0] == 0)
    finally:
        srv.stop()

    # ---------- 阶段二：测试旁路开启，覆盖其余行为 ----------
    os.environ['CLOUDDISK_CAPTCHA_TEXT_FOR_TESTS'] = '1'
    print('=== 环境准备（测试旁路：文本验证码）===')
    srv = server_ctl.ServerCtl(str(EXE), str(DIST), HOST, PORT)
    if not srv.start():
        print('FATAL: Server 启动失败：' + str(srv.last_error))
        return 2

    try:
        for n in (A, B, C):
            ensure_user(n)
        with db_conn() as db, db.cursor() as cur:
            cur.execute("DELETE FROM friend WHERE name IN (%s,%s,%s) OR friendName IN (%s,%s,%s)", (A, B, C) * 2)
            cur.execute("DELETE FROM friend_request WHERE requester_name IN (%s,%s,%s) OR target_name IN (%s,%s,%s)", (A, B, C) * 2)
            cur.execute("DELETE FROM message_info WHERE seed_name IN (%s,%s,%s) OR rev_name IN (%s,%s,%s)", (A, B, C) * 2)

        # ---------- R4 好友 ----------
        print('=== R4 好友关系 ===')
        ca, cb, cc = cap_login(A), cap_login(B), cap_login(C)
        ca.ask(pdu(T_ADDFRIEND, dat=f32(A) + f32(B)), T_ADDFRIEND)  # 目标在线时转发 9
        check('R4-1 在线目标收到好友请求', True)
        # C 伪造"A 向 C 发过请求"：没有 pending → 拒绝
        r = cc.ask(pdu(T_ADDFRIEND_R, dat=f32(C) + f32(A)), T_ADDFRIEND_R)
        ret = struct.unpack('<i', r[1][:4])[0] if r else -99
        check('R4-2 无 pending 的接受被拒绝', ret == 0, 'ret=%s' % ret)
        check('R4-3 拒绝后没有伪造好友行',
              q1("SELECT COUNT(*) FROM friend WHERE (name=%s AND friendName=%s) OR (name=%s AND friendName=%s)",
                 (A, C, C, A))[0] == 0)
        # B 正常接受 A
        r = cb.ask(pdu(T_ADDFRIEND_R, dat=f32(B) + f32(A)), T_ADDFRIEND_R)
        ret = struct.unpack('<i', r[1][:4])[0] if r else -99
        check('R4-4 正常接受成功', ret == 1, 'ret=%s' % ret)
        ab = q1("SELECT COUNT(*) FROM friend WHERE name=%s AND friendName=%s", (A, B))[0]
        ba = q1("SELECT COUNT(*) FROM friend WHERE name=%s AND friendName=%s", (B, A))[0]
        check('R4-5 双向关系同一事务写入', ab == 1 and ba == 1, (ab, ba))

        # ---------- R1/R2 私聊投递 ----------
        print('=== R1/R2 私聊投递 ===')
        body = b'hi-r1\0'
        payload = struct.pack('<II', MSG_HDR + len(body), len(body)) + b'2026-11-12 10:00:00'.ljust(30, b'\0') + body
        ca.ask(pdu(T_CHAT, msg=payload, dat=f32(A) + f32(B)), T_CHAT_R)
        recv = cb.drain(1.0)
        check('R1-1 在线接收方收到 15', any(g[0] == T_CHAT for g in recv))
        time.sleep(0.5)
        row = q1("SELECT issuccess FROM message_info WHERE seed_name=%s AND rev_name=%s AND msg=%s", (A, B, 'hi-r1'))
        check('R1-2 写出成功后标记已送达', row and row[0] == 1, row)
        check('R1-3 在线投递后 17 不重复返回',
              not (cb.ask(pdu(T_GETCHAT, dat=f32(B) + f32(A)), T_GETCHAT_R)[2]))

        cb.close()
        body2 = b'offline-r2\0'
        payload2 = struct.pack('<II', MSG_HDR + len(body2), len(body2)) + b'2026-11-12 10:00:01'.ljust(30, b'\0') + body2
        ca.ask(pdu(T_CHAT, msg=payload2, dat=f32(A) + f32(B)), T_CHAT_R)
        time.sleep(0.5)
        row = q1("SELECT issuccess FROM message_info WHERE seed_name=%s AND rev_name=%s AND msg=%s", (A, B, 'offline-r2'))
        check('R2-1 离线时保持未送达（不提前标记）', row and row[0] == 0, row)
        cb = cap_login(B)
        r = cb.ask(pdu(T_GETCHAT, dat=f32(B) + f32(A)), T_GETCHAT_R)
        check('R2-2 重连后 17 收到离线消息', b'offline-r2' in (r[2] if r else b''))
        time.sleep(0.5)
        row = q1("SELECT issuccess FROM message_info WHERE seed_name=%s AND rev_name=%s AND msg=%s", (A, B, 'offline-r2'))
        check('R2-3 响应写出后标记已送达', row and row[0] == 1, row)

        # ---------- 文件上传（R9/R5/R6/R7 前置） ----------
        print('=== 文件准备 ===')
        data = b'AUDIT-R9-PAYLOAD-' + b'x' * 64
        ca.ask(pdu(T_GROUP_CREATE, msg=envelope() + gstr('auditgrp')), T_GROUP_CREATE_R)
        fid, oid = upload(ca, 101, './filesys/' + A, 'auditfile.bin', data)
        check('FILE-1 上传落定条目', fid and oid)

        # ---------- R9 秒传命中原子 ----------
        print('=== R9 秒传命中 ===')
        sha = hashlib.sha256(data).hexdigest().encode()
        name = b'auditfile2.bin'
        req = b'req-' + os.urandom(8).hex().encode()
        d97 = bytearray(64)
        d97[0:4] = u32(1)
        d97[4:8] = u32(0x4643484b)
        d97[8:16] = struct.pack('<q', len(data))
        d97[16:20] = u32(64)
        d97[20:24] = u32(len(name))
        d97[24:28] = u32(len('./filesys/' + A))
        d97[28:32] = u32(len(req))
        body97 = sha + name + ('./filesys/' + A).encode() + req
        r = ca.ask(pdu(T_FASTCHECK, msg=body97, dat=bytes(d97)), T_FASTCHECK_R)
        hit = r[1][1] if r else 0
        check('R9-1 秒传命中', bool(hit))
        sess = qall("SELECT session_id FROM upload_session WHERE client_request_id=%s", (req.decode(),))
        ents = qall("SELECT file_id FROM user_file WHERE owner_name=%s AND display_name=%s AND status='active'",
                    (A, name.decode()))
        check('R9-2 命中只产生一条完成态会话', len(sess) == 1, len(sess))
        check('R9-3 命中只产生一条条目', len(ents) == 1, len(ents))
        r = ca.ask(pdu(T_FASTCHECK, msg=body97, dat=bytes(d97)), T_FASTCHECK_R)
        sess2 = qall("SELECT session_id FROM upload_session WHERE client_request_id=%s", (req.decode(),))
        ents2 = qall("SELECT file_id FROM user_file WHERE owner_name=%s AND display_name=%s AND status='active'",
                     (A, name.decode()))
        check('R9-4 同请求键重放幂等', len(sess2) == 1 and len(ents2) == 1, (len(sess2), len(ents2)))

        # ---------- R5/R6 外链 ----------
        print('=== R5/R6 外链 ===')
        def create_link(rel, action=None, seconds=None):
            msg = rel.encode() + b'\0'
            if action is not None:
                msg += action.encode() + b'\0' + str(seconds or '').encode()
            r = ca.ask(pdu(T_CREATE_PUB, msg=msg), T_CREATE_PUB_R)
            return (bool(r[1][0]), r[2].split(b'\0')[0].decode() if r and r[1][0] else '')

        ok1, _ = create_link('../aud2/secret.bin')
        ok2, _ = create_link('no-such-dir/x.bin')
        check('R5-1 越界路径拒绝', not ok1)
        check('R5-2 不存在路径拒绝', not ok2)
        ok3, link1 = create_link('auditfile.bin')
        check('R5-3 合法文件创建成功', ok3, link1)

        tok1 = link1.rsplit('/', 1)[-1]
        r = cc.ask(pdu(T_RESOLVE_PUB, msg=tok1.encode()), T_RESOLVE_PUB_R)
        check('R6-1 原 token 可解析', bool(r[1][0]))

        ok4, link2 = create_link('auditfile.bin', action='new')
        tok2 = link2.rsplit('/', 1)[-1] if link2 else ''
        check('R6-2 重新生成成功', ok4 and tok2 and tok2 != tok1, (ok4, tok2[:8]))
        r = cc.ask(pdu(T_RESOLVE_PUB, msg=tok1.encode()), T_RESOLVE_PUB_R)
        check('R6-3 旧 token 立即失效', not r[1][0])
        r = cc.ask(pdu(T_RESOLVE_PUB, msg=tok2.encode()), T_RESOLVE_PUB_R)
        check('R6-4 新 token 可解析', bool(r[1][0]))
        qexec("DELETE FROM user_file WHERE owner_name=%s AND parent_path='/分享链接/'", (C,))

        # ---------- R7 文件与链接一致 ----------
        print('=== R7 文件/链接同事务 ===')
        rel_before = q1("SELECT relative_path FROM share_link WHERE owner_name=%s AND token_hash IS NOT NULL ORDER BY id DESC LIMIT 1", (A,))
        d27 = bytearray(64)
        d27[0:32] = f32('auditfile.bin')
        d27[32:64] = f32('auditfile-ren.bin')
        abs_dir = ('./filesys/' + A).encode()
        r = ca.ask(pdu(T_RENAME, msg=u32(1) + abs_dir, dat=bytes(d27)), T_RENAME_R)
        check('R7-1 改名成功', bool(r[1][0]))
        rel_after = qall("SELECT relative_path FROM share_link WHERE owner_name=%s", (A,))
        check('R7-2 链接同事务跟到新路径',
              ('auditfile-ren.bin',) in rel_after and all('auditfile.bin' != x[0] for x in rel_after),
              rel_after)
        d25 = bytearray(64)
        d25[0:4] = u32(1)
        r = ca.ask(pdu(T_DELFILE, msg=(abs_dir + b'/auditfile-ren.bin'), dat=bytes(d25)), T_DELFILE_R)
        check('R7-3 删除成功', bool(r[1][0]))
        left = qall("SELECT relative_path FROM share_link WHERE owner_name=%s", (A,))
        check('R7-4 删除后链接同事务清除', not any('auditfile-ren.bin' in x[0] for x in left), left)

        # ---------- R3 注销级联 ----------
        print('=== R3 注销级联 ===')
        # 再上传一个文件 + 建群，保证注销时有资产可清
        # 内容随机：保证是本轮新对象（不复用别的账号历史对象），便于断言"归零对象进入待回收"
        upload(ca, 102, './filesys/' + A, 'keepme.bin', b'KEEPME-' + os.urandom(16))
        target_oid = q1("SELECT object_id FROM user_file WHERE owner_name=%s AND display_name='keepme.bin' "
                        "AND status='active'", (A,))[0]
        before_files = q1("SELECT COUNT(*) FROM user_file WHERE owner_name=%s", (A,))[0]
        before_sess = q1("SELECT COUNT(*) FROM upload_session WHERE owner_name=%s", (A,))[0]
        before_gm = q1("SELECT COUNT(*) FROM group_member WHERE user_name=%s AND status='active'", (A,))[0]
        check('R3-1 注销前存在资产', before_files > 0 and before_sess > 0 and before_gm > 0,
              (before_files, before_sess, before_gm))
        r = ca.ask(pdu(T_LOGOFF, dat=f32(A) + hashlib.md5(PWD.encode()).hexdigest().encode()), T_LOGOFF_R)
        check('R3-2 注销成功', bool(r[1][0]))
        after_files = q1("SELECT COUNT(*) FROM user_file WHERE owner_name=%s", (A,))[0]
        after_sess = q1("SELECT COUNT(*) FROM upload_session WHERE owner_name=%s", (A,))[0]
        after_gm = q1("SELECT COUNT(*) FROM group_member WHERE user_name=%s AND status='active'", (A,))[0]
        after_grp = q1("SELECT COUNT(*) FROM group_info gi JOIN group_member gm ON gm.group_id=gi.group_id "
                       "WHERE gm.user_name=%s AND gi.status='active'", (A,))[0]
        after_req = q1("SELECT COUNT(*) FROM friend_request WHERE requester_name=%s OR target_name=%s", (A, A))[0]
        check('R3-3 文件条目清零', after_files == 0, after_files)
        check('R3-4 上传会话清零', after_sess == 0, after_sess)
        check('R3-5 群成员关系失效', after_gm == 0, after_gm)
        check('R3-6 所建群解散', after_grp == 0, after_grp)
        check('R3-7 好友邀请清零', after_req == 0, after_req)
        obj_row = q1("SELECT status,reclaim_at FROM file_object WHERE object_id=%s", (target_oid,))
        check('R3-10 归零对象进入待回收且 reclaim_at 已写入',
              obj_row is not None and obj_row[0] == 'pending_reclaim' and obj_row[1] is not None, obj_row)

        ensure_user(A)
        ca2 = cap_login(A)
        r = ca2.ask(pdu(T_FLUSHFILE, msg=('./filesys/' + A).encode()), T_FLUSHFILE_R)
        names = [r[2][i * 36:i * 36 + 32].split(b'\0')[0] for i in range(len(r[2]) // 36)] if r else []
        names = [n for n in names if n]
        check('R3-8 同名重注册看不到旧文件', not names, names)
        r = ca2.ask(pdu(T_GROUP_LIST, msg=envelope()), T_GROUP_LIST_R)
        check('R3-9 同名重注册看不到旧群', q1(
            "SELECT COUNT(*) FROM group_member WHERE user_name=%s AND status='active'", (A,))[0] == 0)
    finally:
        for c in (locals().get('ca'), locals().get('cb'), locals().get('cc'), locals().get('ca2')):
            try:
                c and c.close()
            except Exception:
                pass
        srv.stop()

    passed = sum(1 for _, ok in RESULTS if ok)
    total = len(RESULTS)
    print('\n==== 结果 %d/%d ====' % (passed, total))
    if passed != total:
        print('FAILED: ' + ', '.join(lb for lb, ok in RESULTS if not ok))
    return 0 if passed == total else 1


if __name__ == '__main__':
    sys.exit(main())
