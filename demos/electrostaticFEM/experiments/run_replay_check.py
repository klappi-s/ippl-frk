#!/usr/bin/env python3
"""Compare ordinary PIC, load capture, solver reset and preconditioner history.

Uses the existing executable without rebuilding. Repeated methods are separate
blocks, identified by their position, never merged by method name. Standard
library only; no plots or Markdown are generated in the result directory.
"""
import argparse
import csv
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import shlex
import signal
import statistics
import subprocess
import sys
import time

from check_compact_timers import read_rows

CASES = ['normal_cg', 'normal_pcg', 'replay_cg', 'replay_cg_cg', 'replay_cg_pcg_cg']


def replay_methods(case, preconditioner):
    return {'normal_cg': [], 'normal_pcg': [], 'replay_cg': ['plain'],
            'replay_cg_cg': ['plain', 'plain'],
            'replay_cg_pcg_cg': ['plain', preconditioner, 'plain']}[case]


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def write_csv(path, rows):
    if not rows:
        return
    with path.open('w') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader(); writer.writerows(rows)


def command_for(config, case, directory):
    command = shlex.split(config['launcher'].format(ranks=config['ranks'])) + [config['executable']]
    options = dict(dim=config['dimension'], order=config['order'], cells=config['cells'],
                   particles=config['particles'], steps=config['steps'], seed=42,
                   lengths='12.566370614359172', dt=.05, alpha=.05, mode=1, thermal_speed=1,
                   interpolation_nodes='gll', quadrature_points='exact',
                   poisson_stiffness_mode='constant_preserving', reuse='full', initial_guess='previous',
                   solver='preconditioned' if case == 'normal_pcg' else 'plain',
                   preconditioner_type=config['preconditioner'], chebyshev_degree=config['degree'],
                   gauss_seidel_inner_iterations=2, gauss_seidel_outer_iterations=config['gs_outer'],
                   tolerance=config['tolerance'], max_iterations=10000, diagnostics_every=1,
                   timer_level=config['timer_level'], timer_recording='summary', timer_sample_limit=1024,
                   output=str(directory))
    methods = replay_methods(case, config['preconditioner'])
    if methods:
        options['replay'] = ','.join(methods)
    for key, value in options.items():
        command += ['--' + key, str(value)]
    return command


