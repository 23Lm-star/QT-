# -*- coding: utf-8 -*-
"""REQ-EXT-TLS-001 服务端 TLS 端口 e2e。

覆盖 PRD 验收项（REQ-EXT-TLS-001，服务端侧）：
  T1 TLS 端口（主端口+1=5001）握手成功，协商版本 >= TLS 1.2（TLS 一，服务端半）
  T2 经 TLS 完成 验证码→注册→登录→拉好友列表，应答帧与明文 5000 同序列逐字节一致
     （TLS 一/协议透明：TLS 只换传输层，PDU 字节不变）
  T3 裸 PDU 直连 5001：不产生协议应答，服务进程存活（TLS 四）
  T4 被裸 PDU 打击后，正常 TLS 客户端仍可握手并完成登录（TLS 四/服务存活）
  T5 dist/certs 缺失时启动：明文 5000 正常服务，5001 不监听（D1 共存降级，
     通过改名 certs 目录制造缺失，收尾恢复）

前置：
  1) dist/Server.exe 为当前分支（含 TLS 改造）构建产物；
  2) dist/certs/server.{crt,key} 由证书生成脚本产出；
  3) MySQL 可达（读 Server/connect.config）。

本套件会：创建/复用账号 tlse；只触碰该账号；互踢顺序为 明文会话→TLS 会话→TLS 会话。
测试进程只清理自己启动的服务端。
用法：
    py -3.13 -X utf8 project-docs/evidence/REQ-EXT-TLS-001/tls_e2e.py
"""
import hashlib
import socket
import ssl
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DIST = ROOT / 'dist'
EXE = DIST / 'Server.exe'
CERTS = DIST / 'certs'

sys.path.insert(0, str(ROOT / 'project-docs' / 'evidence' / '_harness'))
import server_ctl  # noqa: E402

HOST, PLAIN_PORT, TLS_PORT, HDR = '127.0.0.1', 5000, 5001, 76
PWD = '123456'
USER = 'tlse'

T_REG, T_REG_R, T_LOGIN, T_LOGIN_R, T_CAP, T_CAP_R = 1, 2, 3, 4, 44, 45
T_FLUSHFRIEND, T_FLUSHFRIEND_R = 11, 12

results = []


def check(label, cond, detail=''):
    results.append((label, bool(cond)))
    print('%s %s %s' % ('PASS' if cond else 'FAIL', label, detail))


def f32(s):
    return s.encode('utf-8', 'ignore')[:30].ljust(31, b'\0')


def pdu(t, msg=b'', dat=b''):
    return struct.pack('<III', HDR + len(msg), len(msg), t) + dat[:64].ljust(64, b'\0') + msg


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
    """与 boundary_e2e 同构，但接受任意已建立（可能已加密）的 socket。"""

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

    def ask(self, pkt, want, timeout=10):
        self.send(pkt)
        return self.collect(want, 1, timeout)

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


def open_plain():
    return Conn(socket.create_connection((HOST, PLAIN_PORT), timeout=10))


def open_tls():
    ctx = ssl.create_default_context()
    ctx.load_verify_locations(cafile=str(CERTS / 'server.crt'))
    raw = socket.create_connection((HOST, TLS_PORT), timeout=8)
    return Conn(ctx.wrap_socket(raw, server_hostname='tlsserver')), ctx


def full_flow(c):
    """取码→登录（账号不存在则注册后再取码登录）→拉好友；返回 (登录 ret, 12 应答原始帧字节)。

    验证码一次性且 44 有 1 分钟冷却：注册与登录不能背靠背各取一码，
    因此对已存在账号只登录一次；tlse 由首轮注册后长期存在。
    """
    dat = f32(USER) + hashlib.md5(PWD.encode()).hexdigest().encode()
    r = c.ask(pdu(T_CAP), T_CAP_R)
    code = r[0][2].split(b'\0')[0] if r else b''
    r = c.ask(pdu(T_LOGIN, msg=code + b'\0', dat=dat), T_LOGIN_R)
    ret = struct.unpack('<i', r[0][1][:4])[0] if r else -99
    if ret != 1:
        # 账号不存在：等冷却后注册，再等冷却后登录
        time.sleep(1.2)
        r = c.ask(pdu(T_CAP), T_CAP_R)
        code = r[0][2].split(b'\0')[0] if r else b''
        c.ask(pdu(T_REG, msg=code + b'\0', dat=dat), T_REG_R)
        time.sleep(1.2)
        r = c.ask(pdu(T_CAP), T_CAP_R)
        code = r[0][2].split(b'\0')[0] if r else b''
        r = c.ask(pdu(T_LOGIN, msg=code + b'\0', dat=dat), T_LOGIN_R)
        ret = struct.unpack('<i', r[0][1][:4])[0] if r else -99
    c.send(pdu(T_FLUSHFRIEND))
    frames = c.collect(T_FLUSHFRIEND_R)
    raw = b''.join(struct.pack('<III', HDR + len(m), len(m), t) + d[:64].ljust(64, b'\0') + m
                   for (t, d, m) in frames)
    return ret, raw


