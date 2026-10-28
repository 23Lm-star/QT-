# -*- coding: utf-8 -*-
"""REQ-EXT-TLS-001 TLS 七：同一文件下载 明文(5000) vs TLS(5001) 吞吐对比（实测，不做预估）。

方法：登录 tlsx → mkdir 基准目录 → 32MiB 随机内容按 1MiB 分片上传一次（协议帧与
REQ-EXT-TRANSFER-002 套件同口径）→ 同一连接断开后，分别经明文与 TLS 通道计时下载
同一文件；每种通道热身 1 次 + 实测 3 次取最好值。互踢语义：所有步骤串行、每步独立连接。
前置：dist/Server.exe 当前分支产物 + dist/certs/server.{crt,key} + MySQL 可达。
夹具数据：创建/复用账号 tlsx，写 32MiB 测试文件于其 filesys/tlsx/bench 目录。
用法：py -3.13 -X utf8 project-docs/evidence/REQ-EXT-TLS-001/tls_throughput.py
"""
import hashlib
import os
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
USER = 'tlsx'
MAGIC = 0x5253554D

T_REG, T_REG_R, T_LOGIN, T_LOGIN_R, T_CAP, T_CAP_R = 1, 2, 3, 4, 44, 45
T_MKDIR, T_MKDIR_R = 19, 20
T_UPINIT, T_UPINIT_R, T_UPDATA, T_UPDATA_R = 29, 30, 31, 32
T_DOWN, T_DOWN_R, T_DATA, T_FINISH = 40, 41, 42, 43

ID_OFF, DL_ID_OFF = 48, 16
FSROOT = str(DIST / 'filesys').replace('\\', '/')   # 服务端 cwd=dist，路径必须绝对化（相对 ./filesys 随套件 cwd 漂移）
CHUNK = 1024 * 1024
SIZE = 32 * CHUNK


def f32(s):
    return s.encode('utf-8')[:31].ljust(32, b'\0')


def pdu(t, msg=b'', dat=b''):
    # caData 固定 64 字节：dat 必须补齐，否则帧头与总长不符（服务端按畸形包处理）
    return struct.pack('<III', HDR + len(msg), len(msg), t) + dat[:64].ljust(64, b'\0') + msg


def creds(n):
    return f32(n) + hashlib.md5(PWD.encode()).hexdigest().encode()


def unpack(buf):
    out = []
    while len(buf) >= HDR:
        tl, ml, ty = struct.unpack('<III', buf[:12])
        if tl < HDR or tl != HDR + ml or len(buf) < tl:
            break
        out.append((ty, buf[12:76], buf[76:tl]))
        buf = buf[tl:]
    return out, buf


class C:
    def __init__(self, sock):
        self.sock = sock
        self.buf = b''
        self.pending = []

    def pump(self, t=.2):
        self.sock.settimeout(t)
        try:
            d = self.sock.recv(1 << 20)
            if d:
                self.buf += d
                ps, self.buf = unpack(self.buf)
                self.pending.extend(ps)
        except socket.timeout:
            pass

    def take(self, t=.15):
        self.pump(t)
        out, self.pending = self.pending[:], []
        return out

    def wait(self, ty, timeout=15.0):
        dl = time.time() + timeout
        while time.time() < dl:
            for i, p in enumerate(self.pending):
                if p[0] == ty:
                    return self.pending.pop(i)
            self.pump(.2)
        return None

    def send(self, pkt):
        self.sock.sendall(pkt)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def open_plain():
    return C(socket.create_connection((HOST, PLAIN_PORT), timeout=20))


def open_tls():
    ctx = ssl.create_default_context()
    ctx.load_verify_locations(cafile=str(CERTS / 'server.crt'))
    raw = socket.create_connection((HOST, TLS_PORT), timeout=10)
    return C(ctx.wrap_socket(raw, server_hostname='tlsserver'))


def ensure_user():
    """独立连接注册 tlsx（验证码冷却是连接级的，注册与登录不能共用一条连接背靠背取码）。"""
    c = open_plain()
    c.send(pdu(T_CAP)); r = c.wait(T_CAP_R)
    code = r[2].split(b'\0')[0] if r else b''
    c.send(pdu(T_REG, msg=code + b'\0', dat=creds(USER)))
    c.wait(T_REG_R)
    c.close()


def bench_entry_exists():
    """基准文件条目是否已存在（存在即跳过上传，避免重名自动改名破坏固定名下载）。"""
    import pymysql
    cfg = open(ROOT / 'Server' / 'connect.config', 'rb').read().split(b'\r\n')
    conn = pymysql.connect(host=cfg[3].decode(), port=int(cfg[4]), user=cfg[6].decode(),
                           password=cfg[7].decode(), database=cfg[5].decode())
    cur = conn.cursor()
    cur.execute("SELECT file_id FROM user_file WHERE owner_name=%s "
                "AND display_name='bench.bin' AND status='active'", (USER,))
    row = cur.fetchone()
    conn.close()
    return row is not None