def inspect_run(directory, case, settings):
    """Validate complete rank/block sequences, then sum locally before max rank."""
    config = dict(line.split('=', 1) for line in (directory / 'configuration.txt').read_text().splitlines())
    for key in ['dimension', 'order', 'particles', 'steps', 'ranks']:
        assert int(config[key]) == settings[key], key
    assert config['backend'] == settings['expected_backend'], 'Wrong execution backend'
    assert all(int(config['cells_%d' % d]) == settings['cells'] for d in range(settings['dimension']))
    for key, value in dict(interpolation_nodes='gll', stiffness='constant_preserving', reuse='full',
                           initial_guess='previous', seed='42', timer_recording='summary',
                           timer_sample_limit='0', timer_level=settings['timer_level'],
                           load_balancing='disabled').items():
        assert config[key] == value, key
    assert int(config['quadrature_points']) == settings['order'] + 1
    assert float(config['tolerance']) == settings['tolerance']
    assert int(config['chebyshev_degree']) == settings['degree']
    assert int(config['gauss_seidel_inner_iterations']) == 2
    assert int(config['gauss_seidel_outer_iterations']) == settings['gs_outer']
    methods = replay_methods(case, settings['preconditioner'])
    assert config['replay'] == ','.join(methods)
    pic_method = settings['preconditioner'] if case == 'normal_pcg' else 'plain'
    assert config['pic_method'] == pic_method
    count, ranks = settings['steps'] + 1, settings['ranks']
    gate = max(20 * settings['tolerance'], 5e-11)
    blocks = [[] for _ in range(1 + len(methods))]
    timers = read_rows(directory / 'timer-summary.csv')
    assert {int(r['rank']) for r in timers} == set(range(ranks))
    assert all(r['stored_samples'] == '0' for r in timers)
    assert not (directory / 'timers.csv').exists()
    for rank in range(ranks):
        rows = read_rows(directory / ('solves-rank%d.csv' % rank))
        assert len(rows) == count * len(blocks), 'Missing/extra solves'
        counts = {r['timer_name']: int(r['count']) for r in timers if int(r['rank']) == rank}
        assert counts['evaluateAx'] == counts['evaluateAx: outer loop'] == sum(int(r['operator_calls']) for r in rows) + len(rows)
        reference = None
        for index, method in enumerate([pic_method] + methods):
            group = rows[index*count:(index+1)*count]
            assert [int(r['solve']) for r in group] == list(range(count)), 'Incomplete/repeated block'
            assert [r['phase'] for r in group] == (['initial'] + ['step']*(count-1) if index == 0 else ['replay']*count)
            assert all(r['method'] == method and r['success'] == '1' and int(r['rank']) == rank for r in group)
            assert [int(r['operator_setups']) for r in group] == [1] + [0]*(count-1)
            assert [int(r['preconditioner_setups']) for r in group] == [int(method != 'plain')] + [0]*(count-1)
            signature = [float(r['rhs_norm']) for r in group]
            assert all(math.isfinite(v) and v > 0 for v in signature)
            if reference is None:
                reference = signature
            assert all(math.isclose(a, b, rel_tol=2e-11, abs_tol=1e-13) for a, b in zip(signature, reference)), 'Replay loads differ'
            if rank:
                other = blocks[index][0]
                assert [r['iterations'] for r in group] == [r['iterations'] for r in other], 'Ranks disagree on iterations'
                assert all(math.isclose(float(a['rhs_norm']), float(b['rhs_norm']), rel_tol=2e-11, abs_tol=1e-13)
                           for a, b in zip(group, other)), 'Ranks disagree on loads'
            for r in group:
                assert math.isfinite(float(r['fresh_relative_residual'])) and 0 <= float(r['fresh_relative_residual']) <= gate
                assert all(math.isfinite(float(v)) and float(v) >= 0 for k, v in r.items() if k.endswith('_seconds'))
                assert 0 <= int(r['iterations']) < 10000
                if method == 'plain':
                    assert int(r['preconditioner_applications']) == 0
            blocks[index].append(group)
    physics = read_rows(directory / 'physics.csv')
    assert [int(r['step']) for r in physics] == list(range(count))
    volume = 1.
    for d in range(settings['dimension']):
        volume *= float(config['length_%d' % d])
    for r in physics:
        assert all(math.isfinite(float(v)) for v in r.values())
        assert abs(float(r['net_charge']))/volume <= 1e-11 and abs(float(r['mean_potential'])) <= 1e-11
        energy = float(r['field_energy'])
        assert energy >= 0 and abs(energy-float(r['weak_field_energy']))/max(energy, 1e-12) <= max(100*settings['tolerance'], 1e-10)
    pic_totals = []
    for rank in range(ranks):
        steps = read_rows(directory / ('steps-rank%d.csv' % rank))
        totals = [r for r in steps if r['stage'] == 'step_total']
        assert [int(r['step']) for r in totals] == list(range(1, count))
        assert all(math.isfinite(float(r['seconds'])) and float(r['seconds']) >= 0 for r in steps)
        assert sum(r['stage'] == 'capture_load' for r in steps) == (count-1 if methods else 0)
        pic_totals.append(sum(float(r['seconds']) for r in totals))
    summaries, per_solve = [], []
    for index, groups in enumerate(blocks):
        def recurring(key):
            return max(sum(float(r[key]) for r in group[1:]) for group in groups)/(count-1)
        label = 'pic' if index == 0 else 'replay%d' % index
        summaries.append(dict(block=label, method=groups[0][0]['method'],
                              recurring_seconds=recurring('solve_seconds'),
                              recurring_iterations=statistics.mean(int(r['iterations']) for r in groups[0][1:]),
                              total_seconds=max(sum(float(r['solve_seconds']) for r in group) for group in groups),
                              first_setup_seconds=max(sum(float(group[0][k]) for k in ['operator_setup_seconds', 'preconditioner_setup_seconds']) for group in groups),
                              krylov_seconds=recurring('krylov_seconds'), operator_seconds=recurring('operator_seconds'),
                              preconditioner_seconds=recurring('preconditioner_apply_seconds'),
                              operator_calls=recurring('operator_calls'),
                              pic_step_seconds=max(pic_totals)/(count-1) if index == 0 else '',
                              max_fresh_residual=max(float(r['fresh_relative_residual']) for group in groups for r in group)))
        for i in range(count):
            per_solve.append(dict(block=label, method=groups[0][i]['method'], solve=i,
                                  iterations=int(groups[0][i]['iterations']), rhs_norm=float(groups[0][i]['rhs_norm']),
                                  solve_seconds=max(float(group[i]['solve_seconds']) for group in groups),
                                  krylov_seconds=max(float(group[i]['krylov_seconds']) for group in groups),
                                  operator_seconds=max(float(group[i]['operator_seconds']) for group in groups)))
    return dict(blocks=summaries, per_solve=per_solve, pic_rhs=[float(r['rhs_norm']) for r in blocks[0][0]])


