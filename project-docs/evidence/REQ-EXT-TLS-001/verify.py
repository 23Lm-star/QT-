# -*- coding: utf-8 -*-
"""REQ-EXT-TLS-001 TLS 三 / TLS 五 验证入口（无网络依赖的本地 TLS 探针）。

结构：
  - Python 侧起三个临时 TLS 桩服务端（127.0.0.1 随机端口）：
      ok     —— 有效自签证书（CN=tlsserver, SAN=IP:127.0.0.1）
      rogue  —— 另一张自签证书（客户端只 pin ok 的证书 → 必被拒）
      expired—— 已过期证书（且客户端 pin 的就是这张 → 唯一拒因是过期）
    桩服务端只做 TLS 握手后回一字节，不含业务。
  - C++ 探针（编译产品同一份 Client/tlspinning.cpp + 客户端同款 TLS 序列）按场景连接，
    断言 encrypted / sslErrors 的实际结果：
      T3-a 正常证书握手成功
      T3-b 冒名证书被拒且不 encrypted
      T3-c 主机名不符被拒（HostNameMismatch）
      T3-d 过期证书被拒（CertificateExpired）
      T5-a TLS 层断连后状态收敛（探针 abort 自身 socket 后回报）
      T5-b 断连后桩服务端仍可完成新握手
  - 规则形态与写出闸门（T3-e/T3-f）由 C++ 直接调产品实现断言。

不占 5000、不需要 MySQL；与真实 Server.exe 的对接（裸 PDU 拒绝、证书缺失降级、
应答逐字节一致）由 tls_e2e.py 覆盖。

用法：
    py -3.13 -X utf8 project-docs/evidence/REQ-EXT-TLS-001/verify.py
"""
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
QT_BIN = Path(r'C:/Qt/5.14.2/mingw73_64')
MINGW = Path(r'C:/Qt/Tools/mingw730_64')
OPENSSL = Path(r'C:/Program Files/Git/mingw64/bin/openssl.exe')

EXPECTED = ['T3-e-rule-shape', 'T3-e-single-anchor', 'T3-f-write-gate',
            'T6-a-tlsport-derivation', 'T6-b-kicknotice-gate',
            'T3-a-happy', 'T3-b-rogue-rejected', 'T3-c-name-mismatch',
            'T3-d-expired-rejected', 'T5-a-abort-converges', 'T5-b-server-alive-after-abort',
            'X1-stub-resumes-after-abort']

PASS_TAGS = {'T3-b-rogue-rejected': 'REJECTED', 'T3-c-name-mismatch': 'REJECTED',
             'T3-d-expired-rejected': 'REJECTED'}


def die(msg):
    print('FATAL: ' + msg)
    sys.exit(2)


def gen_certs(d):
    if not OPENSSL.exists():
        die('未找到 openssl：%s' % OPENSSL)
    specs = [
        ('server', '/CN=tlsserver', None, None),
        ('rogue', '/CN=tlsserver', None, None),        # 名字相同但密钥不同：只考察信任锚
        ('expired', '/CN=tlsserver', '20230101000000Z', '20240101000000Z'),
    ]
    for name, subj, nb, na in specs:
        crt, key = d / ('%s.crt' % name), d / ('%s.key' % name)
        cmd = [str(OPENSSL), 'req', '-x509', '-newkey', 'rsa:2048',
               '-keyout', str(key), '-out', str(crt), '-days', '825', '-nodes',
               '-subj', subj, '-addext', 'subjectAltName=IP:127.0.0.1']
        if nb:
            cmd += ['-not_before', nb, '-not_after', na]
        env = dict(os.environ, MSYS_NO_PATHCONV='1')
        subprocess.run(cmd, check=True, env=env, capture_output=True)


def build(env_root):
    gxx = MINGW / 'bin' / 'g++.exe'
    if not gxx.exists():
        die('g++ 未找到：%s（本入口需要 MinGW 工具链）' % gxx)
    if not (QT_BIN / 'lib' / 'libQt5Network.a').exists():
        die('Qt 未安装在 %s' % QT_BIN)
    inc = QT_BIN / 'include'
    out = env_root / 'pinning_probe.exe'
    cmd = [str(gxx), '-O2', '-std=gnu++11', '-fno-keep-inline-dllexport',
           '-DUNICODE', '-D_UNICODE', '-DWIN32', '-DMINGW_HAS_SECURE_API=1', '-DQT_NO_DEBUG',
           '-DQT_NETWORK_LIB', '-DQT_CORE_LIB',
           '-I', str(inc), '-I', str(inc / 'QtNetwork'), '-I', str(inc / 'QtCore'),
           '-I', str(ROOT / 'Client'),
           '-I', str(QT_BIN / 'mkspecs' / 'win32-g++'),
           str(HERE / 'pinning_probe.cpp'), str(ROOT / 'Client' / 'tlspinning.cpp'),
           '-o', str(out),
           str(QT_BIN / 'lib' / 'libQt5Network.a'), str(QT_BIN / 'lib' / 'libQt5Core.a'),
           '-Wl,-subsystem,console']
    env = dict(os.environ,
               PATH=str(QT_BIN / 'bin') + os.pathsep + str(MINGW / 'bin') + os.pathsep + os.environ['PATH'])
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:], r.stderr[-4000:])
        die('探针编译失败（退出码 %d）' % r.returncode)
    return out, env


