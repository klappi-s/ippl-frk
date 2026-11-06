#!/usr/bin/env python3
#SBATCH --gpus-per-task=1
#SBATCH --cpus-per-task=72
#SBATCH --exclusive
#SBATCH --mem=0

"""Resume only applications that have no canonical job-state record.

This is deliberately separate from run_mesh_sweep.py's retry mode. A recorded
failure is evidence and is left untouched; only jobs absent from jobs/*.json
are launched. The existing campaign manifest and source hashes remain the
authority for the command, executable and validation contract.
"""

import argparse
import datetime
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

from ordinary_measurements import inspect_run
from run_mesh_sweep import command_for, write_json


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def missing_jobs(output, jobs):
    """Return jobs without a canonical state, regardless of prior attempt dirs."""
    states = output / 'jobs'
    return [job for job in jobs if not (states / (job['name'] + '.json')).is_file()]


def verify_pins(config):
    experiments = Path(__file__).resolve().parent
    checks = [
        (Path(config['executable']), config['executable_sha256'], 'executable'),
        (experiments / 'run_mesh_sweep.py', config['driver_sha256'], 'campaign runner'),
        (experiments / 'ordinary_measurements.py', config['reader_sha256'], 'measurement reader'),
        (experiments / 'fd_pcg_variants.py', config['variant_registry_sha256'], 'variant registry'),
    ]
    if config.get('rank_monitor'):
        checks.append((Path(config['rank_monitor']), config['rank_monitor_sha256'], 'rank monitor'))
    for path, expected, label in checks:
        if not path.is_file() or sha256(path) != expected:
            raise SystemExit('%s changed or is unavailable: %s' % (label, path))


def validate_allocation(config):
    required = ['SLURM_JOB_ID', 'SLURM_NTASKS', 'SLURM_JOB_NUM_NODES']
    missing = [name for name in required if name not in os.environ]
    if missing:
        raise SystemExit('Run through sbatch; missing ' + ', '.join(missing))
    if int(os.environ['SLURM_NTASKS']) != int(config['ranks']):
        raise SystemExit('Recovery allocation rank count differs from campaign')
    if int(os.environ['SLURM_JOB_NUM_NODES']) != int(config['nodes']):
        raise SystemExit('Recovery allocation node count differs from campaign')


def existing_state_summary(output, job, config):
    path = output / 'jobs' / (job['name'] + '.json')
    state = json.loads(path.read_text())
    if state.get('job') != job:
        raise SystemExit('Recorded job identity differs: ' + str(path))
    passed = state.get('returncode') == 0 and not state.get('validation_error')
    if passed:
        inspect_run(output / state['directory'], job, config)
        return 'successful'
    return 'recorded failure (exit=%s, wall=%.2fs)' % (
        state.get('returncode'), float(state.get('wall_seconds', 0)))


