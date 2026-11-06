#!/usr/bin/env python3
"""Submit recovery for missing multigrid-factorial applications."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

from resume_missing_campaign import missing_jobs, sha256
from run_mesh_sweep import write_json


ANCHORS = ((256, '1x2'), (256, '1x4'), (512, '8x4'), (512, '4x4'))
SMOOTHING_PAIRS = ('11', '12', '21', '22', '33', '44')


def queue_contains(job_id):
    status = subprocess.run(
        ['squeue', '-h', '-j', str(job_id), '-o', '%i'], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, universal_newlines=True)
    if status.returncode and 'invalid job id specified' not in status.stderr.lower():
        raise SystemExit('Cannot inspect prior allocation: ' + status.stderr)
    return bool(status.stdout.strip())


def task_output(plan, task):
    output = Path(task['output'])
    if not output.is_absolute():
        raise SystemExit('Campaign task output is not absolute: ' + str(output))
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path,
                        help='FD-PCG campaign root')
    parser.add_argument('--partition', default='normal')
    parser.add_argument('--time', default='00:40:00')
    parser.add_argument('--account')
    parser.add_argument('--uenv')
    parser.add_argument('--view')
    parser.add_argument('--timeout', type=float, default=180,
                        help='Seconds per missing application (default: 180)')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    root = args.root.resolve()
    study = root / 'tune' / 'multigrid-factorial'
    worker = Path(__file__).resolve().with_name('resume_missing_campaign.py')
    if not worker.is_file():
        raise SystemExit('Missing recovery worker: ' + str(worker))

    requested = 0
    for cells, placement in ANCHORS:
        nodes, per_node = map(int, placement.split('x'))
        for pair in SMOOTHING_PAIRS:
            base = study / ('n%d-%s' % (cells, placement)) / ('smoothing-' + pair)
            plan_path = base / 'plans' / (placement + '.json')
            if not plan_path.is_file():
                raise SystemExit('Missing factorial plan: ' + str(plan_path))
            plan = json.loads(plan_path.read_text())
            candidates = [(index, task) for index, task in enumerate(plan['tasks'])
                          if task['repeat'] == 0]
            if len(candidates) != 1:
                raise SystemExit('Expected one first-stage task in ' + str(plan_path))
            task_index, task = candidates[0]
            output = task_output(plan, task)
            manifest_path = output / 'campaign.json'
            if not manifest_path.is_file():
                print('%s: original campaign never started; use the normal first-stage helper' % base)
                continue
            manifest = json.loads(manifest_path.read_text())
            absent = missing_jobs(output, manifest['jobs'])
            if not absent:
                print('%d %s smoothing-%s: no missing applications' %
                      (cells, placement, pair), flush=True)
                continue

            # Never race the original allocation or a prior recovery allocation.
            original_state = base / 'plans' / (placement + '-submission.json')
            if original_state.is_file() and not args.dry_run:
                original_job = json.loads(original_state.read_text())['job_id']
                if queue_contains(original_job):
                    print('%d %s smoothing-%s: original allocation %s is still active; skipped' %
                          (cells, placement, pair, original_job), flush=True)
                    continue
            recovery_state = base / 'plans' / (placement + '-missing-submission.json')
            if recovery_state.is_file() and not args.dry_run:
                prior = json.loads(recovery_state.read_text())['job_id']
                if queue_contains(prior):
                    print('%d %s smoothing-%s: recovery allocation %s is still active; skipped' %
                          (cells, placement, pair, prior), flush=True)
                    continue

            log_stem = base / 'logs' / (placement + '-missing-%j')
            worker_command = [
                sys.executable, str(worker), '--plan', str(plan_path),
                '--task-index', str(task_index), '--timeout', str(args.timeout),
            ]
            command = [
                'sbatch', '--parsable', '--partition=' + args.partition,
                '--nodes=' + str(nodes), '--ntasks=' + str(nodes * per_node),
                '--ntasks-per-node=' + str(per_node), '--gpus-per-task=1',
                '--cpus-per-task=72', '--exclusive', '--mem=0', '--export=ALL',
                '--time=' + args.time,
                '--job-name=mgfix-%d-%s-%s' % (cells, placement, pair),
                '--output=' + str(log_stem) + '.log',
                '--error=' + str(log_stem) + '.err',
                '--wrap=exec ' + ' '.join(shlex.quote(value) for value in worker_command),
            ]
            for name in ('account', 'uenv', 'view'):
                value = getattr(args, name)
                if value:
                    command.insert(-1, '--' + name + '=' + value)
            print('%d %s smoothing-%s: %d missing applications' %
                  (cells, placement, pair, len(absent)), flush=True)
            print(' '.join(shlex.quote(value) for value in command), flush=True)
            requested += 1
            if args.dry_run:
                continue
            environment = dict(os.environ, FEM_ROOT=str(Path(os.environ.get(
                'FEM_ROOT', Path(__file__).resolve().parents[4])).resolve()),
                SLURM_EXPORT_ENV='ALL')
            for key in ('SBATCH_GET_USER_ENV', 'SLURM_GET_USER_ENV'):
                environment.pop(key, None)
            result = subprocess.run(
                command, check=True, stdout=subprocess.PIPE,
                universal_newlines=True, env=environment)
            job_id = result.stdout.strip().split(';')[0]
            if not job_id.isdigit():
                raise SystemExit('Unexpected sbatch response: ' + result.stdout)
            write_json(recovery_state, dict(
                job_id=job_id,
                command=command,
                recovery_runner_sha256=sha256(worker),
                missing_at_submission=[job['name'] for job in absent],
            ))
            print('Submitted recovery job ' + job_id, flush=True)
    if not requested:
        print('No incomplete factorial bundle requires recovery.', flush=True)


if __name__ == '__main__':
    main()