class StubTls(threading.Thread):
    """一次性的 TLS 桩：accept 固定次数后结束。counter 记录完成的握手数。"""

    def __init__(self, crt, key, count=2):
        super().__init__(daemon=True)
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(str(crt), str(key))
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(2)
        self.port = self.listener.getsockname()[1]
        self.count = count
        self.handshakes = 0
        self.done = threading.Event()

    def run(self):
        for _ in range(self.count):
            try:
                raw, _ = self.listener.accept()
            except OSError:
                break
            try:
                s = self.ctx.wrap_socket(raw, server_side=True)
                self.handshakes += 1
                try:
                    s.sendall(b'srv-ok')
                except OSError:
                    pass
                time.sleep(0.2)
                s.close()
            except Exception:
                pass
            finally:
                try:
                    raw.close()
                except OSError:
                    pass
        self.listener.close()
        self.done.set()


def work_dir():
    """工作目录必须是纯 ASCII 路径。

    实测坑：本机 temp 目录含中文用户名（C:/Users/王晨扬/AppData/Local/Temp），
    路径经 getenv 传给 MinGW 程序时是 GBK 字节，Qt 按 UTF-8 解释后 QFile::open
    报「系统找不到指定的路径」——证书与私钥全部读成 0 字节，握手因此失败。
    产品代码不受影响（证书走 applicationDirPath()/certs，Windows 宽字符 API 取值）。
    """
    d = ROOT / 'build' / 'tls001-work'
    if not str(d).isascii():
        die('工作目录必须纯 ASCII，当前 %s' % d)
    d.mkdir(parents=True, exist_ok=True)
    for f in d.glob('*'):
        if f.is_file():
            f.unlink()
    return d


def main():
    if not (HERE / 'pinning_probe.cpp').exists():
        die('缺少 pinning_probe.cpp')
    work = work_dir()
    try:
        gen_certs(work)
        exe, env = build(work)
        ok = StubTls(work / 'server.crt', work / 'server.key', count=4); ok.start()
        rog = StubTls(work / 'rogue.crt', work / 'rogue.key', count=2); rog.start()
        exp = StubTls(work / 'expired.crt', work / 'expired.key', count=2); exp.start()
        time.sleep(.4)
        probe_env = dict(env,
                         TLS_OK_PORT=str(ok.port), TLS_ROG_PORT=str(rog.port),
                         TLS_EXP_PORT=str(exp.port),
                         TLS_SRV_CRT=str(work / 'server.crt'),
                         TLS_EXP_CRT=str(work / 'expired.crt'))
        p = subprocess.run([str(exe)], env=probe_env, capture_output=True, text=True, timeout=120)
        out = p.stdout or ''
        print(out.rstrip())
        lines = [l for l in out.splitlines() if l.startswith('[PASS]') or l.startswith('[FAIL]')]
        seen = {}
        for l in lines:
            tag = l[7:].split(' ', 1)[0]
            seen[tag] = l.startswith('[PASS]')
        missing = [t for t in EXPECTED if t not in seen]
        failed = [t for t, o in seen.items() if not o]
        # 桩侧事实打印：TLS 1.2 服务端先于客户端校验完成本侧握手，
        # 因此 rogue/expired 计数为 1 属预期，不作为判据（客户端已拒用该会话）。
        print('STUB-HANDSHAKES ok=%d rogue=%d expired=%d（rogue/expired=1 为 TLS1.2 预期，不判定）'
              % (ok.handshakes, rog.handshakes, exp.handshakes))
        # 成立的那条交叉判据：被 abort 的连接之后，服务端仍能完成新的正常握手。
        seen['X1-stub-resumes-after-abort'] = ok.handshakes >= 3
        print('%s X1-stub-resumes-after-abort :: ok 桩完成握手=%d（>=3：T3-a/T3-c/T5-a/T5-b）'
              % ('[PASS]' if seen['X1-stub-resumes-after-abort'] else '[FAIL]', ok.handshakes))
        ok.done.wait(timeout=5)
        missing = [t for t in EXPECTED if t not in seen]
        failed = [t for t, o in seen.items() if not o]
        verdict = p.returncode == 0 and not failed and not missing
        print('RESULT %s（断言 %d 项，缺失 %d 项）' % ('PASS' if verdict else 'FAIL', len(seen), len(missing)))
        if missing:
            print('未执行: ' + ', '.join(missing))
        if failed:
            print('失败: ' + ', '.join(failed))
        sys.exit(0 if verdict else 1)
    finally:
        # 保留 build/tls001-work 目录本身（Git 已忽略 build/），只清文件，避免中文 temp 复发
        for f in work.glob('*'):
            if f.is_file():
                try:
                    f.unlink()
                except OSError:
                    pass


if __name__ == '__main__':
    main()
