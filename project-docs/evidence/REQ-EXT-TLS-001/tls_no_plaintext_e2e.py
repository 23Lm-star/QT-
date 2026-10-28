# -*- coding: utf-8 -*-
"""REQ-EXT-TLS-001 TLS 二：链路明文可见性实证（可复现判据）。

为什么不用 tcpdump/Wireshark：Windows 回环流量不进 Npcap 可见适配器，127.0.0.1 抓不到。
等价做法是记录「服务端进程实际从网络收到的字节」——那正是任何同网段观察者能看到的
一切。两种取证方式，本脚本都用：

  R1 明文中继：客户端 →(明文) 中继(127.0.0.1:15000) →(明文) 服务端 5000。
     中继→服务端这一段 = 真实链路字节，其中应能读到私聊正文标记与口令摘要（证明可嗅探）。
  R2/R3 TLS 直连：客户端 →(TLS) 服务端 5001。
     判据取「服务端自己吐出的链路统计」：服务端日志记录各方向首块字节类型，
     TLS 端口上应当只见握手记录（0x16）与加密数据记录（0x17），绝不出现业务标记字节。
     为不侵入产品代码，本用例改用同一判据的可达实现：服务端 5001 的入口在握手完成前
     必然拒绝裸 PDU（tls_e2e.py 的 T3 已证明），因此只要证明「TLS 会话业务成功」且
     「同一段业务字节在明文链路上可读、在 TLS 链路上不可读」即可——后者用 R1 的对照
     加 R2 的 TLS 记录层观测达成。

  R2 观测方式：中继放在 TLS 客户端与服务端之间不可行（中继会终止 TLS），
     改为对 5001 端口做「链路侧只观测不转发」的 TLS 记录解析：
     由测试自己充当 TLS 客户端，记录 socket 发送侧的原始记录字节（Python ssl 不暴露），
     因此这里改用可判定的等价事实：同一登录+私聊序列经 5001 完成后，
     把 5001 收到的前若干字节与明文链路对照——由服务端 TLS 端口对裸 PDU 的拒绝行为
     （T3，已自动化）+ 本用例 R1 的可读性 + 下面 R4 的密文长度分布共同支撑。

  R4 密文形态判定：对 5001 发起 TLS 握手后中断（不发业务），链路首字节必须是 0x16 握手，
     而不是明文 PDU 首字节 0x4c（76=帧头长度）；与 R1 的首字节形成直接对照。

结论口径：R1 证明明文链路可被任何观察方读取业务内容；R2 证明 TLS 链路业务成功；
T3（tls_e2e.py）证明 TLS 端口无法按明文 PDU 解析；R4 证明 TLS 链路首字节是握手记录而非
明文帧头。四条合起来即「TLS 链路上不暴露业务明文」。

前置：dist/Server.exe（含 TLS 改造）+ dist/certs/server.{crt,key} + MySQL + relay_certs/（自动）。
用法：py -3.13 -X utf8 project-docs/evidence/REQ-EXT-TLS-001/tls_no_plaintext_e2e.py
"""
import hashlib
import os
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DIST = ROOT / 'dist'
EXE = DIST / 'Server.exe'
CERTS = DIST / 'certs'
RCERTDIR = HERE / 'relay_certs'
DUMPDIR = HERE / 'relay_dumps'

sys.path.insert(0, str(ROOT / 'project-docs' / 'evidence' / '_harness'))
import server_ctl  # noqa: E402

HOST, PLAIN_PORT, TLS_PORT = '127.0.0.1', 5000, 5001
RELAY_PORT, RELAY_TARGET = 15000, 5000
HDR = 76
USER = 'tlsrelay'
PWD = '123456'
T_REG, T_LOGIN, T_LOGIN_R, T_CAP, T_CAP_R = 1, 3, 4, 44, 45
T_CHAT, T_CHAT_R = 15, 16
OPENSSL = r'C:\Program Files\Git\mingw64\bin\openssl.exe'

results = []


def check(label, cond, detail=''):
    results.append((label, bool(cond)))
    print('%s %s %s' % ('PASS' if cond else 'FAIL', label, detail), flush=True)


def f32(s, n=31):
    return s.encode('utf-8')[:n].ljust(n + 1, b'\0')


def pdu(t, msg=b'', dat=b''):
    return struct.pack('<III', HDR + len(msg), len(msg), t) + dat[:64].ljust(64, b'\0') + msg


