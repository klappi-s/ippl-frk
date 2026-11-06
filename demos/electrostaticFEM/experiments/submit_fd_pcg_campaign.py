#!/usr/bin/env python3
"""Plan and submit the staged Alpine FD PCG tuning campaign."""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from fd_pcg_variants import (
    LARGE_COUNT_PREFLIGHT_VARIANTS,
    SMOKE_VARIANTS,
    TUNING_BUNDLES,
    VARIANTS,
)
from submit_campaigns import FD_PCG_PLACEMENTS


ORDINARY_PLACEMENTS = {128: ['1x2', '1x4'], 256: ['1x2'], 512: ['8x4'], 1024: ['8x4']}
MULTIGRID_PLACEMENTS = {128: ['1x2', '1x4'], 256: ['1x4'], 512: ['2x4'], 1024: ['8x4']}
PREFLIGHT_NUMERICAL_FAILURES = {'newton5'}


def selection_variants(path):
    value = json.loads(Path(path).read_text())
    names = value.get('variants')
    if not isinstance(names, list) or not names or len(names) != len(set(names)):
        raise SystemExit('Selection must contain a nonempty unique "variants" list')
    unknown = [name for name in names if name not in VARIANTS]
    if unknown:
        raise SystemExit('Unknown selection variants: ' + ', '.join(unknown))
    if 'cg' not in names:
        raise SystemExit('Selection must retain paired control "cg"')
    return names


def recorded_numerical_failure(directory):
    """Return true when Alpine reached its independent residual rejection.

    This distinguishes a completed capacity attempt from launch, memory, CUDA,
    timeout and process failures, none of which produce a rank-zero solve row.
    """
    path = directory/'solves-rank0.csv'
    if not path.is_file():
        return False
    try:
        with path.open() as stream:
            rows = list(csv.DictReader(stream))
        return bool(rows) and rows[-1].get('success') == '0' \
            and float(rows[-1]['solve_seconds']) > 0 \
            and float(rows[-1]['fresh_relative_residual']) >= 0
    except (KeyError, TypeError, ValueError):
        return False