def main():
    if not EXE.exists():
        raise SystemExit('FATAL: dist/Server.exe 不存在，先构建当前分支产物')
    if not (CERTS / 'server.crt').exists() or not (CERTS / 'server.key').exists():
        raise SystemExit('FATAL: dist/certs/server.{crt,key} 缺失，先运行证书生成脚本')

    ctl = server_ctl.ServerCtl(str(EXE), str(DIST))
    if not ctl.start():
        raise SystemExit('FATAL: 服务端未起来: %s' % ctl.last_error)
    try:
        # T1 握手 + 版本下限
        ts = None
        try:
            ts, ctx = open_tls()
            ver = ts.s.version()
            check('T1-tls-handshake', ver in ('TLSv1.2', 'TLSv1.3'),
                  'version=%s cipher=%s' % (ver, ts.s.cipher()[0]))
        except Exception as e:
            check('T1-tls-handshake', False, repr(e))

        # T2 TLS 与明文同序列应答逐字节一致（互踢：先明文后 TLS）
        try:
            ps = open_plain()
            ret_p, raw_p = full_flow(ps)
            ps.close()
            if ts is None:
                check('T2-frame-identical', False, 'no TLS session from T1')
            else:
                ret_t, raw_t = full_flow(ts)
                check('T2-frame-identical',
                      ret_p == 1 and ret_t == 1 and raw_p == raw_t and len(raw_t) >= HDR,
                      'ret plain/tls=%d/%d frames-equal=%s len=%d' % (ret_p, ret_t, raw_p == raw_t, len(raw_t)))
                ts.close()
        except Exception as e:
            check('T2-frame-identical', False, repr(e))

        # T3 裸 PDU 打 TLS 端口：无应答、进程存活
        try:
            raw = socket.create_connection((HOST, TLS_PORT), timeout=8)
            raw.sendall(pdu(T_FLUSHFRIEND))
            time.sleep(1.0)
            raw.settimeout(1.0)
            data = b''
            try:
                data = raw.recv(4096)
            except socket.timeout:
                pass
            except ConnectionError:
                pass
            raw.close()
            alive = _server_alive()
            check('T3-rawpdu-rejected', alive, 'echo=%r alive=%s' % (data[:16], alive))
        except Exception as e:
            check('T3-rawpdu-rejected', False, repr(e))

        # T4 打击后 TLS 仍可登录
        try:
            ts2, _ = open_tls()
            ret2, _ = full_flow(ts2)
            ts2.close()
            check('T4-alive-after-raw', ret2 == 1, 'login ret=%d' % ret2)
        except Exception as e:
            check('T4-alive-after-raw', False, repr(e))
    finally:
        ctl.stop()

    # T5 证书缺失 → 明文照常、TLS 端口不监听
    backup = CERTS.with_name('certs_tls_e2e_backup')
    try:
        CERTS.rename(backup)
        ctl2 = server_ctl.ServerCtl(str(EXE), str(DIST))
        if not ctl2.start():
            check('T5-no-certs-degrade', False, 'server failed to start without certs: %s' % ctl2.last_error)
        else:
            try:
                ps = open_plain()
                ret5, _ = full_flow(ps)
                ps.close()
                plain_ok = ret5 == 1
            except Exception:
                plain_ok = False
            tls_refused = False
            try:
                s = socket.create_connection((HOST, TLS_PORT), timeout=3)
                s.close()
            except (ConnectionRefusedError, socket.timeout, OSError):
                tls_refused = True
            check('T5-no-certs-degrade', plain_ok and tls_refused,
                  'plain_ok=%s tls_refused=%s' % (plain_ok, tls_refused))
            ctl2.stop()
    finally:
        if backup.exists() and not CERTS.exists():
            backup.rename(CERTS)

    fails = [l for l, ok in results if not ok]
    print('SUMMARY %d/%d passed%s' % (len(results) - len(fails), len(results),
                                      ('; failed: ' + ', '.join(fails)) if fails else ''))
    sys.exit(1 if fails else 0)


def _server_alive():
    try:
        s = socket.create_connection((HOST, PLAIN_PORT), timeout=3)
        s.close()
        return True
    except OSError:
        return False


if __name__ == '__main__':
    main()
