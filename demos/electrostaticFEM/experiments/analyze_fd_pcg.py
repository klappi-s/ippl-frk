#!/usr/bin/env python3
"""Validate FD-PCG campaign output, summarize paired controls, and shortlist variants."""

import argparse
import csv
import json
import math
from pathlib import Path
import statistics

from fd_pcg_variants import VARIANTS
from ordinary_measurements import inspect_run


def write_csv(path, rows):
    if not rows:
        return
    fields = list(dict.fromkeys(key for row in rows for key in row))
    with path.open('w') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def location(root, manifest):
    parts = manifest.relative_to(root).parts
    if len(parts) >= 4:
        return parts[0], parts[1], parts[2]
    return 'unknown', 'unknown', manifest.parent.name


def analyze(root, output):
    root, output = Path(root).resolve(), Path(output).resolve()
    records, missing = [], []
    # Allocation evidence contains a separate small CUDA preflight campaign.
    # It is not part of the requested study and intentionally has no recorded
    # node/GPU placement, so only consume top-level planned campaign manifests.
    manifests = [
        path for path in sorted(root.rglob('campaign.json'))
        if 'allocations' not in path.relative_to(root).parts
    ]
    for manifest_path in manifests:
        manifest = json.loads(manifest_path.read_text())
        if manifest.get('schema') != 2 or manifest['config']['application'] != 'alpine':
            continue
        config = manifest['config']
        phase, bundle, anchor = location(root, manifest_path)
        placement = '%dx%d' % (config['nodes'], config['gpus_per_node'])
        for job in manifest['jobs']:
            state_path = manifest_path.parent/'jobs'/(job['name'] + '.json')
            state = json.loads(state_path.read_text()) if state_path.exists() else {}
            if state.get('returncode') != 0 or state.get('validation_error'):
                missing.append(dict(
                    phase=phase, bundle=bundle, anchor=anchor, placement=placement,
                    job=job['name'],
                    reason=state.get('validation_error') or str(state.get('returncode', 'unrecorded'))))
                continue
            measurement = inspect_run(manifest_path.parent/state['directory'], job, config)
            records.append(dict(
                phase=phase, bundle=bundle, anchor=anchor, placement=placement,
                ranks=config['ranks'], cells=job['cells'], particles=job['particles'],
                variant=job['variant'], family=VARIANTS[job['variant']]['family'],
                repeat=job['repeat'], allocation=state.get('allocation', 'unknown'),
                directory=str(manifest_path.parent/state['directory']),
                application_wall_seconds=state.get('wall_seconds'), **measurement))

    if not manifests:
        raise SystemExit('No campaign manifests below ' + str(root))

    for row in records:
        controls = [
            candidate for candidate in records
            if candidate['phase'] == row['phase'] and candidate['bundle'] == row['bundle']
            and candidate['anchor'] == row['anchor'] and candidate['placement'] == row['placement']
            and candidate['cells'] == row['cells'] and candidate['repeat'] == row['repeat']
            and candidate['allocation'] == row['allocation'] and candidate['variant'] == 'cg'
        ]
        if controls:
            control = controls[0]
            row['paired_solve_speedup'] = (
                control['recurring_solve_seconds'] / row['recurring_solve_seconds'])
            row['paired_step_speedup'] = (
                control['recurring_step_seconds'] / row['recurring_step_seconds'])
            steady_gain = (control['recurring_solve_seconds']
                           - row['recurring_solve_seconds'])
            control_one_time = (control['setup_seconds'] + control['initial_solve_seconds']
                                - control['recurring_solve_seconds'])
            method_one_time = (row['setup_seconds'] + row['initial_solve_seconds']
                               - row['recurring_solve_seconds'])
            extra = max(0.0, method_one_time - control_one_time)
            row['break_even_recurring_solves'] = (
                math.ceil(extra / steady_gain) if steady_gain > 0 else None)
        else:
            row['paired_solve_speedup'] = None
            row['paired_step_speedup'] = None
            row['break_even_recurring_solves'] = None

    group_keys = ('phase', 'bundle', 'anchor', 'placement', 'ranks', 'cells',
                  'particles', 'variant', 'family')
    metrics = (
        'recurring_solve_seconds', 'recurring_step_seconds', 'recurring_iterations',
        'initial_solve_seconds', 'setup_seconds', 'all_solve_seconds',
        'max_fresh_residual', 'application_wall_seconds', 'sampled_device_peak_gib',
        'host_process_peak_gib', 'paired_solve_speedup', 'paired_step_speedup',
        'break_even_recurring_solves',
    )
    means = []
    identities = sorted({tuple(row[key] for key in group_keys) for row in records})
    for identity in identities:
        group = [row for row in records
                 if tuple(row[key] for key in group_keys) == identity]
        summary = dict(zip(group_keys, identity))
        summary['n'] = len(group)
        for metric in metrics:
            values = [row[metric] for row in group if row.get(metric) is not None]
            summary[metric] = statistics.mean(values) if values else None
            summary[metric + '_min'] = min(values) if values else None
            summary[metric + '_max'] = max(values) if values else None
        means.append(summary)

    family_choices = {}
    tuning = [row for row in means if row['phase'] == 'tune' and row['variant'] != 'cg']
    for family in sorted({row['family'] for row in tuning}):
        candidates = [row for row in tuning if row['family'] == family]
        scores = {}
        for row in candidates:
            if row['paired_solve_speedup'] is not None:
                scores.setdefault(row['variant'], []).append(row['paired_solve_speedup'])
        # Parameter discovery is allowed to test every family, but mesh/placement
        # transfer should not automatically carry a family whose best measured
        # setting is slower than its paired CG controls.  Jacobi can still pass
        # this filter as the structural overhead control when its mean is at
        # least neutral.
        anchors = {(row['cells'], row['placement']) for row in candidates}
        competitive = {
            name for name, values in scores.items()
            if statistics.mean(values) >= 1.0
            and {(row['cells'], row['placement']) for row in candidates
                 if row['variant'] == name} == anchors
        }
        if not competitive:
            continue
        winners = []
        for anchor in sorted({(row['cells'], row['placement']) for row in candidates}):
            group = [row for row in candidates
                     if (row['cells'], row['placement']) == anchor
                     and row['variant'] in competitive]
            if group:
                winners.append(min(group, key=lambda row: row['recurring_solve_seconds']))
        ranked = sorted(competitive,
                        key=lambda name: statistics.mean(scores[name]), reverse=True)
        ordered = []
        for row in winners:
            if row['variant'] not in ordered:
                ordered.append(row['variant'])
        for name in ranked:
            if name not in ordered:
                ordered.append(name)
        family_choices[family] = ordered[:2]

    selected = ['cg']
    for family in sorted(family_choices):
        selected.extend(name for name in family_choices[family] if name not in selected)
    selection = dict(
        schema=1,
        source_root=str(root),
        metric='mean recurring solve time with paired CG controls',
        variants=selected,
        families=family_choices,
        provisional=any(row['n'] < 3 for row in tuning),
        minimum_successful_repeats=min((row['n'] for row in tuning), default=0),
        required_repeats=3,
        rule=('require success at every tuned family anchor and exclude variants with mean '
              'paired solve speedup below 1.0; then retain the fastest competitive point '
              'at each tuned anchor and rank by mean paired speedup; at most two per family'),
    )

    output.mkdir(parents=True, exist_ok=True)
    write_csv(output/'runs.csv', records)
    write_csv(output/'means.csv', means)
    (output/'coverage.json').write_text(json.dumps(
        dict(successful=len(records), missing=missing), indent=2) + '\n')
    (output/'selection.json').write_text(json.dumps(selection, indent=2) + '\n')
    return records, means, selection


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    runs, means, selection = analyze(args.root, args.output)
    print(f'validated {len(runs)} runs; wrote {len(means)} means; '
          f'selected {len(selection["variants"])} variants')
