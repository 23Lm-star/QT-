# -*- coding: utf-8 -*-
"""REQ-EXT-TLS-001 TLS 二：链路明文可见性实证（可复现判据）。

为什么不用 tcpdump/Wireshark：Windows 回环流量不进 Npcap 可见适配器，127.0.0.1 抓不到。
等价做法是记录「服务端进程实际从网络收到的字节」——那正是任何同网段观察者能看到的
一切。两种取证方式，本脚本都用：

  R1 明文中继：客户端 →(明文) 中继(127.0.0.1:15000) →(明文) 服务端 5000。
     中继→服务端这一段 = 真实链路字节，其中应能读到私聊正文标记与口令摘要（证明可嗅探）。
  R2 TLS 直连业务成功：同一登录+私聊序列经客户端 →(TLS) 服务端 5001 完成，
     协商版本 >= TLS1.2 —— 证明「加密不是摆设，业务确实跑在这条链路上」。
  R3 形态对照：明文端口首帧首字节是帧长 0x4c(=76) 且得到应答；同一序号帧经 5001
     不可能以该形态被读取（5001 上跑的是 TLS 记录层，协商到 ECDHE-RSA-AES256-GCM-SHA384）。
  R4 带标记的完整明文帧直发 5001：不得产生任何业务应答字节（服务存活、进程不崩），
     与 R1「同一帧在明文链路会得到应答」形成对照 —— 即明文业务字节在 TLS 端口不被解析。
  R5 篡改判定：TLS 客户端经「只转不终止」的篡改中继连 5001，握手与首个请求正常，
     此后每个密文块末字节被翻转；篡改后的会话必须拿不到任何有效业务应答
     （TLS 记录鉴权失败 → 告警或断连），且服务端仍存活、新会话照常可用。
     中继不做 TLS 终止——否则篡改的是明文段，测不到记录层完整性。

结论口径：R1 证明明文链路可被任何同网段观察方读取业务内容；R2+R3+R4 证明同样的业务
字节经 TLS 链路既不被明文解析也不可读；R5 证明篡改会被拒而非静默解析。
合起来即 PRD「TLS 二」的两半句：链路无明文可见 + 篡改即断连。

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
RELAY_PORT, RELAY_TARGET, TAMPER_PORT = 15000, 5000, 15002
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


class TamperTap(threading.Thread):
    """TLS 透传中继；arm() 后翻转「下一个客户端→服务端字节块」的末字节 = 篡改一条记录。

    中继不做 TLS 终止——否则篡改的是明文段，测不到记录层完整性。
    只改客户端方向：服务端应答误伤会让"是否被解析"无法判读。
    """

    def __init__(self, port, target_port):
        super().__init__(daemon=True)
        self.port, self.target_port = port, target_port
        self.arm_flag = threading.Event()
        self.ready = threading.Event()
        self.corrupted = 0
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind((HOST, port))
        self.srv.listen(2)

    def run(self):
        self.ready.set()
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            try:
                t = socket.create_connection((HOST, self.target_port), timeout=10)
            except OSError:
                c.close()
                continue
            threading.Thread(target=self._copy, args=(c, t, True), daemon=True).start()
            threading.Thread(target=self._copy, args=(t, c, False), daemon=True).start()

    def _copy(self, src, dst, to_server):
        try:
            while True:
                d = src.recv(65536)
                if not d:
                    break
                if to_server and self.arm_flag.is_set():
                    self.arm_flag.clear()
                    d = d[:-1] + bytes([d[-1] ^ 0xFF])
                    self.corrupted += 1
                dst.sendall(d)
        except OSError:
            pass
        finally:
            for s in (src, dst):
                try:
                    s.close()
                except OSError:
                    pass


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

    # R5 篡改记录 → 连接报错断开，绝不静默解析（TLS 二后半句判据）
    #    手法：Python TLS 客户端经「篡改中继」连服务端 5001；先让握手与一次正常请求通过，
    #    再打开篡改开关（此后每个字节块翻转末字节），发第二条请求。
    #    合格：篡改后的会话拿不到任何有效业务应答（TLS 记录鉴权失败 → 告警/断连），
    #    且服务端仍存活、新会话照常可用。
    try:
        ctl3 = server_ctl.ServerCtl(str(EXE), str(DIST))
        if not ctl3.start():
            raise RuntimeError('server restart failed: %s' % ctl3.last_error)
        tap = TamperTap(TAMPER_PORT, TLS_PORT)
        tap.start()
        tap.ready.wait()
        time.sleep(.3)
        ctx = ssl.create_default_context()
        ctx.load_verify_locations(cafile=str(CERTS / 'server.crt'))
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        s = ctx.wrap_socket(socket.create_connection((HOST, TAMPER_PORT), timeout=10),
                            server_hostname='tlsserver')
        c = Conn(s)
        # 1) 篡改前基线：发 44 必须拿到 45（中继此刻纯透传）
        c.s.sendall(pdu(T_CAP))
        ok_before = len(c.collect(T_CAP_R, 1, 6)) > 0

        # 2) 篡改一条记录：被篡改的那条请求不得产生有效业务应答
        tap.arm_flag.set()
        c.s.sendall(pdu(T_CAP))
        tamper_resp, closed, saw_error = False, False, ''
        try:
            c.s.settimeout(4)
            data = c.s.recv(65536)
            if data == b'':
                closed = True
            tamper_resp = any(f[0] == T_CAP_R for f in unpack(data)[0])
        except ssl.SSLError as e:
            saw_error, closed = 'SSLError:%s' % e, True
        except socket.timeout:
            saw_error = 'timeout'
        except OSError as e:
            saw_error, closed = type(e).__name__, True

        # 3) 同一会话再发一条干净记录：会话是否被判失效（不接受"篡改记录被跳过、会话照用"）
        clean_resp, closed2 = False, False
        if not closed:
            try:
                c.s.sendall(pdu(T_CAP))
                c.s.settimeout(4)
                d2 = c.s.recv(65536)
                if d2 == b'':
                    closed2 = True
                clean_resp = any(f[0] == T_CAP_R for f in unpack(d2)[0])
            except ssl.SSLError as e:
                saw_error = saw_error + ' / SSLError:%s' % e
                closed2 = True
            except socket.timeout:
                pass
            except OSError:
                closed2 = True
        try:
            c.close()
        except OSError:
            pass

        # 4) 服务端存活 + 新会话可用（篡改不能拖垮进程或污染后续连接）
        fresh = session_via_tls(marker + '_AFTER')
        # 判据：篡改记录不被静默解析（无有效应答）；且会话失效——表现为断开/报错，
        #       或后续干净记录也不再被应答（服务端已放弃该流，只是没主动 FIN）。
        session_dead = closed or closed2 or saw_error in ('SSLError',) or not clean_resp
        check('R5-tamper-not-silently-parsed',
              tap.corrupted == 1 and ok_before and not tamper_resp and not clean_resp and fresh[0],
              'corrupted=%d before=%s tamper_resp=%s clean_resp=%s closed=%s/%s err=%r '
              'session_after=%s' % (tap.corrupted, ok_before, tamper_resp, clean_resp,
                                    closed, closed2, saw_error, fresh[0]))
        print('NOTE R5 断连形态（事实记录，不改判据）：closed=%s/%s error=%r 会话判死=%s'
              % (closed, closed2, saw_error, session_dead))
    except Exception as e:
        check('R5-tamper-not-silently-parsed', False, repr(e))
    finally:
        try:
            ctl3.stop()
        except Exception:
            pass

    fails = [l for l, ok in results if not ok]
    print('SUMMARY %d/%d passed%s' % (len(results) - len(fails), len(results),
                                      ('; failed: ' + ', '.join(fails)) if fails else ''))
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
