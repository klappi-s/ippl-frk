#!/usr/bin/env python3
"""Resumable mesh sweep using the existing identical-load replay runner."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import time

BATCHES = {
    'baseline': dict(methods=['plain', 'jacobi', 'gauss-seidel', 'chebyshev'], degree=8, outer=2),
    'tuned': dict(methods=['plain', 'gauss-seidel', 'chebyshev'], degree=16, outer=1),
}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--executable', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--dimensions', type=int, nargs='+', choices=[1, 2, 3], default=[1, 2, 3])
    p.add_argument('--cells-1d', type=int, nargs='+', default=[128, 256, 512, 1024])
    p.add_argument('--cells-2d', type=int, nargs='+', default=[8, 16, 32])
    p.add_argument('--cells-3d', type=int, nargs='+', default=[3, 4, 6])
    p.add_argument('--orders', type=int, nargs='+', default=[3])
    p.add_argument('--particles', type=int, default=32768)
    p.add_argument('--particles-per-cell', type=int, help='Overrides fixed global particle count')
    p.add_argument('--steps', type=int, default=10)
    p.add_argument('--repeats', type=int, default=3)
    p.add_argument('--ranks', type=int, default=1)
    p.add_argument('--launcher', default='mpiexec -n {ranks}')
    p.add_argument('--family', choices=['gll', 'equispaced'], default='gll')
    p.add_argument('--quadrature', choices=['9', 'exact'], default='exact')
    p.add_argument('--initial-guess', choices=['zero', 'previous'], default='previous')
    p.add_argument('--timer-level', choices=['coarse', 'detailed'], default='coarse')
    p.add_argument('--tolerance', type=float, default=1e-10)
    p.add_argument('--max-iterations', type=int, default=10000)
    p.add_argument('--timeout', type=float, default=3600, help='Seconds per replay batch, including MPI launch')
    p.add_argument('--resume', action='store_true')
    p.add_argument('--dry-run', action='store_true', help='Write/validate the manifest and list jobs only')
    p.add_argument('--job-index', type=int, help='Run one manifest job (e.g. a Slurm array task)')
    a = p.parse_args()
    if min(a.ranks, a.repeats, a.particles, a.max_iterations) < 1 or a.steps < 1 or a.timeout <= 0:
        p.error('Positive sizes, repeats, timeout and at least one timestep are required.')
    if a.particles_per_cell is not None and a.particles_per_cell < 1:
        p.error('particles-per-cell must be positive')
    if any(order not in range(1, 6) for order in a.orders):
        p.error('Supported orders are 1 through 5')
    grids = {1: a.cells_1d, 2: a.cells_2d, 3: a.cells_3d}
    if any(n < 2 for sizes in grids.values() for n in sizes):
        p.error('At least two cells per axis are required')
    a.output = a.output.resolve()
    a.executable = a.executable.resolve(strict=True)
    runner = Path(__file__).with_name('run_reuse.py').resolve()
    config = {k: v for k, v in vars(a).items() if k not in {'resume', 'dry_run', 'job_index', 'output', 'executable'}}
    config.update(executable=str(a.executable), executable_sha256=hashlib.sha256(a.executable.read_bytes()).hexdigest(),
                  replay_runner_sha256=hashlib.sha256(runner.read_bytes()).hexdigest(), batches=BATCHES)
    jobs = []
    for dim in a.dimensions:
        for order in a.orders:
            for n in grids[dim]:
                for repeat in range(a.repeats):
                    # Alternate which batch runs first; run_reuse rotates method order.
                    batches = list(BATCHES) if repeat % 2 == 0 else list(reversed(BATCHES))
                    for batch in batches:
                        jobs.append(dict(index=len(jobs), name=f'd{dim}_p{order}_n{n}_{batch}_r{repeat}',
                                         dimension=dim, order=order, cells=n, repeat=repeat, batch=batch,
                                         particles=a.particles_per_cell * n**dim if a.particles_per_cell else a.particles))
    manifest = dict(schema=1, config=config, jobs=jobs)
    a.output.mkdir(parents=True, exist_ok=a.resume)
    path = a.output / 'campaign.json'
    if path.exists():
        old = json.loads(path.read_text())
        if old['config'] != config or old['jobs'] != jobs:
            raise SystemExit('Resume configuration/binary/runner differs from the saved campaign. Use a new output directory.')
    else:
        manifest.update(created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                        host=platform.node(), platform=platform.platform(), python=sys.version,
                        environment={k: os.environ[k] for k in ['OMP_NUM_THREADS', 'OMP_PROC_BIND', 'OMP_PLACES',
                                     'CUDA_VISIBLE_DEVICES', 'SLURM_JOB_ID', 'SLURM_JOB_NUM_NODES', 'MPICH_GPU_SUPPORT_ENABLED'] if k in os.environ})
        path.write_text(json.dumps(manifest, indent=2) + '\n')
    if a.job_index is not None:
        if a.job_index not in range(len(jobs)):
            p.error('job-index is outside the manifest')
        jobs = [jobs[a.job_index]]
    (a.output / 'jobs').mkdir(exist_ok=True)
    failures = []
    for job in jobs:
        print(job['index'], job['name'], flush=True)
        if a.dry_run:
            continue
        status_path = a.output / 'jobs' / (job['name'] + '.json')
        if status_path.exists() and json.loads(status_path.read_text())['returncode'] == 0:
            print('  completed; skipped', flush=True)
            continue
        # Preserve failed or interrupted attempts for diagnosis.
        attempt = 0
        while (a.output / f"{job['name']}_attempt{attempt}").exists():
            attempt += 1
        target = a.output / f"{job['name']}_attempt{attempt}"
        batch = BATCHES[job['batch']]
        command = [sys.executable, str(runner), '--executable', str(a.executable), '--output', str(target),
                   '--dimension', str(job['dimension']), '--orders', str(job['order']), '--cells', str(job['cells']),
                   '--particles', str(job['particles']), '--steps', str(a.steps), '--repeats', '1',
                   '--repeat-offset', str(job['repeat']), '--ranks', str(a.ranks), '--launcher', a.launcher,
                   '--reuse', 'full', '--timer-level', a.timer_level, '--quadrature', a.quadrature,
                   '--family', a.family, '--initial-guess', a.initial_guess, '--degree', str(batch['degree']),
                   '--gs-outer', str(batch['outer']), '--tolerance', str(a.tolerance),
                   '--max-iterations', str(a.max_iterations), '--methods', *batch['methods']]
        started = time.monotonic()
        with target.with_suffix('.log').open('w') as log:
            child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                code = child.wait(timeout=a.timeout)
            except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
                os.killpg(child.pid, signal.SIGTERM)
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()
                code = 124 if isinstance(error, subprocess.TimeoutExpired) else 130
        result = dict(job=job, directory=target.name, command=command, returncode=code,
                      wall_seconds=time.monotonic()-started)
        temporary = status_path.with_suffix('.tmp')
        temporary.write_text(json.dumps(result, indent=2) + '\n')
        temporary.replace(status_path)
        print(f"  exit={code}, wall={result['wall_seconds']:.2f}s", flush=True)
        if code:
            failures.append(job['name'])
            if code == 130:
                break
    if failures:
        raise SystemExit('Unsuccessful jobs (logs retained): ' + ', '.join(failures))


if __name__ == '__main__':
    main()