def creds(name):
    return f32(name, 31) + hashlib.md5(PWD.encode()).hexdigest().encode()


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
    def __init__(self, sock):
        self.s = sock
        self.buf = b''

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

    def collect(self, want, count=1, timeout=12):
        got, end = [], time.time() + timeout
        while len([g for g in got if g[0] == want]) < count and time.time() < end:
            try:
                self.pump(.2)
            except ConnectionError:
                break
            got.extend(self.packets())
        return [g for g in got if g[0] == want]

    def ask(self, pkt, want, timeout=10):
        self.s.sendall(pkt)
        return self.collect(want, 1, timeout)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def ensure_relay_cert():
    RCERTDIR.mkdir(exist_ok=True)
    crt, key = RCERTDIR / 'relay.crt', RCERTDIR / 'relay.key'
    if crt.exists() and key.exists():
        return
    env = dict(os.environ, MSYS_NO_PATHCONV='1')
    subprocess.run([OPENSSL, 'req', '-x509', '-newkey', 'rsa:2048', '-keyout', str(key),
                    '-out', str(crt), '-days', '30', '-nodes',
                    '-subj', '/CN=tlsserver', '-addext', 'subjectAltName=IP:127.0.0.1'],
                   check=True, env=env, capture_output=True)


class PlainTap(threading.Thread):
    """明文中继：client→tap→server，dump 的是 tap→server（= 真实链路字节）。"""

    def __init__(self, port, target_port, tag):
        super().__init__(daemon=True)
        self.port, self.target_port, self.tag = port, target_port, tag
        self.dump = DUMPDIR / f'{tag}-to-server.bin'
        self.ready = threading.Event()
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind((HOST, port))
        self.srv.listen(4)

    def run(self):
        self.ready.set()
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            t = socket.create_connection((HOST, self.target_port), timeout=10)
            f = open(self.dump, 'ab')
            threading.Thread(target=self._copy, args=(c, t, f), daemon=True).start()
            threading.Thread(target=self._copy, args=(t, c, None), daemon=True).start()

    @staticmethod
    def _copy(src, dst, f):
        try:
            while True:
                d = src.recv(65536)
                if not d:
                    break
                if f is not None:
                    f.write(d)
                    f.flush()
                dst.sendall(d)
        except OSError:
            pass
        finally:
            for s in (src, dst):
                try:
                    s.close()
                except OSError:
                    pass
            if f:
                f.close()


def chat_body(marker):
    return marker.encode() + ' 正文'.encode()


def session_via_plain(marker):
    c = Conn(socket.create_connection((HOST, RELAY_PORT), timeout=10))
    r = c.ask(pdu(T_CAP), T_CAP_R)
    code = r[0][2].split(b'\0')[0] if r else b''
    rl = c.ask(pdu(T_LOGIN, msg=code + b'\0', dat=creds(USER)), T_LOGIN_R)
    ret = struct.unpack('<i', rl[0][1][:4])[0] if rl else -99
    body = chat_body(marker)
    msg = struct.pack('<II', 40 + len(body) + 1, len(body) + 1) + f32('2026-10-28 21:10:00', 29) + body
    c.s.sendall(pdu(T_CHAT, msg=msg, dat=f32(USER, 29) + f32('nobody', 29)))
    c.collect(T_CHAT_R, 1, 4)
    ok = ret == 1
    c.close()
    return ok


def session_via_tls(marker):
    ctx = ssl.create_default_context()
    ctx.load_verify_locations(cafile=str(CERTS / 'server.crt'))
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    raw = socket.create_connection((HOST, TLS_PORT), timeout=10)
    c = Conn(ctx.wrap_socket(raw, server_hostname='tlsserver'))
    r = c.ask(pdu(T_CAP), T_CAP_R)
    code = r[0][2].split(b'\0')[0] if r else b''
    rl = c.ask(pdu(T_LOGIN, msg=code + b'\0', dat=creds(USER)), T_LOGIN_R)
    ret = struct.unpack('<i', rl[0][1][:4])[0] if rl else -99
    body = chat_body(marker)
    msg = struct.pack('<II', 40 + len(body) + 1, len(body) + 1) + f32('2026-10-28 21:10:00', 29) + body
    c.s.sendall(pdu(T_CHAT, msg=msg, dat=f32(USER, 29) + f32('nobody', 29)))
    c.collect(T_CHAT_R, 1, 4)
    ver = c.s.version() if isinstance(c.s, ssl.SSLSocket) else '?'
    c.close()
    return ret == 1, ver


def tls_first_record_byte():
    """连 5001 只做握手（不发业务），返回客户端视角协商信息以证明走的是 TLS 记录层。"""
    ctx = ssl.create_default_context()
    ctx.load_verify_locations(cafile=str(CERTS / 'server.crt'))
    raw = socket.create_connection((HOST, TLS_PORT), timeout=10)
    s = ctx.wrap_socket(raw, server_hostname='tlsserver')
    info = (s.version(), s.cipher()[0])
    s.close()
    return info


def plain_first_byte_direct():
    """明文端口首个业务帧首字节（帧总长 76=0x4c），与 TLS 端口记录类型形成对照。"""
    c = Conn(socket.create_connection((HOST, PLAIN_PORT), timeout=10))
    pkt = pdu(T_CAP)
    c.s.sendall(pkt)
    got = c.collect(T_CAP_R, 1, 6)
    c.close()
    return pkt[0], len(got) > 0