def preflight_complete(root):
    """Check capacity evidence while preserving known numerical failures.

    Every fixed preflight method must have been attempted. CG, SSOR and
    multigrid must pass normally. Newton level 5 may reject its solution at the
    fresh-residual gate after completing a solve; that still demonstrates the
    large-particle allocation and does not block transferring Newton level 1.
    """
    study = root/'preflight'/'capacity'/'n1024-8x4'
    plan_path = study/'plans'/'8x4.json'
    if not plan_path.exists():
        return False
    plan = json.loads(plan_path.read_text())
    first = next((task for task in plan['tasks'] if task['repeat'] == 0), None)
    if first is None:
        return False
    campaign_path = Path(first['output'])/'campaign.json'
    if not campaign_path.exists():
        return False
    campaign = json.loads(campaign_path.read_text())
    jobs = campaign.get('jobs', [])
    if [job.get('variant') for job in jobs] != LARGE_COUNT_PREFLIGHT_VARIANTS:
        return False
    for job in jobs:
        status_path = Path(first['output'])/'jobs'/(job['name'] + '.json')
        if not status_path.exists():
            return False
        status = json.loads(status_path.read_text())
        if status.get('returncode') == 0 and not status.get('validation_error'):
            continue
        data = Path(first['output'])/status.get('directory', '')
        if (job['variant'] not in PREFLIGHT_NUMERICAL_FAILURES
                or status.get('returncode') != 1
                or status.get('validation_error')
                or not recorded_numerical_failure(data)):
            return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path,
                        help='Immutable top-level output root for this source/build')
    parser.add_argument('--phase', required=True,
                        choices=['smoke', 'preflight', 'placement', 'tune',
                                 'transfer', 'confirm'])
    parser.add_argument('--bundle', choices=list(TUNING_BUNDLES),
                        help='Parameter family for phase=tune')
    parser.add_argument('--variants', nargs='+', choices=list(VARIANTS),
                        help='Explicit tuning subset; gets a content-derived bundle name')
    parser.add_argument('--label',
                        help='Stable output label for an explicit tuning subset')
    parser.add_argument('--substudy',
                        help='Optional slash-separated subdivisions below a labeled mesh/placement root')
    parser.add_argument('--reuse-first-subset', action='store_true',
                        help='Admit repeats from successful requested variants in the parent first run')
    parser.add_argument('--selection', type=Path,
                        help='JSON shortlist for transfer/confirm')
    parser.add_argument('--cells', nargs='+', type=int, choices=[128, 256, 512, 1024],
                        help='Tune defaults to 256 512; 1024 is supported after preflight')
    parser.add_argument('--placements', nargs='+', choices=FD_PCG_PLACEMENTS,
                        help='Override the phase placement shortlist')
    parser.add_argument('--stage', choices=['first', 'repeats', 'all'], default='first')
    parser.add_argument('--account')
    parser.add_argument('--uenv')
    parser.add_argument('--view')
    parser.add_argument('--dependency')
    parser.add_argument('--time', help='Slurm wall limit per array task (default: 00:59:00)')
    parser.add_argument('--timeout', type=float,
                        help='Timeout per Alpine process in seconds (default: 3000)')
    parser.add_argument('--concurrent', type=int, default=1)
    parser.add_argument('--retry-failed', action='store_true')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()

    if args.reuse_first_subset and not (
            args.phase == 'tune' and args.variants and args.label
            and args.substudy and args.stage == 'repeats'):
        parser.error('--reuse-first-subset requires labeled explicit tune repeats in a substudy')

    if args.phase == 'placement':
        if args.bundle or args.variants or args.selection or args.label or args.substudy:
            parser.error('phase=placement is the fixed plain-CG allocation search')
        if not args.cells or not args.placements:
            parser.error('phase=placement requires explicit --cells and --placements')
        variants, cells, label = ['cg'], list(args.cells), 'cg'
    elif args.phase == 'tune':
        if bool(args.bundle) == bool(args.variants):
            parser.error('phase=tune requires exactly one of --bundle or --variants')
        if args.label and not args.variants:
            parser.error('--label requires an explicit --variants tuning subset')
        if args.substudy and not args.label:
            parser.error('--substudy requires --label and an explicit --variants subset')
        if args.label and not re.fullmatch(r'[a-z0-9]+(?:-[a-z0-9]+)*', args.label):
            parser.error('--label must contain lowercase letters, digits and single hyphens')
        if args.substudy and not re.fullmatch(
                r'[a-z0-9]+(?:-[a-z0-9]+)*(?:/[a-z0-9]+(?:-[a-z0-9]+)*)*',
                args.substudy):
            parser.error('--substudy must contain slash-separated lowercase slugs')
        variants = list(args.variants or TUNING_BUNDLES[args.bundle])
        if variants == ['cg']:
            parser.error('use phase=placement for a plain-CG allocation search')
        cells = args.cells or [256, 512]
        label = args.bundle
        if args.variants:
            if args.label:
                label = args.label
            else:
                digest = hashlib.sha256('\0'.join(variants).encode()).hexdigest()[:10]
                label = 'custom-' + digest
    elif args.phase in ('transfer', 'confirm'):
        if (not args.selection or args.bundle or args.variants
                or args.label or args.substudy):
            parser.error('transfer/confirm requires --selection and no bundle override')
        variants = selection_variants(args.selection)
        cells = args.cells or [128, 1024]
        label = args.selection.stem
    elif args.phase == 'smoke':
        if (args.bundle or args.variants or args.selection or args.label or args.substudy
                or args.cells or args.placements):
            parser.error('smoke has a fixed correctness recipe')
        variants, cells, label = SMOKE_VARIANTS, [128], 'families'
    else:
        if (args.bundle or args.variants or args.selection or args.label or args.substudy
                or args.cells or args.placements):
            parser.error('preflight has a fixed 1024 capacity recipe')
        variants, cells, label = LARGE_COUNT_PREFLIGHT_VARIANTS, [1024], 'capacity'

    if len(variants) != len(set(variants)):
        parser.error('Duplicate variants')
    if args.placements and len(args.placements) != len(set(args.placements)):
        parser.error('Duplicate placements')
    if any(name not in VARIANTS for name in variants):
        parser.error('Unknown variant')
    if args.phase not in ('smoke', 'preflight') and 1024 in cells:
        if not args.dry_run and not preflight_complete(args.root.resolve()):
            raise SystemExit(
                '1024^3 is blocked until the fixed phase=preflight recipe succeeds at this root')

    contains_multigrid = any(VARIANTS[name]['family'] == 'multigrid' for name in variants)
    placement_map = MULTIGRID_PLACEMENTS if contains_multigrid else ORDINARY_PLACEMENTS
    if args.placements:
        placement_map = {cells_per_axis: args.placements for cells_per_axis in cells}
    if args.phase == 'preflight':
        placement_map = {1024: ['8x4']}
    elif args.phase == 'smoke':
        placement_map = {128: ['1x2']}

    submitter = Path(__file__).with_name('submit_campaigns.py')
    extended_slurm = None
    if args.placements and any(placement in ('16x4', '32x4')
                               for placement in args.placements):
        workspace = Path(os.environ.get(
            'FEM_ROOT', str(Path(__file__).resolve().parents[4])))
        site_script = workspace/'gh200_campaign_extended.slurm'
        extended_slurm = (site_script if site_script.is_file()
                          else Path(__file__).with_name('gh200_campaign_extended.slurm'))
    calls = []
    for cells_per_axis in cells:
        for placement in placement_map[cells_per_axis]:
            output = args.root.resolve()/args.phase/label/f'n{cells_per_axis}-{placement}'
            first_run_output_root = output
            if args.substudy:
                substudy = Path(args.substudy)
                if args.reuse_first_subset and substudy.parent != Path('.'):
                    first_run_output_root /= substudy.parent
                output /= substudy
            steps = 1 if args.phase == 'preflight' else (2 if args.phase == 'smoke' else 10)
            repeats = 1 if args.phase in ('smoke', 'preflight') else 3
            command = [
                sys.executable, str(submitter), '--application', 'alpine',
                '--campaign', 'fd-pcg', '--output-root', str(output),
                '--placements', placement, '--cells-3d', str(cells_per_axis),
                '--variants', *variants, '--steps', str(steps), '--repeats', str(repeats),
                '--stage', 'first' if args.phase in ('smoke', 'preflight') else args.stage,
                '--concurrent', str(args.concurrent),
            ]
            if cells_per_axis == 1024:
                command.append('--allow-large-particle-count')
            if extended_slurm:
                command += ['--slurm-script', str(extended_slurm.resolve())]
            command += ['--time', args.time or '00:59:00',
                        '--timeout', str(args.timeout or 3000)]
            for option in ('account', 'uenv', 'view', 'dependency'):
                value = getattr(args, option)
                if value:
                    command += ['--' + option, value]
            if args.retry_failed:
                command.append('--retry-failed')
            if args.reuse_first_subset:
                command += ['--first-run-output-root', str(first_run_output_root)]
            if args.dry_run:
                command.append('--dry-run')
            calls.append(command)

    ordinary = len(calls) * len(variants) * (
        1 if args.phase in ('smoke', 'preflight') or args.stage == 'first' else
        2 if args.stage == 'repeats' else 3)
    print(f'phase={args.phase} bundle={label}: {len(calls)} allocation recipes, '
          f'up to {ordinary} ordinary application processes', flush=True)
    print('variants=' + ' '.join(variants), flush=True)
    for command in calls:
        environment = dict(os.environ)
        environment.setdefault('FEM_ROOT', str(Path(__file__).resolve().parents[4]))
        subprocess.run(command, check=True, env=environment)


if __name__ == '__main__':
    main()
