# 一次性端到端验证：TLS 连接的私聊实时投递与上传应答路由（2026-11-14 GUI 缺陷批次）
# 复现根因：TLS 监听是独立 MyTcpServer 实例，旧代码把连接登记进 TLS 实例自己的
# tcpsocketlist，主单例 deliverChat/上传写盘回调只看主单例连接表 → 109 推送、
# 32 上传应答全部丢失（网页网关走明文端口在主表内，故仅桌面端故障）。
# 断言（全部走 TLS 端口 5001）：
#   1) A 发 15 私聊 → B(已声明 116) 实时收到 109；
#   2) B 发 15 → A 实时收到 109（双向）；
#   3) 私聊回显 16 正常（同 socket 路径，作对照）。
# 前置：MySQL 可达（读 Server/connect.config）；dist/Server.exe 为当前分支产物；
# TCP 5000 独占，串行运行；夹具复用 pfa/pfb，测试产生的私聊行测试尾部自行清理。
import os
import ssl
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
import server_ctl  # noqa: E402
import pymysql     # noqa: E402

HOST, PORT, HDR = '127.0.0.1', 5000, 76
TLS_PORT = PORT + 1
PWD = '123456'

T_REG, T_REG_R, T_LOGIN, T_LOGIN_R, T_CAP, T_CAP_R = 1, 2, 3, 4, 44, 45
T_CHAT_ACK_CAP, T_CHAT, T_CHAT_R, T_DELIVER = 116, 15, 16, 109
MSG_HDR = 38  # Msg：totalLen(u32)+msgLen(u32)+Time[30]


def f32(s):
    return s.encode()[:31].ljust(32, b'\0')


def pdu(t, msg=b'', dat=b''):
    return struct.pack('<III', HDR + len(msg), len(msg), t) + dat[:64].ljust(64, b'\0') + msg


class TlsConn:
    def __init__(self):
        raw = socket.create_connection((HOST, TLS_PORT), timeout=15)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        self.s = ctx.wrap_socket(raw, server_hostname=HOST)
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

    def drain(self, seconds=1.5):
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
        except OSError:
            pass


def unpack(buf):
    out = []
    while len(buf) >= HDR:
        tl, ml, ty = struct.unpack('<III', buf[:12])
        if tl < HDR or tl != HDR + ml or len(buf) < tl:
            break
        out.append((ty, buf[12:76], buf[76:tl]))
        buf = buf[tl:]
    return out, buf


def cap_raw(c):
    r = c.ask(pdu(T_CAP), T_CAP_R)
    if r is None:
        raise RuntimeError('验证码响应缺失')
    return r


def cap_login(name):
    c = TlsConn()
    code = cap_raw(c)[2].split(b'\0')[0]
    r = c.ask(pdu(T_LOGIN, msg=code + b'\0', dat=f32(name) + PWD.encode()), T_LOGIN_R)
    code = struct.unpack('<i', r[1][:4])[0] if r else -99
    if code != 0:
        c.close()
        raise RuntimeError('登录失败 ' + name + ' code=' + str(code))
    return c


def ensure_user(name):
    c = TlsConn()
    code = cap_raw(c)[2].split(b'\0')[0]
    r = c.ask(pdu(T_REG, msg=code + b'\0', dat=f32(name) + PWD.encode()), T_REG_R)
    rc = struct.unpack('<i', r[1][:4])[0] if r else -99
    c.close()
    if rc not in (0, -2012):    # 0=注册成功；-2012(账号已存在)=复用
        raise RuntimeError('注册失败 ' + name + ' code=' + str(rc))


def msg_record(content):
    body = content.encode()
    now = time.strftime('%a %b %d %H:%M:%S %Y').encode()[:29].ljust(30, b'\0')
    return struct.pack('<II', MSG_HDR + len(body) + 1, len(body) + 1) + now + body + b'\0'


def parse_deliver(pkt):
    sender = pkt[1][:32].split(b'\0')[0].decode('utf-8', 'replace')
    body = pkt[2][MSG_HDR:].split(b'\0')[0].decode('utf-8', 'replace')
    return sender, body


def main():
    if not EXE.exists():
        print('FATAL: 未找到 ' + str(EXE))
        return 2
    os.environ['CLOUDDISK_CAPTCHA_TEXT_FOR_TESTS'] = '1'
    srv = server_ctl.ServerCtl(str(EXE), str(DIST), HOST, PORT)
    if not srv.start():
        print('FATAL: Server 启动失败：' + str(srv.last_error))
        return 2
    results = []

    def check(label, cond, detail=''):
        results.append((label, bool(cond)))
        print(('[PASS] ' if cond else '[FAIL] ') + label + (' :: ' + str(detail) if detail else ''))
        sys.stdout.flush()

    a = b = None
    try:
        A, B = 'pfa', 'pfb'
        ensure_user(A)
        ensure_user(B)
        a, b = cap_login(A), cap_login(B)
        a.s.sendall(pdu(T_CHAT_ACK_CAP))       # 桌面端语义：登录后声明 109/110 能力
        b.s.sendall(pdu(T_CHAT_ACK_CAP))
        time.sleep(.2)
        b.drain(.4)

        content = 'tls-realtime-%d' % int(time.time())
        r = a.ask(pdu(T_CHAT, msg=msg_record(content), dat=f32(A) + f32(B)), T_CHAT_R)
        check('16 私聊入库应答（同 socket 对照）', r is not None
              and struct.unpack('<i', r[1][32:36])[0] == 0,
              '缺失' if r is None else struct.unpack('<i', r[1][32:36])[0])
        ev = b.drain(1.5)
        got = [g for g in ev if g[0] == T_DELIVER]
        if got:
            sender, body = parse_deliver(got[0])
            check('A→B 实时 109 推送', sender == A and body == content, (sender, body))
        else:
            check('A→B 实时 109 推送', False, 'B 未收到 109，类型: %s' % [g[0] for g in ev])

        content2 = 'tls-realtime-rev-%d' % int(time.time())
        r2 = b.ask(pdu(T_CHAT, msg=msg_record(content2), dat=f32(B) + f32(A)), T_CHAT_R)
        check('16 反向入库应答', r2 is not None
              and struct.unpack('<i', r2[1][32:36])[0] == 0,
              '缺失' if r2 is None else struct.unpack('<i', r2[1][32:36])[0])
        ev2 = a.drain(1.5)
        got2 = [g for g in ev2 if g[0] == T_DELIVER]
        if got2:
            sender2, body2 = parse_deliver(got2[0])
            check('B→A 实时 109 推送', sender2 == B and body2 == content2, (sender2, body2))
        else:
            check('B→A 实时 109 推送', False, 'A 未收到 109，类型: %s' % [g[0] for g in ev2])
    finally:
        if a:
            a.close()
        if b:
            b.close()
        try:
            lines = (ROOT / 'Server' / 'connect.config').read_text(encoding='utf-8').splitlines()
            with pymysql.connect(host=lines[3] or '127.0.0.1', port=int(lines[4]), user=lines[6],
                                 password=lines[7], database=lines[5], autocommit=True) as c, \
                    c.cursor() as cur:
                cur.execute("DELETE FROM message_info WHERE (seed_name='pfa' AND rev_name='pfb')"
                            " OR (seed_name='pfb' AND rev_name='pfa')")
        except Exception as e:  # noqa: BLE001 清理失败不掩盖断言结果
            print('清理提示：私聊夹具行未删除', e)
        srv.stop()

    ok = all(r for _, r in results)
    print('==== 汇总：%d/%d 通过 ====' % (sum(1 for _, r in results if r), len(results)))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