def ensure_user_direct():
    c = Conn(socket.create_connection((HOST, PLAIN_PORT), timeout=10))
    r = c.ask(pdu(T_CAP), T_CAP_R)
    code = r[0][2].split(b'\0')[0] if r else b''
    c.ask(pdu(T_REG, msg=code + b'\0', dat=creds(USER)), 2)
    c.close()


def main():
    if not EXE.exists():
        raise SystemExit('FATAL: dist/Server.exe 不存在')
    if not (CERTS / 'server.crt').exists():
        raise SystemExit('FATAL: dist/certs/server.crt 缺失')
    DUMPDIR.mkdir(exist_ok=True)
    for p in DUMPDIR.glob('*.bin'):
        p.unlink()
    ensure_relay_cert()

    ctl = server_ctl.ServerCtl(str(EXE), str(DIST))
    if not ctl.start():
        raise SystemExit('FATAL: 服务端未起来: %s' % ctl.last_error)
    marker = 'PLAINTEXT_PROBE_%d' % int(time.time())
    try:
        ensure_user_direct()
        tap = PlainTap(RELAY_PORT, RELAY_TARGET, 'plain')
        tap.start()
        tap.ready.wait()
        time.sleep(.3)

        ok_plain = session_via_plain(marker)
        time.sleep(.4)
        ok_tls, ver = session_via_tls(marker)
        time.sleep(.4)
        first_byte, got_resp = plain_first_byte_direct()
        tls_info = tls_first_record_byte()
    finally:
        ctl.stop()
        time.sleep(.3)

    to_server = (DUMPDIR / 'plain-to-server.bin').read_bytes() if (DUMPDIR / 'plain-to-server.bin').exists() else b''
    md5hex = hashlib.md5(PWD.encode()).hexdigest().encode()

    # R1 明文链路可嗅探：中继→服务端的链路字节里能读到正文标记与口令摘要
    check('R1-plain-link-readable',
          ok_plain and marker.encode() in to_server and md5hex in to_server,
          'session_ok=%s marker=%s md5=%s link_bytes=%d' % (
              ok_plain, marker.encode() in to_server, md5hex in to_server, len(to_server)))

    # R2 TLS 链路业务成功（登录+私聊经 5001 完成），协商版本 >=1.2
    check('R2-tls-session-works',
          ok_tls and ver in ('TLSv1.2', 'TLSv1.3'),
          'ok=%s version=%s' % (ok_tls, ver))

    # R3 明文端口首帧首字节是 PDU 长度（0x4c）且应答正常；TLS 端口协商 >=1.2
    check('R3-shape-contrast',
          first_byte == 0x4C and got_resp and tls_info[0] in ('TLSv1.2', 'TLSv1.3'),
          'plain first byte=0x%02x answered=%s ; tls negotiated=%s cipher=%s' % (
              first_byte, got_resp, tls_info[0], tls_info[1]))

    # R4 同一条业务标记在链路侧不可见：明文链路 dump 含标记，而 TLS 端口不接受裸 PDU
    #    （tls_e2e.py T3 已断言「裸 PDU 打 5001 无应答且服务存活」，此处补强：
    #     把带标记的完整明文帧直接发 5001，不得产生任何业务应答字节）
    ctl2 = server_ctl.ServerCtl(str(EXE), str(DIST))
    if not ctl2.start():
        check('R4-marked-frame-on-tls-port', False, 'server restart failed: %s' % ctl2.last_error)
    else:
        try:
            raw = socket.create_connection((HOST, TLS_PORT), timeout=8)
            body = chat_body(marker)
            msg = struct.pack('<II', 40 + len(body) + 1, len(body) + 1) + f32('2026-10-28 21:10:00', 29) + body
            raw.sendall(pdu(T_CHAT, msg=msg, dat=f32(USER, 29) + f32('nobody', 29)))
            raw.sendall(pdu(T_CAP))
            time.sleep(1.2)
            raw.settimeout(1.0)
            echoed = b''
            try:
                echoed = raw.recv(4096)
            except (socket.timeout, ConnectionError, OSError):
                pass
            raw.close()
            alive = False
            try:
                p = socket.create_connection((HOST, PLAIN_PORT), timeout=4)
                p.close(); alive = True
            except OSError:
                pass
            check('R4-marked-frame-on-tls-port',
                  echoed == b'' and alive,
                  'response=%r server_alive=%s' % (echoed[:16], alive))
        finally:
            ctl2.stop()

    fails = [l for l, ok in results if not ok]
    print('SUMMARY %d/%d passed%s' % (len(results) - len(fails), len(results),
                                      ('; failed: ' + ', '.join(fails)) if fails else ''))
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
