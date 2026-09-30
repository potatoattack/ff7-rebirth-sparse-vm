#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Optional GPU regression runner. No elevated privileges or system writes."""
from collections import Counter
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
COUNTERS = ('ff7_vm_e03_stats', 'ff7_vm_e04_stats', 'ff7_vm_e07_stats',
            'ff7_vm_e13_stats', 'ff7_vm_explicit_stats', 'ff7_vm_e24_stats',
            'ff7_vm_e27_stats')
CASES = ('exact_64k', 'split_128k', 'split_large_physical',
         'resident_to_hole', 'hole_to_resident', 'replace_4m')


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def counters():
    base = Path('/sys/module/amdgpu/parameters')
    return {name: {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', (base / name).read_text())}
            for name in COUNTERS}


def records(text, prefix):
    return [dict(re.findall(r'(\w+)=([^\s]+)', line))
            for line in text.splitlines() if line.startswith(prefix)]


def validate_logs(order, lifetime, neighbor):
    rows = records(order, 'E12_ORDER_PASS:')
    require(len(rows) == 12 and {int(x['test']) for x in rows} == set(range(12)),
            'Required-wait cases incomplete')
    require(all(x.get('old_and_new_edges') == 'checked' for x in rows), 'Old/new mapping data not checked')
    rows = records(order, 'E12_OVERLAP:')
    require(len(rows) == 16 and {int(x['round']) for x in rows} == set(range(16)),
            'Legacy exact-batch checks incomplete')
    # These old long probes already had 0/16 overlap in the accepted E28 run.
    # Require correctness/completeness here; use E22's tiny probes for overlap.
    legacy_pending = sum(x.get('graphics_still_pending') == '1' for x in rows)
    rows = records(lifetime, 'E16_LIFETIME_PASS:')
    expected = {(str(k), str(r)) for k in range(3) for r in range(12)}
    require(len(rows) == 36 and {(x['kind'], x['round']) for x in rows} == expected,
            'Lifetime cases incomplete')
    require(all(x.get('old_and_new_data') == 'checked' for x in rows), 'Lifetime readbacks incomplete')
    rows = records(neighbor, 'E22_CASE_PASS:')
    expected = {(k, str(r)) for k in CASES for r in range(12)}
    require(len(rows) == 72 and {(x['case'], x['round']) for x in rows} == expected,
            'Neighbor cases incomplete')
    for row in rows:
        for field in ('reader_started_before_bind', 'gpu_new_mapping_checked', 'full_post_checked',
                      'no_rebind_pending', 'reader_pending_after_gpu'):
            require(row.get(field) == '1', f"Neighbor {row.get('case')}: incomplete {field}")
        require(row.get('probe_bytes') == '32' and row.get('probe_regions') == '4',
                'Unexpected neighbor probe workload')
    return {'required_waits': 12, 'legacy_exact_rounds': 16,
            'legacy_graphics_pending': legacy_pending, 'lifetimes': 36,
            'neighbors': dict(Counter(row['case'] for row in rows))}


def validate_counters(before, after, counter_files):
    delta = {}
    for name in COUNTERS:
        require(before[name] and before[name].keys() == after[name].keys(), f'Missing/changed counter schema: {name}')
        delta[name] = {key: after[name][key] - before[name][key] for key in before[name]}
        require(all(n >= 0 for n in delta[name].values()), f'Counter reset: {name}')
        require(delta[name].get('errors', 0) == 0, f'Mapping errors: {name}')
    for name, field in (('ff7_vm_e04_stats', 'entries'), ('ff7_vm_e07_stats', 'cross_entries'),
                        ('ff7_vm_e13_stats', 'direct'), ('ff7_vm_explicit_stats', 'eligible'),
                        ('ff7_vm_e27_stats', 'leaf_vms'), ('ff7_vm_e27_stats', 'capped')):
        require(delta[name][field] > 0, f'Profile path was not exercised: {name}/{field}')
    require(delta['ff7_vm_e27_stats']['upper_writes'] == 0, 'Unexpected upper-level PTE write')
    guard = delta['ff7_vm_e24_stats']
    require(guard['checked'] == guard['kept'] + guard['guarded'], 'Split guard accounting mismatch')
    require(counter_files, 'No RADV cleanup counter files; verify the packaged driver was used')
    explicit_physical = explicit_virtual = 0
    for path in counter_files:
        data = path.read_bytes()
        require(len(data) == 4096, f'Wrong counter file size: {path.name}')
        values = struct.unpack_from('<12Q', data)
        require(values[0] == 0x313645374646 and values[1] == 1 and values[3] == 1,
                f'Wrong cleanup counter header/mode: {path.name}')
        require(values[10] == 0, f'Cleanup errors: {path.name}')
        require(sum(values[4:7]) == sum(values[7:10]), f'Cleanup counts do not balance: {path.name}')
        explicit_physical += values[7]
        explicit_virtual += values[8]
    require(explicit_physical > 0 and explicit_virtual > 0, 'Explicit cleanup paths were not both exercised')
    return delta


def main():
    require(os.geteuid() != 0, 'Run as your normal user, not root')
    flags = re.split(r'[,\s]+', os.environ.get('RADV_EXPERIMENTAL', ''))
    require('sparse_vm' in flags, 'Run with RADV_EXPERIMENTAL=sparse_vm')
    for name in ('lifetimes', 'neighbors'):
        require((HERE / 'build' / name).is_file(), 'First run: make -C tests self-test')
    stamp = datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S')
    out = Path(tempfile.mkdtemp(prefix=f'results-{stamp}-', dir=HERE))
    stats = out / 'cleanup'
    stats.mkdir()
    env = os.environ.copy()
    env['RADV_SPARSE_VM_STATS_DIR'] = str(stats)
    status = {'passed': False, 'kernel': os.uname().release, 'experimental': env['RADV_EXPERIMENTAL']}
    print(f'Results: {out}', flush=True)
    try:
        before = counters()
        (out / 'before.json').write_text(json.dumps(before, indent=2) + '\n')
        logs = []
        for name, binary, args in (('ordering', 'lifetimes', ['--regression']),
                                   ('lifetimes', 'lifetimes', []), ('neighbors', 'neighbors', [])):
            print(f'Running {name}; no camera movement or hotkey needed.', flush=True)
            work = out / name
            work.mkdir()
            with (out / f'{name}.log').open('w') as log:
                subprocess.run([str(HERE / 'build' / binary), *args], cwd=work, env=env,
                               stdout=log, stderr=subprocess.STDOUT, timeout=180, check=True)
            text = (out / f'{name}.log').read_text()
            require('radv: sparse_vm v1 enabled for this device VM' in text,
                    f'No successful driver negotiation recorded for {name}')
            logs.append(text)
        after = counters()
        (out / 'after.json').write_text(json.dumps(after, indent=2) + '\n')
        status['cases'] = validate_logs(*logs)
        status['deltas'] = validate_counters(before, after, sorted(stats.glob('destroy-*.bin')))
        status['passed'] = True
        print('PASS: data, required waits, path activation, cleanup and GPU overlap. Game testing remains separate.')
    except (RuntimeError, OSError, subprocess.SubprocessError, KeyError, ValueError) as error:
        status['error'] = str(error)
        print(f'Needs review: {error}. Logs retained in {out}', file=sys.stderr)
    finally:
        (out / 'result.json').write_text(json.dumps(status, indent=2) + '\n')
    return 0 if status['passed'] else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, OSError) as error:
        sys.exit(str(error))