def run_missing(output, manifest, timeout):
    config, jobs = manifest['config'], manifest['jobs']
    jobs_dir = output / 'jobs'
    jobs_dir.mkdir(exist_ok=True)
    absent = {job['name'] for job in missing_jobs(output, jobs)}
    launched, failures = [], []
    interrupted = False

    def stop(signum, frame):
        raise KeyboardInterrupt

    previous = signal.signal(signal.SIGTERM, stop)
    try:
        for job in jobs:
            if job['name'] not in absent:
                print(job['name'] + ': ' + existing_state_summary(output, job, config)
                      + '; skipped', flush=True)
                continue

            attempt = 0
            while (output / ('%s_attempt%d' % (job['name'], attempt))).exists():
                attempt += 1
            target = output / ('%s_attempt%d' % (job['name'], attempt))
            data = target / 'data'
            data.mkdir(parents=True)
            command = command_for(config, job, data)
            state = dict(
                job=job,
                directory=str(data.relative_to(output)),
                command=command,
                started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                timeout_seconds=timeout,
                allocation=os.environ['SLURM_JOB_ID'],
                recovery='missing-only',
            )
            print(job['name'] + ' starting (previously missing)', flush=True)
            started = time.monotonic()
            code = None
            with (target / 'run.log').open('w') as log:
                child = subprocess.Popen(
                    command, cwd=str(data), stdout=log, stderr=subprocess.STDOUT,
                    start_new_session=True)
                try:
                    code = child.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGTERM)
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGKILL)
                        child.wait()
                    code = 124
                except KeyboardInterrupt:
                    os.killpg(child.pid, signal.SIGTERM)
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGKILL)
                        child.wait()
                    interrupted = True

            state.update(returncode=code if code is not None else 130,
                         wall_seconds=time.monotonic() - started,
                         validation_error='')
            if interrupted:
                # Preserve attempt evidence, but leave no canonical state. A
                # later missing-only recovery may safely retry this interrupted job.
                write_json(target / 'status.json', state)
                print('  interrupted; attempt retained without canonical state', flush=True)
                break
            if code == 0:
                try:
                    state['measurement'] = inspect_run(data, job, config)
                except (AssertionError, ValueError, KeyError, OSError) as error:
                    state['validation_error'] = str(error) or type(error).__name__
            write_json(target / 'status.json', state)
            write_json(jobs_dir / (job['name'] + '.json'), state)
            launched.append(job['name'])
            print('  exit=%d, wall=%.2fs, validation=%s' % (
                code, state['wall_seconds'],
                state['validation_error'] or ('passed' if code == 0 else 'not run')),
                flush=True)
            if code or state['validation_error']:
                failures.append(job['name'])
    finally:
        signal.signal(signal.SIGTERM, previous)
    return launched, failures, interrupted


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', required=True, type=Path)
    parser.add_argument('--task-index', required=True, type=int)
    parser.add_argument('--timeout', type=float, default=180,
                        help='Seconds per previously missing application (default: 180)')
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('Timeout must be finite and positive')

    plan = json.loads(args.plan.resolve(strict=True).read_text())
    if not 0 <= args.task_index < len(plan['tasks']):
        parser.error('Task index outside plan')
    task = plan['tasks'][args.task_index]
    output = Path(task['output']).resolve(strict=True)
    manifest_path = output / 'campaign.json'
    if not manifest_path.is_file():
        raise SystemExit('Existing campaign manifest is missing: ' + str(manifest_path))
    manifest = json.loads(manifest_path.read_text())
    if manifest.get('schema') != 2:
        raise SystemExit('Unsupported campaign schema')
    jobs = manifest.get('jobs')
    if (not isinstance(jobs, list) or not jobs
            or len({job.get('name') for job in jobs}) != len(jobs)):
        raise SystemExit('Invalid campaign job list')
    if ([job.get('variant') for job in jobs] != list(plan['variants'])
            or any(job.get('dimension') != task['dimension']
                   or job.get('order') != task['order']
                   or job.get('cells') != task['cells']
                   or job.get('repeat') != task['repeat'] for job in jobs)):
        raise SystemExit('Campaign manifest does not match the selected immutable plan task')
    config = manifest['config']
    validate_allocation(config)
    verify_pins(config)

    recovery_dir = output / 'recovery-allocations'
    recovery_dir.mkdir(exist_ok=True)
    allocation = os.environ['SLURM_JOB_ID']
    evidence = recovery_dir / (allocation + '.json')
    initial_missing = [job['name'] for job in missing_jobs(output, jobs)]
    record = dict(
        schema=1,
        mode='missing-only',
        allocation=allocation,
        plan=str(args.plan.resolve()),
        task_index=args.task_index,
        timeout_seconds=args.timeout,
        recovery_runner_sha256=sha256(__file__),
        started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        initial_missing=initial_missing,
    )
    write_json(evidence, record)
    with (output / 'campaign.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise SystemExit('Another runner owns this output directory')
        launched, failures, interrupted = run_missing(output, manifest, args.timeout)
    record.update(
        finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        launched=launched,
        failures=failures,
        interrupted=interrupted,
        remaining_missing=[job['name'] for job in missing_jobs(output, jobs)],
    )
    write_json(evidence, record)
    if interrupted:
        raise SystemExit(130)
    if failures:
        raise SystemExit('New unsuccessful runs (recorded and excluded from future missing-only '
                         'recovery): ' + ', '.join(failures))


if __name__ == '__main__':
    main()
