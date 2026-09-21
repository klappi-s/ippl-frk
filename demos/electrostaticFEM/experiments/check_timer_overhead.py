#!/usr/bin/env python3
"""Interleave coarse/detailed replay pairs to assess instrumentation overhead."""
import argparse
import csv
import json
from pathlib import Path
import subprocess
import sys
import numpy as np

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--executable', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--pairs', type=int, default=6)
a = p.parse_args()
a.output = a.output.resolve()
a.output.mkdir(parents=True, exist_ok=False)
runner = Path(__file__).with_name('run_reuse.py')
methods = ['plain', 'jacobi', 'gauss-seidel', 'chebyshev']
records = []
for pair in range(a.pairs):
    rotated = methods[pair % 4:] + methods[:pair % 4]
    levels = ['coarse', 'detailed'] if pair % 2 == 0 else ['detailed', 'coarse']
    for level in levels:
        name = f'pair{pair}_{level}'
        command = [sys.executable, str(runner), '--executable', str(a.executable),
                   '--output', str(a.output / name), '--orders', '3', '--reuse', 'full',
                   '--repeats', '1', '--timer-level', level, '--methods', *rotated]
        print(name, flush=True)
        with (a.output / f'{name}.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        rows = list(csv.DictReader((a.output / name / 'p3_full_r0/solves-rank0.csv').open()))
        for method in methods:
            selected = [r for r in rows if r['phase'] == 'replay' and r['method'] == method]
            assert len(selected) == 11 and all(r['success'] == '1' for r in selected)
            records.append(dict(pair=pair, level=level, method=method,
                                seconds=sum(float(r['solve_seconds']) for r in selected),
                                iterations=sum(int(r['iterations']) for r in selected)))
        (a.output / 'results.json').write_text(json.dumps(records, indent=2) + '\n')

report = ['# Detailed timer overhead check', '',
          f'{a.pairs} alternating coarse/detailed pairs; P3 GLL, 128 cells, 32768 particles, '
          '11 replay solves, full reuse, one Serial MPI rank. Method order rotates between pairs.',
          '', '| Method | Coarse seconds (median) | Detailed seconds (median) | '
          'Paired change (median) | Paired change range |', '|---|---:|---:|---:|---:|']
for method in methods:
    coarse = [r for r in records if r['method'] == method and r['level'] == 'coarse']
    detailed = [r for r in records if r['method'] == method and r['level'] == 'detailed']
    assert [r['iterations'] for r in coarse] == [r['iterations'] for r in detailed]
    changes = [100 * (d['seconds'] / c['seconds'] - 1) for c, d in zip(coarse, detailed)]
    report.append(f"| {method} | {np.median([r['seconds'] for r in coarse]):.6g} | "
                  f"{np.median([r['seconds'] for r in detailed]):.6g} | "
                  f"{np.median(changes):+.1f}% | {min(changes):+.1f}% to {max(changes):+.1f}% |")
report += ['', 'These are elapsed-time differences, including run-to-run variation. A negative '
           'difference does not mean instrumentation improves performance. Use coarse timers for '
           'method ranking and detailed timers to attribute costs; retain the observed spread.']
(a.output / 'README.md').write_text('\n'.join(report) + '\n')
