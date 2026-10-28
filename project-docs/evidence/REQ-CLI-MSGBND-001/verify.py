#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""REQ-CLI-MSGBND-001 私聊正文边界验证入口（无服务端、无 MySQL、无界面）。

做法与 REQ-CLI-MSGSTORE-001 一致：用项目工具链（MinGW g++ + Qt 5.14.2 头/库）直接编译
产品实现 `Server/protocol.cpp` 与断言探针 `boundary_probe.cpp`，探针编译的是被测函数本体。
内存池 `MemAlloc` 由探针内的最小 stub 顶替（探针不分配 PDU），不影响被测代码。

另加两项两端一致性检查（协议头是兼容边界，只改一端就是事故）：
  C1  两端 protocol.h 的 CHAT_MAX_BODY_BYTES / CHAT_MSG_BODY_OFFSET 数值必须相同
  C2  两端 protocol.cpp 的本需求实现区块必须逐字相同

真实服务端行为（畸形报文是否打爆进程、超限是否既不入库也不转发）由同目录
boundary_e2e.py 覆盖，本入口不替代它。

运行：
    py -3.13 -X utf8 project-docs/evidence/REQ-CLI-MSGBND-001/verify.py

退出码：0 全部通过；非 0 表示编译失败、运行失败、用例失败或断言总数漂移。
"""
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]

GXX = 'C:/Qt/Tools/mingw730_64/bin/g++.exe'
QT = 'C:/Qt/5.14.2/mingw73_64'
PROBE_SRC = HERE / 'boundary_probe.cpp'
PROTO_SRC = ROOT / 'Server' / 'protocol.cpp'
PROTO_HDR = ROOT / 'Server' / 'protocol.h'
CLI_HDR = ROOT / 'Client' / 'protocol.h'
CLI_SRC = ROOT / 'Client' / 'protocol.cpp'
OUT_EXE = ROOT / 'build' / 'probe' / 'msgbnd001_probe.exe'

MARKER = '// ===== REQ-CLI-MSGBND-001'

PROBE_IDS = [
    'P1_ascii_untouched',
    'P2_complete_cjk_untouched',
    'P3_half_codepoint_trimmed',
    'P4_emoji_complete_untouched',
    'P5_emoji_truncated_to_1_drops_all',
    'P5_emoji_truncated_to_2_drops_all',
    'P5_emoji_truncated_to_3_drops_all',
    'P6_only_continuation_bytes_yield_empty',
    'P7_invalid_lead_dropped',
    'P8_valid_prefix_kept',
    'P9_tail_half_dropped_prefix_kept',
    'P10_normal_roundtrip',
    'P11_declared_overrun_clamped_to_available',
    'P12_short_buffer_rejected',
    'P13_null_pointer_rejected',
    'P14_clamped_result_is_valid_utf8',
    'P15_time_full_30_bytes_no_terminator',
    'P16_time_stops_at_first_nul',
    'P17_exactly_2000_accepted',
    'P18_2001_rejected_with_length_reason',
    'P19_empty_body_rejected',
    'P20_zero_msglen_rejected',
    'P21_declared_overrun_rejected',
    'P22_record_header_missing_rejected',
    'P23_null_rejected',
    'P24_reason_holds_no_plaintext',
    'P25_two_records_stride_matches',
    'P26_records_extracted_in_order',
    'P27_zero_total_len_stops',
    'P28_record_beyond_payload_stops',
    'P29_msglen_gt_totallen_stops',
    'P30_less_than_one_header_stops',
]

LINE = re.compile(r'^\[(PASS|FAIL)\] (\S+) :: (.*)$')
LINE_NO_DETAIL = re.compile(r'^\[(PASS|FAIL)\] (\S+)$')
CONST = re.compile(r'static const uint (CHAT_MAX_BODY_BYTES|CHAT_MSG_BODY_OFFSET)\s*=\s*(\d+)')


def fail_hard(reason, tail=''):
    print(f'RESULT FAIL :: {reason}')
    if tail:
        print(tail[-4000:])
    return 1


def build():
    missing = [str(p) for p in (PROBE_SRC, PROTO_SRC, PROTO_HDR) if not p.exists()]
    if missing:
        return f'缺少源码文件: {", ".join(missing)}'
    if not Path(GXX).exists():
        return f'编译器不存在: {GXX}'
    OUT_EXE.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        GXX, '-std=gnu++11', '-Wall', '-Wextra', '-fexceptions', '-mthreads',
        '-DUNICODE', '-D_UNICODE', '-DWIN32', '-DQT_NO_DEBUG', '-DQT_CORE_LIB',
        '-I', str(ROOT),
        '-I', f'{QT}/include', '-I', f'{QT}/include/QtCore',
        '-I', f'{QT}/mkspecs/win32-g++',
        str(PROBE_SRC), str(PROTO_SRC),
        '-L', f'{QT}/lib', '-lQt5Core',
        '-o', str(OUT_EXE),
    ]
    ran = subprocess.run(cmd, capture_output=True)
    if ran.returncode != 0:
        return '探针编译失败\n' + (ran.stdout + ran.stderr).decode('utf-8', 'replace')
    return None


def run():
    import os
    env = dict(os.environ)
    env['PATH'] = f'{QT}/bin;' + env.get('PATH', '')
    return subprocess.run([str(OUT_EXE)], capture_output=True, env=env)


def block_of(path):
    """取本需求的实现/声明区块（到文件尾），换行归一，用于两端比对。"""
    t = path.read_text(encoding='utf-8', newline='').replace('\r\n', '\n')
    if MARKER not in t:
        return None
    return t[t.index(MARKER):]


def consistency_checks(seen):
    """C1 常量同值、C2 实现区块逐字相同。结果按同一格式并入计账。"""
    srv_h, cli_h = CONST.findall(PROTO_HDR.read_text(encoding='utf-8')), \
        CONST.findall(CLI_HDR.read_text(encoding='utf-8'))
    check('C1_both_ends_same_constants', srv_h == cli_h and len(srv_h) == 2,
          f'server={srv_h} client={cli_h}')

    sb, cb = block_of(PROTO_SRC), block_of(CLI_SRC)
    check('C2_both_ends_identical_block',
          sb is not None and cb is not None and sb == cb,
          'server/client 区块缺失' if sb is None or cb is None
          else ('一致' if sb == cb else '两端实现区块不一致'))


def check(id_, ok, detail=''):
    RESULTS.append((id_, bool(ok)))
    print(('[PASS] ' if ok else '[FAIL] ') + id_ + (f' :: {detail}' if detail else ''))
    sys.stdout.flush()


RESULTS = []


def main():
    err = build()
    if err:
        return fail_hard(err.splitlines()[0], err)

    ran = run()
    out = (ran.stdout or b'') .decode('utf-8', 'replace') + (ran.stderr or b'').decode('utf-8', 'replace')
    seen = {}
    for line in out.splitlines():
        m = LINE.match(line.strip()) or LINE_NO_DETAIL.match(line.strip())
        if m:
            seen[m.group(2)] = (m.group(1) == 'PASS', m.group(3) if m.lastindex and m.lastindex > 2 else '')

    if not seen:
        return fail_hard(f'探针没有产出任何断言行（退出码 {ran.returncode}）', out)

    print('--- 正文边界断言（产品实现 Server/protocol.cpp） ---')
    bad = []
    for pid in PROBE_IDS:
        if pid not in seen:
            bad.append(f'{pid} 缺失（断言总数漂移）')
            print(f'[FAIL] {pid} :: 探针未产出该用例')
            continue
        ok, detail = seen[pid]
        print(f"[{'PASS' if ok else 'FAIL'}] {pid} :: {detail}")
        if not ok:
            bad.append(f'{pid} :: {detail}')

    extra = [pid for pid in seen if pid not in PROBE_IDS]
    for pid in extra:
        ok, detail = seen[pid]
        print(f"[{'PASS' if ok else 'FAIL'}] {pid} :: {detail}（未在登记清单内）")
        bad.append(f'{pid} 未登记或失败，请核对后加进 PROBE_IDS')

    print('--- 两端一致性 ---')
    consistency_checks(seen)

    if ran.returncode != 0:
        bad.append(f'探针退出码 {ran.returncode}')

    allbad = bad + [n for n, ok in RESULTS if not ok]
    if allbad:
        print(f'\nRESULT FAIL {len(allbad)} 项：')
        for b in allbad:
            print('  - ' + b)
        if ran.returncode != 0:
            print('--- 探针原始输出尾部 ---')
            print(out[-2000:])
        return 1

    total = len(PROBE_IDS) + len(RESULTS)
    print(f'\nRESULT PASS {total}/{total}（探针 {len(PROBE_IDS)} 项 + 一致性 {len(RESULTS)} 项）')
    return 0


if __name__ == '__main__':
    sys.exit(main())
