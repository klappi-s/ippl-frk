#!/usr/bin/env python3
"""Resumable ordinary FEM/Alpine runs: initialize, evolve, exit; one method/process."""
import argparse
import datetime
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import time
from ordinary_measurements import DEFAULT_VARIANTS, VARIANTS, inspect_run
from fd_pcg_variants import selected_definitions


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def command_for(config, job, directory):
    v = VARIANTS[job['variant']]
    command = shlex.split(config['launcher'].format(ranks=config['ranks']))
    if config.get('rank_monitor'):
        command += [sys.executable, config['rank_monitor'], '--output', str(directory),
                    '--estimate-gib', str(config['memory_estimate_gib']), '--']
    command += [config['executable']]
    if config['application'] == 'alpine':
        command += [str(job['cells'])]*3 + [str(job['particles']), str(config['steps']),
                    'CG' if v['method'] == 'plain' else 'PCG', '1.0', 'LeapFrog']
        command += v['args']
        command += ['--benchmark-output', str(directory), '--benchmark-tolerance', str(config['tolerance']),
                    '--benchmark-max-iterations', str(config['max_iterations']),
                    '--benchmark-dt', str(config['dt']), '--info', '0']
    else:
        options = dict(dim=job['dimension'], order=job['order'], cells=job['cells'],
            particles=job['particles'], steps=config['steps'], seed=42, dt=config['dt'],
            initial_condition='alpine_product', interpolation_nodes='gll', quadrature_points='exact',
            poisson_stiffness_mode='constant_preserving', reuse='full', initial_guess='previous',
            solver='plain' if v['method'] == 'plain' else 'preconditioned',
            preconditioner_type='jacobi' if v['method'] == 'plain' else v['method'],
            chebyshev_degree=v.get('degree', 8), gauss_seidel_inner_iterations=v.get('inner', 2),
            gauss_seidel_outer_iterations=v.get('outer', 2), tolerance=config['tolerance'],
            max_iterations=config['max_iterations'], timer_level='coarse', timer_recording='summary', output=str(directory))
        for key, value in options.items(): command += ['--' + key, str(value)]
    return command