def login(c):
    """独立连接取码→登录。返回 ret（1=成功）。"""
    c.send(pdu(T_CAP))
    r = c.wait(T_CAP_R)
    code = r[2].split(b'\0')[0] if r else b''
    c.send(pdu(T_LOGIN, msg=code + b'\0', dat=creds(USER)))
    r = c.wait(T_LOGIN_R)
    return struct.unpack('<i', r[1][:4])[0] if r else -99


def bench_download(c):
    """计时下载基准文件，返回 (秒, 正文字节数)。42 正文按 tid 收干到 43。"""
    tid = 9100
    # caMsg 必须是 目录/文件名 全路径（服务端 absolutePath() 取父目录做条目键，与 TRANSFER-002 一致）
    path = f'{FSROOT}/{USER}/bench/bench.bin'
    c.send(pdu(T_DOWN, msg=path.encode(),
               dat=f32('bench.bin') + struct.pack('<q', 0) + b'\0' * 8 + struct.pack('<Q', tid)))
    r = c.wait(T_DOWN_R)
    if r is None or r[1][0] == 0:
        raise RuntimeError('41 未受理: %r' % (r,))
    t0 = time.perf_counter()
    total = 0
    finish_at = None
    dl = time.time() + 120
    while time.time() < dl:
        for ty, dat, msg in c.take(.2):
            if ty == T_DATA and struct.unpack('<Q', dat[ID_OFF:ID_OFF + 8])[0] == tid:
                total += len(msg)
            elif ty == T_FINISH and struct.unpack('<Q', dat[ID_OFF:ID_OFF + 8])[0] == tid:
                finish_at = time.perf_counter()
        if finish_at is not None:
            break
    if finish_at is None or total != SIZE:
        raise RuntimeError('下载不完整 bytes=%d/%d finish=%s' % (total, SIZE, finish_at is not None))
    return finish_at - t0, total


def main():
    if not (CERTS / 'server.crt').exists():
        raise SystemExit('FATAL: dist/certs 缺失，先运行 Server/make_tls_certs.ps1')
    ctl = server_ctl.ServerCtl(str(EXE), str(DIST))
    if not ctl.start():
        raise SystemExit('FATAL: 服务端未起来: %s' % ctl.last_error)
    try:
        # 准备：基准文件只建一次——重名会被服务端自动改名成 bench(N).bin，破坏固定名下载
        ensure_user()
        if bench_entry_exists():
            print('prepare: bench.bin 已存在，跳过上传')
        else:
            BLOB = os.urandom(SIZE)
            c = open_plain()
            if login(c) != 1:
                raise SystemExit('FATAL: tlsx 登录失败')
            c.send(pdu(T_MKDIR, msg=f'{FSROOT}/{USER}'.encode(), dat=f32('bench')))
            c.wait(T_MKDIR_R)
            tid = 7001
            c.send(pdu(T_UPINIT, msg=f'{FSROOT}/{USER}/bench'.encode(),
                       dat=f32('bench.bin') + struct.pack('<q', SIZE)
                           + struct.pack('<I', MAGIC) + b'\0' * 4 + struct.pack('<Q', tid)))
            r = c.wait(T_UPINIT_R)
            if r is None or r[1][0] == 0:
                raise SystemExit('FATAL: 29 INIT 被拒: %r' % (r,))
            done = False
            for off in range(0, SIZE, CHUNK):
                c.send(pdu(T_UPDATA, msg=BLOB[off:off + CHUNK],
                           dat=struct.pack('<q', off) + b'\0' * 32
                               + struct.pack('<I', MAGIC) + b'\0' * 4 + struct.pack('<Q', tid)))
                a = c.wait(T_UPDATA_R, 30)
                if a is None or a[1][0] == 0:
                    raise SystemExit('FATAL: 分片 %d 失败' % off)
                done = a[1][16] != 0      # caData[16]=complete：服务端已收满登记大小
            if not done:
                raise SystemExit('FATAL: 末片后 complete 未置位')
            c.close()
            print('prepare: %d MiB uploaded' % (SIZE // CHUNK))

        # 热身
        c = open_plain(); login(c); bench_download(c); c.close()
        c = open_tls(); login(c); bench_download(c); c.close()

        def timed(opener):
            ts = []
            for _ in range(3):
                cc = opener()
                login(cc)
                dt, n = bench_download(cc)
                cc.close()
                ts.append(dt)
            best = min(ts)
            return best, n

        dt_p, n = timed(open_plain)
        dt_t, n = timed(open_tls)
        sp_p, sp_t = n / dt_p / 1048576, n / dt_t / 1048576
        print('plain: best %.3fs -> %.1f MiB/s (%d bytes)' % (dt_p, sp_p, n))
        print('tls  : best %.3fs -> %.1f MiB/s (%d bytes)' % (dt_t, sp_t, n))
        print('ratio tls/plain = %.1f%% (overhead %.1f%%)' % (100 * sp_t / sp_p, 100 * (1 - sp_t / sp_p)))
    finally:
        ctl.stop()


if __name__ == '__main__':
    main()
