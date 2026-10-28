#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""REQ-CLI-MSGSTORE-001 存储层验证入口（无服务端、无 MySQL、无界面）。

做法：用与项目一致的工具链（MinGW g++ + Qt 5.14.2 头文件与库）直接编译产品源码
`Client/messagestore.cpp` 与断言探针 `store_probe.cpp`，运行探针并逐条核对结果。
探针编译的是产品实现，不是复制的等价实现。

运行：
    py -3.13 -X utf8 project-docs/evidence/REQ-CLI-MSGSTORE-001/verify.py

退出码：0 表示全部用例通过；非 0 表示编译失败、运行失败、用例失败或断言总数漂移。
"""
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]          # project-docs/evidence/<REQ> → 仓库根

GXX = 'C:/Qt/Tools/mingw730_64/bin/g++.exe'
QT = 'C:/Qt/5.14.2/mingw73_64'
PROBE_SRC = HERE / 'store_probe.cpp'
STORE_SRC = ROOT / 'Client' / 'messagestore.cpp'
STORE_HDR = ROOT / 'Client' / 'messagestore.h'
OUT_EXE = ROOT / 'build' / 'probe' / 'msgstore001_store_probe.exe'

# 探针固定产出的用例编号：即使编译失败也按固定条数计账，避免"断言总数漂移"
PROBE_IDS = [
    'S1_open_creates_schema',
    'S2_contiguous_advance',
    'S3_beyond_water_not_stored',
    'S4_hole_fill_promotes',
    'S5_duplicate_seq_idempotent',
    'S6_capacity_truncates_oldest',
    'S7_kind_separated_databases',
    'S8_user_column_isolation',
    'S9_reopen_restores_history',
    'S10_clear_keeps_water',
    'S11_clear_scoped_to_conversation',
    'S12_remove_user_purges_rows',
    'S13_direction_preserved',
    'S14_unavailable_degrades_gracefully',
    'S15_corrupt_db_degrades',
    'S16_seed_water_monotonic',
    'S17_dedup_key_skips_duplicate',
    'S18_dedup_scoped_to_conversation',
    'S19_time_text_roundtrip',
    'S20_hole_blocks_promotion',
    'S21_contiguous_run_drained',
    'S22_partial_run_stops_at_hole',
    'S23_zero_seq_rejected_and_idempotent',
    'S24_cap_drops_oldest',
    'S25_unstorable_hole_blocks_then_resumes',
    'S26_advance_water_single_step_only',
]

LINE = re.compile(r'^\[(PASS|FAIL)\] (\S+) :: (.*)$')


def fail_hard(reason, tail=''):
    print(f'RESULT FAIL :: {reason}')
    if tail:
        print(tail[-4000:])
    return 1


def build():
    missing = [str(p) for p in (PROBE_SRC, STORE_SRC, STORE_HDR) if not p.exists()]
    if missing:
        return f'缺少源码文件: {", ".join(missing)}'
    if not Path(GXX).exists():
        return f'编译器不存在: {GXX}'
    OUT_EXE.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        GXX, '-std=gnu++11', '-Wall', '-Wextra', '-fexceptions', '-mthreads',
        '-DUNICODE', '-D_UNICODE', '-DWIN32', '-DMINGW_HAS_SECURE_API=1',
        '-DQT_NO_DEBUG', '-DQT_SQL_LIB', '-DQT_CORE_LIB',
        '-I', str(ROOT),
        '-I', f'{QT}/include', '-I', f'{QT}/include/QtCore', '-I', f'{QT}/include/QtSql',
        '-I', f'{QT}/mkspecs/win32-g++',
        str(PROBE_SRC), str(STORE_SRC),
        '-L', f'{QT}/lib', '-lQt5Sql', '-lQt5Core',
        '-o', str(OUT_EXE),
    ]
    ran = subprocess.run(cmd, capture_output=True, text=True)
    if ran.returncode != 0:
        return '探针编译失败\n' + (ran.stdout or '') + (ran.stderr or '')
    return None


def run():
    import os
    env = dict(os.environ)
    env['PATH'] = f'{QT}/bin;' + env.get('PATH', '')
    return subprocess.run([str(OUT_EXE)], capture_output=True, text=True, env=env)


def main():
    err = build()
    if err:
        return fail_hard(err.splitlines()[0], err)

    ran = run()
    out = (ran.stdout or '') + (ran.stderr or '')
    seen = {}
    for line in out.splitlines():
        m = LINE.match(line.strip())
        if m:
            seen[m.group(2)] = (m.group(1) == 'PASS', m.group(3))

    if not seen:
        return fail_hard(f'探针没有产出任何断言行（退出码 {ran.returncode}）', out)

    print('--- 存储层断言 ---')
    bad = []
    for sid in PROBE_IDS:
        if sid not in seen:
            bad.append(f'{sid} 缺失（断言总数漂移）')
            print(f'[FAIL] {sid} :: 探针未产出该用例')
            continue
        ok, detail = seen[sid]
        print(f"[{'PASS' if ok else 'FAIL'}] {sid} :: {detail}")
        if not ok:
            bad.append(f'{sid} :: {detail}')

    extra = [sid for sid in seen if sid not in PROBE_IDS]
    for sid in extra:
        ok, detail = seen[sid]
        print(f"[{'PASS' if ok else 'FAIL'}] {sid} :: {detail}（未在登记清单内）")
        if not ok:
            bad.append(f'{sid} :: {detail}')
        else:
            bad.append(f'{sid} 未登记，请把编号加进 PROBE_IDS')

    # 退出码非 0 但所有行都 PASS 时同样判失败：不允许把崩溃当成通过
    if ran.returncode != 0:
        bad.append(f'探针退出码 {ran.returncode}')

    if bad:
        print(f'\nRESULT FAIL {len(bad)} 项：')
        for b in bad:
            print('  - ' + b)
        if ran.returncode != 0:
            print('--- 探针原始输出尾部 ---')
            print(out[-2000:])
        return 1

    print(f'\nRESULT PASS {len(PROBE_IDS)}/{len(PROBE_IDS)} 存储层用例通过')
    return 0


if __name__ == '__main__':
    sys.exit(main())