def make_jobs(a):
    jobs = []
    grids = {1: a.cells_1d, 2: a.cells_2d, 3: a.cells_3d}
    for dim in a.dimensions:
        for order in (a.orders if a.application == 'fem' else [0]):
            for cells in grids[dim]:
                for repeat in range(a.repeat_offset, a.repeat_offset + a.repeats):
                    offset = repeat % len(a.variants)
                    for variant in a.variants[offset:] + a.variants[:offset]:
                        particles = a.particles_per_cell * cells**dim if a.particles_per_cell else a.particles
                        jobs.append(dict(index=len(jobs), name='d%d_p%d_n%d_%s_r%d' % (dim, order, cells, variant, repeat),
                            dimension=dim, order=order, cells=cells, repeat=repeat, variant=variant, particles=particles))
    return jobs


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--application', choices=['fem', 'alpine'], default='fem')
    p.add_argument('--executable', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--dimensions', type=int, nargs='+', choices=[1, 2, 3], default=[1, 2, 3])
    p.add_argument('--orders', type=int, nargs='+', choices=range(1, 6), default=[2, 3])
    p.add_argument('--cells-1d', type=int, nargs='+', default=[256, 512, 1024])
    p.add_argument('--cells-2d', type=int, nargs='+', default=[16, 32, 64])
    p.add_argument('--cells-3d', type=int, nargs='+', default=[8, 16, 32, 64])
    p.add_argument('--variants', nargs='+', choices=list(VARIANTS), default=DEFAULT_VARIANTS)
    p.add_argument('--particles', type=int, default=32768)
    p.add_argument('--particles-per-cell', type=int)
    p.add_argument('--steps', type=int, default=10)
    p.add_argument('--repeats', type=int, default=3)
    p.add_argument('--repeat-offset', type=int, default=0)
    p.add_argument('--ranks', type=int, default=1)
    p.add_argument('--nodes', type=int)
    p.add_argument('--gpus-per-node', type=int)
    p.add_argument('--rank-monitor', type=Path)
    p.add_argument('--memory-estimate-gib', type=float, default=0)
    p.add_argument('--launcher', default='mpiexec -n {ranks}')
    p.add_argument('--expected-backend', default='Serial')
    p.add_argument('--tolerance', type=float, default=1e-10)
    p.add_argument('--dt', type=float, default=.05)
    p.add_argument('--max-iterations', type=int, default=10000)
    p.add_argument('--timeout', type=float, default=9000, help='Seconds per application launch; may increase on resume')
    p.add_argument('--resume', action='store_true')
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--allow-large-particle-count', action='store_true',
                   help='Acknowledge that this run exceeds the legacy global INT_MAX audit')
    a = p.parse_args()
    if min(a.ranks, a.repeats, a.steps, a.particles, a.max_iterations) < 1 or a.repeat_offset < 0:
        p.error('Positive sizes and nonnegative repeat-offset required')
    if not all(math.isfinite(v) and v > 0 for v in [a.tolerance, a.dt, a.timeout]):
        p.error('Finite positive numeric controls required')
    if a.particles_per_cell is not None and a.particles_per_cell < 1: p.error('particles-per-cell must be positive')
    if a.application == 'alpine': a.dimensions = [3]
    if any(a.application not in VARIANTS[name]['applications'] for name in a.variants):
        p.error('One or more variants do not support application ' + a.application)
    for values in [a.dimensions, a.orders, a.variants, a.cells_1d, a.cells_2d, a.cells_3d]:
        if len(values) != len(set(values)): p.error('Duplicate dimensions/orders/variants/mesh sizes')
    if any(n < 2 for n in a.cells_1d + a.cells_2d + a.cells_3d): p.error('At least two cells per axis required')
    if (a.nodes is None) != (a.gpus_per_node is None): p.error('Set nodes and gpus-per-node together')
    if a.nodes is not None and (min(a.nodes, a.gpus_per_node) < 1 or a.nodes*a.gpus_per_node != a.ranks):
        p.error('Allocation metadata does not match rank count')
    if not math.isfinite(a.memory_estimate_gib) or a.memory_estimate_gib < 0: p.error('Invalid memory estimate')
    if a.rank_monitor: a.rank_monitor = str(a.rank_monitor.resolve(strict=True))
    a.output = a.output.resolve()
    a.executable = a.executable.resolve(strict=True)
    config = {k: v for k, v in vars(a).items() if k not in {'output', 'executable', 'resume', 'dry_run', 'timeout'}}
    config.update(executable=str(a.executable), executable_sha256=hashlib.sha256(a.executable.read_bytes()).hexdigest(),
        driver_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        reader_sha256=hashlib.sha256(Path(__file__).with_name('ordinary_measurements.py').read_bytes()).hexdigest(),
        variant_registry_sha256=hashlib.sha256(Path(__file__).with_name('fd_pcg_variants.py').read_bytes()).hexdigest(),
        variant_definitions=selected_definitions(a.variants),
        large_particle_count_acknowledged=a.allow_large_particle_count)
    if a.rank_monitor:
        config['rank_monitor_sha256'] = hashlib.sha256(Path(a.rank_monitor).read_bytes()).hexdigest()
    jobs = make_jobs(a)
    if any(j['particles'] > 2147483647 for j in jobs) and not a.allow_large_particle_count:
        p.error('Particle counts above INT_MAX require --allow-large-particle-count and a capacity preflight')
    a.output.mkdir(parents=True, exist_ok=a.resume)
    with (a.output/'campaign.lock').open('a') as lock:
        try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError: raise SystemExit('Another runner owns this output directory')
        manifest = dict(schema=2, config=config, jobs=jobs)
        path = a.output/'campaign.json'
        if path.exists():
            old = json.loads(path.read_text())
            if old.get('schema') != 2 or old['config'] != config or old['jobs'] != jobs:
                raise SystemExit('Resume configuration/binary/runner differs; use a new output directory')
        else:
            manifest['created_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
            write_json(path, manifest)
        (a.output/'jobs').mkdir(exist_ok=True)
        if a.dry_run:
            for job in jobs:
                print(job['index'], job['name'], ' '.join(shlex.quote(s) for s in command_for(config, job, a.output/job['name'])))
            return
        def interrupted(signum, frame): raise KeyboardInterrupt
        previous = signal.signal(signal.SIGTERM, interrupted)
        failures = []
        try:
            for job in jobs:
                state_path = a.output/'jobs'/(job['name']+'.json')
                if state_path.exists():
                    state = json.loads(state_path.read_text())
                    if state['returncode'] == 0 and not state.get('validation_error'):
                        inspect_run(a.output/state['directory'], job, config)
                        print(job['name'], 'completed; skipped', flush=True)
                        continue
                attempt = 0
                while (a.output/('%s_attempt%d' % (job['name'], attempt))).exists(): attempt += 1
                target = a.output/('%s_attempt%d' % (job['name'], attempt))
                data = target/'data'
                data.mkdir(parents=True)
                command = command_for(config, job, data)
                state = dict(job=job, directory=str(data.relative_to(a.output)), command=command,
                    started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), timeout_seconds=a.timeout,
                    allocation=os.environ.get('SLURM_JOB_ID', 'local'))
                print(job['name'], 'starting', flush=True)
                started = time.monotonic()
                with (target/'run.log').open('w') as log:
                    child = subprocess.Popen(command, cwd=str(data), stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                    try: code = child.wait(timeout=a.timeout)
                    except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
                        os.killpg(child.pid, signal.SIGTERM)
                        try: child.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            os.killpg(child.pid, signal.SIGKILL)
                            child.wait()
                        code = 124 if isinstance(error, subprocess.TimeoutExpired) else 130
                state.update(returncode=code, wall_seconds=time.monotonic()-started, validation_error='')
                if code == 0:
                    try: state['measurement'] = inspect_run(data, job, config)
                    except (AssertionError, ValueError, KeyError, OSError) as error:
                        state['validation_error'] = str(error) or type(error).__name__
                write_json(target/'status.json', state)
                write_json(state_path, state)
                print('  exit=%d, wall=%.2fs, validation=%s' %
                    (code, state['wall_seconds'], state['validation_error'] or ('passed' if code == 0 else 'not run')), flush=True)
                if code or state['validation_error']: failures.append(job['name'])
                if code == 130: break
        finally:
            signal.signal(signal.SIGTERM, previous)
        if failures: raise SystemExit('Unsuccessful runs (logs retained): ' + ', '.join(failures))


if __name__ == '__main__': main()