def summarize(output, manifest):
    records, per_solve, completed, missing, allocations = [], [], {}, [], {}
    for job in manifest['jobs']:
        path = output / 'jobs' / (job['name'] + '.json')
        state = json.loads(path.read_text()) if path.exists() else None
        if not state or state['returncode'] != 0 or state.get('validation_error'):
            missing.append(job['name']); continue
        result = inspect_run(output / state['directory'], job['case'], manifest['config'])
        completed[(job['repeat'], job['case'])] = result
        allocations[(job['repeat'], job['case'])] = state.get('allocation', 'local')
        context = dict(case=job['case'], repeat=job['repeat'], directory=state['directory'],
                       allocation=state.get('allocation', 'local'), timer_level=manifest['config']['timer_level'])
        records += [dict(**context, **r) for r in result['blocks']]
        per_solve += [dict(**context, **r) for r in result['per_solve']]
    comparisons, warnings = [], []
    for repeat in range(manifest['config']['repeats']):
        normal = completed.get((repeat, 'normal_cg'))
        normal_pcg = completed.get((repeat, 'normal_pcg'))
        if normal and normal_pcg:
            match = all(math.isclose(a, b, rel_tol=2e-11, abs_tol=1e-13)
                        for a, b in zip(normal['pic_rhs'], normal_pcg['pic_rhs']))
            comparisons.append(dict(repeat=repeat, comparison='normal PCG / normal CG timestep',
                                    ratio=normal_pcg['blocks'][0]['pic_step_seconds']/normal['blocks'][0]['pic_step_seconds'],
                                    rhs_norms_match=match,
                                    same_allocation=allocations[(repeat, 'normal_cg')] == allocations[(repeat, 'normal_pcg')]))
        for case in ['replay_cg', 'replay_cg_cg', 'replay_cg_pcg_cg']:
            result = completed.get((repeat, case))
            if not result:
                continue
            if normal:
                same_allocation = allocations[(repeat, 'normal_cg')] == allocations[(repeat, case)]
                if not same_allocation:
                    warnings.append('%s r%d: cross-process comparison spans different allocations after resume' % (case, repeat))
                loads_match = all(math.isclose(a, b, rel_tol=2e-11, abs_tol=1e-13)
                                  for a, b in zip(normal['pic_rhs'], result['pic_rhs']))
                if not loads_match:
                    warnings.append('%s r%d: regenerated CG PIC loads differ; cross-process ratios are not identical-load comparisons' % (case, repeat))
                comparisons.append(dict(repeat=repeat, comparison=case + ': capture PIC CG solve / normal CG solve',
                                        ratio=result['blocks'][0]['recurring_seconds']/normal['blocks'][0]['recurring_seconds'],
                                        rhs_norms_match=loads_match, same_allocation=same_allocation))
                comparisons.append(dict(repeat=repeat, comparison=case + ': capture timestep / normal timestep',
                                        ratio=result['blocks'][0]['pic_step_seconds']/normal['blocks'][0]['pic_step_seconds'],
                                        rhs_norms_match=loads_match, same_allocation=same_allocation))
                if case == 'replay_cg':
                    comparisons.append(dict(repeat=repeat, comparison='replay CG / normal CG solve',
                                            ratio=result['blocks'][1]['recurring_seconds']/normal['blocks'][0]['recurring_seconds'],
                                            rhs_norms_match=loads_match, same_allocation=same_allocation))
            if case != 'replay_cg':
                comparisons.append(dict(repeat=repeat, comparison=case + ': last CG / first CG',
                                        ratio=result['blocks'][-1]['recurring_seconds']/result['blocks'][1]['recurring_seconds'],
                                        rhs_norms_match=True, same_allocation=True))
    write_csv(output / 'blocks.csv', records)
    write_csv(output / 'per-solve.csv', per_solve)
    write_csv(output / 'comparisons.csv', comparisons)
    averages = []
    for case, block, method in sorted({(r['case'], r['block'], r['method']) for r in records}):
        group = [r for r in records if (r['case'], r['block'], r['method']) == (case, block, method)]
        values = [r['recurring_seconds'] for r in group]
        averages.append(dict(case=case, block=block, method=method, n=len(group),
                             mean_seconds=statistics.mean(values), min_seconds=min(values), max_seconds=max(values),
                             mean_iterations=statistics.mean(r['recurring_iterations'] for r in group)))
    write_csv(output / 'means.csv', averages)
    write_json(output / 'analysis.json', dict(complete=len(completed), missing=missing, warnings=warnings,
               interpretation='Ratios >1 mean the numerator is slower. Duplicate methods are separate replay blocks. '
                              'Normal PCG follows its own trajectory. Detailed and coarse campaigns must not be pooled.'))
    return missing, warnings


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--executable', type=Path)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--dimension', type=int, choices=[1, 2, 3], default=3)
    p.add_argument('--order', type=int, choices=range(1, 6), default=2)
    p.add_argument('--cells', type=int, default=64)
    p.add_argument('--steps', type=int, default=10)
    p.add_argument('--particles', type=int, default=32768)
    p.add_argument('--repeats', type=int, default=2)
    p.add_argument('--ranks', type=int, default=8)
    p.add_argument('--launcher', default='mpiexec -n {ranks}')
    p.add_argument('--preconditioner', choices=['chebyshev', 'jacobi', 'gauss-seidel'], default='chebyshev')
    p.add_argument('--degree', type=int, default=8)
    p.add_argument('--gs-outer', type=int, default=2)
    p.add_argument('--cases', nargs='+', choices=CASES, default=CASES)
    p.add_argument('--timer-level', choices=['coarse', 'detailed'], default='coarse')
    p.add_argument('--expected-backend', default='Cuda')
    p.add_argument('--tolerance', type=float, default=1e-10)
    p.add_argument('--timeout', type=float, default=3600)
    p.add_argument('--resume', action='store_true')
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--analyze-only', action='store_true')
    a = p.parse_args()
    output = a.output.resolve()
    if a.analyze_only:
        missing, warnings = summarize(output, json.loads((output / 'campaign.json').read_text()))
        print('Incomplete cases:', missing, '\nWarnings:', warnings)
        return
    if a.executable is None:
        p.error('--executable is required for execution/dry-run')
    if min(a.steps, a.particles, a.repeats, a.ranks, a.degree, a.gs_outer) < 1 or a.cells < 2:
        p.error('Invalid sizes/iteration counts')
    if not math.isfinite(a.timeout) or a.timeout <= 0 or not math.isfinite(a.tolerance) or a.tolerance <= 0:
        p.error('Positive finite timeout and tolerance required')
    if len(set(a.cases)) != len(a.cases):
        p.error('Duplicate cases')
    config = {k: v for k, v in vars(a).items() if k not in ['output', 'executable', 'resume', 'dry_run', 'analyze_only', 'timeout']}
    executable = a.executable.resolve(strict=True)
    config.update(executable=str(executable), executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
                  driver_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  reader_sha256=hashlib.sha256(Path(__file__).with_name('check_compact_timers.py').read_bytes()).hexdigest())
    jobs = []
    for repeat in range(a.repeats):
        # Reverse independent process order to expose time drift. The sequence
        # WITHIN each process is always the named diagnostic, never rotated.
        for case in a.cases if repeat % 2 == 0 else list(reversed(a.cases)):
            jobs.append(dict(name='%s_r%d' % (case, repeat), case=case, repeat=repeat))
    manifest = dict(config=config, jobs=jobs)
    output.mkdir(parents=True, exist_ok=a.resume)
    path = output / 'campaign.json'
    if path.exists():
        old = json.loads(path.read_text())
        if old['config'] != config or old['jobs'] != jobs:
            raise SystemExit('Configuration/binary/driver changed; use a new output directory.')
    else:
        write_json(path, manifest)
    (output / 'jobs').mkdir(exist_ok=True)
    if a.dry_run:
        for job in jobs:
            print(job['name'], shlex.join(command_for(config, job['case'], output / job['name'])) if hasattr(shlex, 'join')
                  else ' '.join(shlex.quote(x) for x in command_for(config, job['case'], output / job['name'])))
        return
    def terminate(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, terminate)
    for job in jobs:
        statefile = output / 'jobs' / (job['name'] + '.json')
        if statefile.exists():
            old = json.loads(statefile.read_text())
            if old['returncode'] == 0 and not old.get('validation_error'):
                inspect_run(output / old['directory'], job['case'], config)
                print(job['name'], 'completed; skipped', flush=True); continue
        attempt = 0
        while (output / ('%s_attempt%d' % (job['name'], attempt))).exists():
            attempt += 1
        folder = output / ('%s_attempt%d' % (job['name'], attempt))
        folder.mkdir()
        target = folder / 'data'
        command = command_for(config, job['case'], target)
        print(job['name'], 'starting', flush=True)
        started = time.monotonic()
        timestamp = datetime.datetime.now(datetime.timezone.utc).isoformat()
        with (folder / 'run.log').open('w') as log:
            child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                code = child.wait(timeout=a.timeout)
            except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
                os.killpg(child.pid, signal.SIGTERM)
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL); child.wait()
                code = 124 if isinstance(error, subprocess.TimeoutExpired) else 130
        validation_error = ''
        if code == 0:
            try:
                inspect_run(target, job['case'], config)
            except Exception as error:
                validation_error = '%s: %s' % (type(error).__name__, error)
        result = dict(job=job, directory=str(target.relative_to(output)), command=command, returncode=code,
                      validation_error=validation_error, wall_seconds=time.monotonic()-started, started_utc=timestamp,
                      timeout_seconds=a.timeout, allocation=os.environ.get('SLURM_JOB_ID', 'local'))
        write_json(folder / 'status.json', result)
        write_json(statefile, result)
        validation = (validation_error or 'passed') if code == 0 else 'not accepted'
        print('  exit=%d, wall=%.2fs, validation=%s' % (code, result['wall_seconds'], validation), flush=True)
        summarize(output, manifest)
        if code == 130:
            break
    missing, warnings = summarize(output, manifest)
    print('Results:', output, '\nWarnings:', warnings)
    if missing:
        raise SystemExit('Incomplete/failed cases retained for resume: ' + ', '.join(missing))


if __name__ == '__main__':
    main()
