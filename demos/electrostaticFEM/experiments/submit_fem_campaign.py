#!/usr/bin/env python3
"""Plan and submit current 3D FEM placement or preconditioner studies."""

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys

from submit_campaigns import (
    FEM_LARGE_PLACEMENTS, FEM_SMALL_PLACEMENTS, FEM_VARIANTS, PLACEMENTS,
)


SMALL_CELLS = (16, 32, 64)
LARGE_CELLS = (128, 256)
FEM_TUNING_BUNDLES = {
    'controls': ('cg', 'jacobi'),
    'gauss-seidel': ('cg', 'gs21', 'gs22'),
    'chebyshev-low': ('cg', 'cheb2', 'cheb4'),
    'chebyshev-high': ('cg', 'cheb8', 'cheb16'),
    'all': FEM_VARIANTS,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path,
                        help='Top-level FEM campaign output root')
    parser.add_argument('--phase', required=True, choices=['placement', 'tune'])
    parser.add_argument('--cells', required=True, nargs='+', type=int,
                        choices=SMALL_CELLS + LARGE_CELLS)
    parser.add_argument('--placements', required=True, nargs='+', choices=PLACEMENTS)
    parser.add_argument('--orders', nargs='+', type=int, choices=[1, 2, 3],
                        default=[1, 2, 3])
    tuning = parser.add_mutually_exclusive_group()
    tuning.add_argument('--bundle', choices=FEM_TUNING_BUNDLES,
                        help='Named paired method set for phase=tune')
    tuning.add_argument('--variants', nargs='+', choices=FEM_VARIANTS,
                        help='Explicit paired method set for phase=tune')
    parser.add_argument('--label',
                        help='Stable output label for an explicit tuning set')
    parser.add_argument('--stage', choices=['first', 'repeats', 'all'], default='first',
                        help='all submits all three repeats without first-run admission')
    parser.add_argument('--account')
    parser.add_argument('--uenv')
    parser.add_argument('--view')
    parser.add_argument('--dependency')
    parser.add_argument('--time', help='Slurm wall limit per array task')
    parser.add_argument('--timeout', type=float, help='Timeout per FEM process')
    parser.add_argument('--concurrent', type=int, default=1)
    parser.add_argument('--retry-failed', action='store_true')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()

    for values, label in ((args.cells, 'cells'), (args.placements, 'placements'),
                          (args.orders, 'orders')):
        if len(values) != len(set(values)):
            parser.error('duplicate ' + label)
    if args.concurrent < 1:
        parser.error('positive concurrency required')

    if args.phase == 'placement':
        if args.bundle or args.variants or args.label:
            parser.error('phase=placement is the fixed plain-CG allocation search')
        variants, label = ['cg'], 'cg'
    else:
        if not args.bundle and not args.variants:
            parser.error('phase=tune requires --bundle or explicit --variants including cg')
        if args.label and not args.variants:
            parser.error('--label applies only to explicit --variants')
        if args.label and not re.fullmatch(r'[a-z0-9]+(?:-[a-z0-9]+)*', args.label):
            parser.error('--label must contain lowercase letters, digits and single hyphens')
        variants = list(FEM_TUNING_BUNDLES[args.bundle] if args.bundle else args.variants)
        if len(variants) != len(set(variants)):
            parser.error('duplicate variants')
        if 'cg' not in variants or len(variants) < 2:
            parser.error('phase=tune requires cg and at least one FEM preconditioner')
        if args.bundle:
            label = args.bundle
        elif args.label:
            label = args.label
        else:
            digest = hashlib.sha256('\0'.join(variants).encode()).hexdigest()[:10]
            label = 'custom-' + digest

    invalid_small = set(args.placements) - set(FEM_SMALL_PLACEMENTS)
    invalid_large = set(args.placements) - set(FEM_LARGE_PLACEMENTS)
    if any(cells in SMALL_CELLS for cells in args.cells) and invalid_small:
        parser.error('small FEM cells do not support: ' + ', '.join(sorted(invalid_small)))
    if any(cells in LARGE_CELLS for cells in args.cells) and invalid_large:
        parser.error('large FEM cells do not support: ' + ', '.join(sorted(invalid_large)))

    submitter = Path(__file__).with_name('submit_campaigns.py')
    calls = []
    for cells in args.cells:
        campaign = 'fem-small' if cells in SMALL_CELLS else 'fem-large'
        default_time = '01:00:00' if campaign == 'fem-small' else '08:00:00'
        default_timeout = 3000 if campaign == 'fem-small' else 27000
        for placement in args.placements:
            output = args.root.resolve()/args.phase/label/f'n{cells}-{placement}'
            command = [
                sys.executable, str(submitter), '--application', 'fem',
                '--campaign', campaign, '--output-root', str(output),
                '--placements', placement, '--cells-3d', str(cells),
                '--orders', *map(str, args.orders), '--variants', *variants,
                '--steps', '10', '--repeats', '3', '--stage', args.stage,
                '--concurrent', str(args.concurrent), '--time', args.time or default_time,
                '--timeout', str(args.timeout or default_timeout),
            ]
            for option in ('account', 'uenv', 'view', 'dependency'):
                value = getattr(args, option)
                if value:
                    command += ['--' + option, value]
            if args.retry_failed:
                command.append('--retry-failed')
            if args.dry_run:
                command.append('--dry-run')
            calls.append(command)

    factor = 1 if args.stage == 'first' else 2 if args.stage == 'repeats' else 3
    print('phase=%s bundle=%s: %d allocation recipes, up to %d ordinary processes' %
          (args.phase, label, len(calls), len(calls)*len(args.orders)*len(variants)*factor),
          flush=True)
    print('variants=' + ' '.join(variants), flush=True)
    for command in calls:
        environment = dict(os.environ)
        environment.setdefault('FEM_ROOT', str(Path(__file__).resolve().parents[4]))
        subprocess.run(command, check=True, env=environment)


if __name__ == '__main__':
    main()
