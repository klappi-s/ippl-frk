#!/usr/bin/env python3
"""Check matched summary/raw demo runs using only the Python standard library."""
import argparse
import csv
import json
import math
from pathlib import Path


def read_rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def check(summary, raw, ranks, limit):
    assert not (summary / 'timers.csv').exists(), 'Summary mode exported raw events'
    counts = []
    configurations = []
    for directory, recording in [(summary, 'summary'), (raw, 'raw')]:
        config = dict(line.split('=', 1) for line in (directory / 'configuration.txt').read_text().splitlines() if '=' in line)
        configurations.append({key: value for key, value in config.items()
                               if key not in ('timer_recording', 'timer_sample_limit',
                                              'option.timer_recording', 'option.timer_sample_limit',
                                              'option.output')})
        assert config['timer_recording'] == recording
        assert int(config['ranks']) == ranks
        assert int(config['timer_sample_limit']) == (0 if recording == 'summary' else limit)
        table = read_rows(directory / 'timer-summary.csv')
        assert {int(row['rank']) for row in table} == set(range(ranks))
        assert len({(row['timer_name'], row['rank']) for row in table}) == len(table)
        counts.append({(row['timer_name'], row['rank']): int(row['count']) for row in table})
        for row in table:
            count, stored = int(row['count']), int(row['stored_samples'])
            assert stored == (0 if recording == 'summary' else min(count, limit))
            numbers = [float(row[key]) for key in ['total_seconds', 'min_seconds', 'max_seconds', 'mean_seconds', 'stddev_seconds']]
            assert all(math.isfinite(value) and value >= 0 for value in numbers)
            assert float(row['min_seconds']) <= float(row['mean_seconds']) + 1e-15
            assert float(row['mean_seconds']) <= float(row['max_seconds']) + 1e-15
        for rank in range(ranks):
            rows = read_rows(directory / ('solves-rank%d.csv' % rank))
            assert rows and all(row['success'] == '1' for row in rows)
            assert all(0 <= float(row['fresh_relative_residual']) <= max(20 * float(config['tolerance']), 5e-11) for row in rows)
            # Each recorded solve also makes one uncounted evaluateAx call for
            # the independent residual check. Verify capture against the work
            # done in THIS process, rather than another floating-point trajectory.
            expected = sum(int(row['operator_calls']) for row in rows) + len(rows)
            for timer in ('evaluateAx', 'evaluateAx: outer loop'):
                assert counts[-1][(timer, str(rank))] == expected, 'Operator timer count disagrees with solve statistics'
    assert configurations[0] == configurations[1], 'Simulation configurations differ'
    assert counts[0].keys() == counts[1].keys(), 'Timer registry differs'
    for key in counts[0]:
        if key[0].startswith('electrostaticFEM.') or key[0] in ('findInternal', 'findPeriodic'):
            assert counts[0][key] == counts[1][key], 'Stage/layout counts differ: ' + str(key)
    raw_rows = read_rows(raw / 'timers.csv')
    samples = {}
    for row in raw_rows:
        key = (row['timer_name'], row['rank'])
        ids = samples.setdefault(key, [])
        assert int(row['measurement_id']) == len(ids) and len(ids) < limit
        ids.append(int(row['measurement_id']))
    assert all(len(samples.get(key, [])) == min(count, limit) for key, count in counts[1].items())
    iteration_differences = []
    for rank in range(ranks):
        left = read_rows(summary / ('solves-rank%d.csv' % rank))
        right = read_rows(raw / ('solves-rank%d.csv' % rank))
        assert len(left) == len(right)
        for a, b in zip(left, right):
            for key in ['phase', 'method', 'solve', 'operator_setups', 'preconditioner_setups']:
                assert a[key] == b[key], key
            assert math.isclose(float(a['rhs_norm']), float(b['rhs_norm']), rel_tol=2e-11, abs_tol=1e-13)
            # GPU atomic assembly is not bitwise reproducible between launches.
            # A different last CG iteration is diagnostic data, not proof that
            # the timer policy changed the equation. Residual gates stay unchanged.
            if a['iterations'] != b['iterations']:
                iteration_differences.append(dict(rank=rank, phase=a['phase'], method=a['method'],
                                                   solve=int(a['solve']), summary=int(a['iterations']),
                                                   raw=int(b['iterations'])))
    left, right = [read_rows(directory / 'physics.csv') for directory in (summary, raw)]
    assert left and len(left) == len(right), 'Missing/incomplete physics history'
    for a, b in zip(left, right):
        assert a.keys() == b.keys() and a['step'] == b['step']
        for key in a:
            x, y = float(a[key]), float(b[key])
            assert math.isfinite(x) and math.isfinite(y)
            assert math.isclose(x, y, rel_tol=max(20 * float(config['tolerance']), 5e-11),
                                abs_tol=1e-12), 'Physics histories differ: ' + key
    return dict(ranks=ranks, raw_rows=len(raw_rows), summary_rows=len(counts[0]),
                summary_bytes=(summary / 'timer-summary.csv').stat().st_size,
                raw_bytes=(raw / 'timers.csv').stat().st_size,
                iteration_differences=iteration_differences,
                timer_count_differences={str(key): [counts[0][key], counts[1][key]]
                                         for key in counts[0] if counts[0][key] != counts[1][key]},
                neighbor_calls={str(key): count for key, count in counts[0].items() if key[0] in ('findInternal', 'findPeriodic')})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--summary', type=Path, required=True)
    parser.add_argument('--raw', type=Path, required=True)
    parser.add_argument('--ranks', type=int, required=True)
    parser.add_argument('--sample-limit', type=int, default=4)
    args = parser.parse_args()
    print(json.dumps(check(args.summary, args.raw, args.ranks, args.sample_limit), indent=2))
