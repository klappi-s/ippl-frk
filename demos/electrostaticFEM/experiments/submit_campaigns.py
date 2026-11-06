#!/usr/bin/env python3
"""Submit ordinary FEM or Alpine studies; --dry-run prints the complete Slurm plan."""
import argparse
import fcntl
import json
import math
import os
from pathlib import Path
import shlex
import subprocess
from ordinary_measurements import DEFAULT_VARIANTS, VARIANTS
from run_mesh_sweep import make_jobs, write_json

# FEM exposes the shared plain/Jacobi/GS/Chebyshev subset. Derive it from the
# pinned canonical registry so submission and execution cannot drift, without
# changing that registry while active FD studies pin its file hash.
FEM_VARIANTS = tuple(
    name for name, definition in VARIANTS.items()
    if 'fem' in definition['applications']
)

LEGACY_PLACEMENTS = ['1x1', '1x2', '1x4', '2x4', '2x1']
SMALL_PLACEMENTS = ['1x1', '1x2', '1x4', '2x1', '2x2', '2x4', '4x1', '4x2', '4x4']
LARGE_PLACEMENTS = ['1x2', '1x4', '2x4', '4x4', '8x4']
FD_PCG_PLACEMENTS = LARGE_PLACEMENTS + ['16x4', '32x4']
# Keep historical preset defaults unchanged; wider FEM searches are opt-in.
FEM_SMALL_PLACEMENTS = SMALL_PLACEMENTS + ['8x1', '8x2', '8x4', '16x1', '16x2', '16x4']
FEM_LARGE_PLACEMENTS = LARGE_PLACEMENTS + ['2x1', '4x1', '8x1', '16x1',
                                            '2x2', '4x2', '8x2', '16x2', '16x4']
PLACEMENTS = FEM_SMALL_PLACEMENTS + ['32x4']

def tasks_for(a, placement):
    meshes = []
    if a.application == 'alpine':
        meshes = [(3, 0, n) for n in (a.cells_3d or [8, 16, 32, 64])]
    else:
        for order in a.orders:
            grids = {1: [256, 512, 1024], 2: [32, 64, 128] if order == 2 else [16, 32, 64],
                     3: a.cells_3d or ([8, 16, 32, 64] if order == 2 else [4, 8, 16, 32, 64])}
            meshes += [(dim, order, n) for dim in a.dimensions for n in grids[dim]]
    if a.large:
        meshes += [(3, p, n) for p in (a.orders if a.application == 'fem' else [0]) for n in a.large]
    meshes = sorted(set(meshes))
    if placement == '2x1' and not a.full_control and getattr(a, 'campaign', 'custom') == 'custom':
        meshes = [(d, p, n) for d, p, n in meshes if
                  (d == 3 and n in ([16, 64] if p in (0, 2) else [8, 64]))
                  or (d == 2 and n == (64 if p == 2 else 32))]
    tasks = []
    for dim, order, cells in meshes:
        for repeat in range(a.repeats):
            name = 'd%d_p%d_n%d_r%d' % (dim, order, cells, repeat)
            args = ['--dimensions', str(dim), '--orders', str(order or 2), '--cells-%dd' % dim, str(cells),
                    '--steps', str(getattr(a, 'steps', 10)), '--repeats', '1', '--repeat-offset', str(repeat),
                    '--tolerance', str(a.tolerance), '--dt', str(a.dt), '--variants'] + a.variants
            ppc = getattr(a, 'particles_per_cell', None)
            args += ['--particles-per-cell', str(ppc)] if ppc else ['--particles', str(a.particles)]
            nodes, per_node = map(int, placement.split('x'))
            args += ['--nodes', str(nodes), '--gpus-per-node', str(per_node)]
            # Reuse the worker's population calculation and count guard.
            job_args = argparse.Namespace(application=a.application, dimensions=[dim],
                orders=[order], cells_1d=[cells], cells_2d=[cells], cells_3d=[cells],
                repeat_offset=repeat, repeats=1, variants=a.variants,
                particles_per_cell=ppc, particles=a.particles)
            particles = make_jobs(job_args)[0]['particles']
            allow_large = getattr(a, 'allow_large_particle_count', False)
            if particles > 2147483647 and not allow_large:
                raise ValueError('Particle counts above INT_MAX require --allow-large-particle-count')
            if particles > 2147483647:
                args.append('--allow-large-particle-count')
            task = dict(name=name, output=str(a.output_root/placement/name), arguments=args,
                        dimension=dim, order=order, cells=cells, repeat=repeat, particles=particles)
            if getattr(a, 'campaign', 'custom') != 'custom':
                scalar = 8 * ((order or 1) * cells)**3
                diagnostic = 56 * (order+1)**3 * cells**3 if order else 0
                extra_fields = max(VARIANTS[name].get('extra_scalar_fields', 0)
                                   for name in a.variants)
                scalar_fields = (10 if order else 9) + extra_fields
                floor = (80*particles + scalar_fields*scalar + diagnostic)/(nodes*per_node)
                task['memory'] = dict(payload_floor_gib=floor/2**30,
                    screening_estimate_gib=(1.5*floor + 2**30)/2**30,
                    scalar_field_equivalents=scalar_fields,
                    maximum_variant_extra_fields=extra_fields,
                    large_particle_count_acknowledged=particles > 2147483647,
                    note='1.5 times average payload plus 1 GiB; planning margin, not a peak bound')
                if task['memory']['screening_estimate_gib'] > 70:
                    if a.campaign != 'fem-large':
                        raise ValueError('Memory screening budget exceeded: '+name+' '+placement)
                    task['memory']['screening_excluded'] = True
                    task['memory']['note'] += '; excluded until a new measured capacity model is pinned'
                else:
                    args += ['--rank-monitor', str(Path(__file__).with_name('rank_memory.py').resolve()),
                             '--memory-estimate-gib', str(task['memory']['screening_estimate_gib'])]
            tasks.append(task)
    return tasks

def task_complete(task, variants):
    campaign = Path(task['output'])/'campaign.json'
    if not campaign.exists(): return False
    saved = json.loads(campaign.read_text())
    if saved.get('schema') != 2 or len(saved['jobs']) != len(variants): return False
    for job in saved['jobs']:
        path = Path(task['output'])/'jobs'/(job['name']+'.json')
        if not path.exists(): return False
        state = json.loads(path.read_text())
        if state['returncode'] or state.get('validation_error'): return False
    return True


def task_subset_complete(task, variants):
    """Admit repeats from a completed first run that contains extra variants."""
    campaign = Path(task['output'])/'campaign.json'
    if not campaign.exists():
        return False
    saved = json.loads(campaign.read_text())
    if saved.get('schema') != 2:
        return False
    jobs = {job.get('variant'): job for job in saved.get('jobs', [])}
    if not set(variants).issubset(jobs):
        return False
    for variant in variants:
        job = jobs[variant]
        path = Path(task['output'])/'jobs'/(job['name']+'.json')
        if not path.exists():
            return False
        state = json.loads(path.read_text())
        if state.get('returncode') != 0 or state.get('validation_error'):
            return False
    return True


def save_plan(path, plan):
    """Preserve recipes; allow adding subset admission to an unused draft only.

    A dry-run from before subset admission was added may have already saved
    exactly the requested execution recipe. Preserve that draft for provenance
    and add only the admission metadata, before any task could consume it.
    The caller holds the study's submission lock.
    """
    if not path.exists():
        write_json(path, plan)
        return
    previous_bytes = path.read_bytes()
    previous = json.loads(previous_bytes)
    if previous == plan:
        return
    admission = plan.get('repeat_admission', {})
    recipe = {key: value for key, value in plan.items() if key != 'repeat_admission'}
    study = path.parent.parent
    unused = (not any(path.parent.glob('*-submission.json'))
              and not (study/'study-pin.json').exists()
              and not any(Path(task['output']).exists() for task in previous.get('tasks', []))
              and not any((study/'logs').glob('*')))
    if (admission.get('mode') != 'successful-variant-subset'
            or previous != recipe or not unused):
        raise SystemExit('Study recipe changed: use a new output root ('+str(path)+')')
    history = path.parent/'history'
    history.mkdir(exist_ok=True)
    backup = history/(path.stem+'-before-subset-admission.json')
    if backup.exists():
        if backup.read_bytes() != previous_bytes:
            raise SystemExit('Conflicting draft-plan backup: '+str(backup))
    else:
        backup.write_bytes(previous_bytes)
    write_json(path, plan)
    print('Added parent first-run admission to unsubmitted plan: '+str(path), flush=True)


def apply_campaign(a, parser):
    if a.campaign == 'custom':
        a.placements = a.placements or LEGACY_PLACEMENTS
        a.orders = a.orders or [2, 3]
        a.dimensions = a.dimensions or [1, 2, 3]
        a.variants = a.variants or DEFAULT_VARIANTS
        a.stage = a.stage or 'all'
        a.particles = 32768 if a.particles is None else a.particles
        a.steps = 10 if a.steps is None else a.steps
        return
    small = a.campaign == 'fem-small'
    fem_large = a.campaign == 'fem-large'
    fd_pcg = a.campaign == 'fd-pcg'
    if a.application != ('fem' if small or fem_large else 'alpine'):
        parser.error('Campaign/application mismatch')
    allowed = (FEM_SMALL_PLACEMENTS if small else
               FEM_LARGE_PLACEMENTS if fem_large else
               FD_PCG_PLACEMENTS if fd_pcg else LARGE_PLACEMENTS)
    grids = [16, 32, 64] if small else ([128, 256] if fem_large else
            ([128, 256, 512, 1024] if fd_pcg else [128, 256, 512]))
    legal_variants = list(VARIANTS) if fd_pcg else (
        list(FEM_VARIANTS) if small or fem_large else ['cg'])
    for requested, legal, name in [(a.placements, allowed, 'placements'),
            (a.orders, [1, 2, 3], 'orders'), (a.cells_3d, grids, 'cells-3d'),
            (a.dimensions, [3], 'dimensions'), (a.variants, legal_variants, 'variants')]:
        if requested and not set(requested).issubset(legal): parser.error('Invalid '+name+' for '+a.campaign)
    if a.large or a.particles is not None or a.particles_per_cell not in (None, 8):
        parser.error('Phase-1 presets require eight particles/cell and their fixed mesh lists')
    # Extended 16x4/32x4 placements are opt-in for FD-PCG so existing default
    # recipes retain their original five-placement identity.
    a.placements = a.placements or (SMALL_PLACEMENTS if small else LARGE_PLACEMENTS)
    a.orders = a.orders or [1, 2, 3]
    a.dimensions = [3]
    a.cells_3d = a.cells_3d or grids
    a.variants = a.variants or ['cg']
    a.particles, a.particles_per_cell = 32768, 8
    a.steps = 10 if a.steps is None else a.steps
    a.stage = a.stage or 'first'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--application', choices=['fem', 'alpine'], required=True)
    p.add_argument('--output-root', type=Path, required=True, help='Reuse this path to resume; use a new path for changed physics/binary')
    p.add_argument('--campaign', choices=['custom', 'fem-small', 'fem-large', 'fd-large', 'fd-pcg'], default='custom')
    p.add_argument('--stage', choices=['first', 'repeats', 'all'],
                   help='Presets default to first; repeats require successful r0; all queues every repeat independently')
    p.add_argument('--placements', nargs='+', choices=PLACEMENTS)
    p.add_argument('--orders', type=int, nargs='+', choices=[1, 2, 3])
    p.add_argument('--dimensions', type=int, nargs='+', choices=[1, 2, 3])
    p.add_argument('--cells-3d', type=int, nargs='+', help='Override ordinary 3D mesh lists; changes the recipe')
    p.add_argument('--select-cells', type=int, nargs='+', help='Submit only these mesh sizes without changing the saved recipe')
    p.add_argument('--select-orders', type=int, nargs='+', choices=[1, 2, 3],
                   help='Submit only these FEM orders without changing the saved recipe')
    p.add_argument('--large', type=int, nargs='+', choices=[128, 256, 512, 1024], help='Opt-in larger 3D cells/axis')
    p.add_argument('--full-control', action='store_true', help='Use all meshes also at 2 nodes x 1 GPU')
    p.add_argument('--variants', nargs='+', choices=list(VARIANTS))
    p.add_argument('--repeats', type=int, default=3)
    p.add_argument('--steps', type=int, help='Normal PIC timesteps; presets default to ten')
    population = p.add_mutually_exclusive_group()
    population.add_argument('--particles', type=int)
    population.add_argument('--particles-per-cell', type=int)
    p.add_argument('--tolerance', type=float, default=1e-10)
    p.add_argument('--dt', type=float, default=.05)
    p.add_argument('--timeout', type=float, help='Per-simulation timeout (default: 27000 s FEM-large, 3000 s other presets, 9000 s custom)')
    p.add_argument('--time', help='Slurm wall limit per array task (default: 08:00:00 FEM-large/custom, 01:00:00 FEM-small, 00:59:00 FD)')
    p.add_argument('--concurrent', type=int, default=1, help='Concurrent exclusive allocations per placement array')
    p.add_argument('--dependency', help='Slurm dependency, e.g. afterok:<build-job>')
    p.add_argument('--account')
    p.add_argument('--uenv')
    p.add_argument('--view')
    p.add_argument('--slurm-script', type=Path)
    p.add_argument('--retry-failed', action='store_true', help='Explicitly retry a failed preset run after investigating its logs')
    p.add_argument('--first-run-output-root', type=Path,
                   help='For repeats, admit a requested variant subset from an existing first-run root')
    p.add_argument('--allow-large-particle-count', action='store_true',
                   help='Acknowledge the >INT_MAX global population after a dedicated preflight')
    p.add_argument('--dry-run', action='store_true')
    a = p.parse_args()
    apply_campaign(a, p)
    if a.select_orders and a.application != 'fem':
        p.error('--select-orders requires FEM')
    if a.first_run_output_root and a.stage != 'repeats':
        p.error('--first-run-output-root requires --stage repeats')
    if a.timeout is None: a.timeout = 27000 if a.campaign == 'fem-large' else (9000 if a.campaign == 'custom' else 3000)
    if a.time is None:
        a.time = {'fem-small': '01:00:00', 'fd-large': '00:59:00', 'fd-pcg': '00:59:00',
                  'fem-large': '08:00:00'}.get(a.campaign, '08:00:00')
    if a.particles_per_cell is not None and a.particles_per_cell < 1: p.error('Positive particles-per-cell required')
    for values in [a.placements, a.orders, a.dimensions, a.variants, a.cells_3d or []]:
        if len(values) != len(set(values)): p.error('Duplicate campaign entries')
    if min(a.repeats, a.steps, a.particles, a.concurrent) < 1: p.error('Positive repeats/steps/particles/concurrency required')
    if a.particles > 2147483647 and not a.allow_large_particle_count:
        p.error('Particle counts above INT_MAX require --allow-large-particle-count')
    if not all(math.isfinite(v) and v > 0 for v in [a.tolerance, a.dt, a.timeout]):
        p.error('Finite positive tolerance, dt and timeout required')
    if a.cells_3d and min(a.cells_3d) < 2: p.error('At least two cells per axis required')
    a.output_root = a.output_root.resolve()
    root = Path(os.environ.get('FEM_ROOT', os.getcwd())).resolve()
    script = a.slurm_script or root/'gh200_campaign.slurm'
    if not script.is_file(): script = Path(__file__).with_name('gh200_campaign.slurm')
    script = script.resolve()
    a.output_root.mkdir(parents=True, exist_ok=True)
    (a.output_root/'plans').mkdir(exist_ok=True)
    (a.output_root/'logs').mkdir(exist_ok=True)
    with (a.output_root/'submission.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        for placement in a.placements:
            tasks = tasks_for(a, placement)
            if not tasks: continue
            plan = dict(schema=2, campaign=a.campaign, application=a.application, placement=placement, tasks=tasks,
                        diagnostic_implementation='full-volume-quadrature' if a.application == 'fem' else 'mesh-reduction',
                        variants={name: VARIANTS[name] for name in a.variants},
                        large_particle_count_acknowledged=a.allow_large_particle_count)
            if a.first_run_output_root:
                plan['repeat_admission'] = dict(
                    mode='successful-variant-subset',
                    output_root=str(a.first_run_output_root.resolve()))
            path = a.output_root/'plans'/(placement+'.json')
            save_plan(path, plan)
            pending = []
            for index, task in enumerate(tasks):
                if a.select_cells and task['cells'] not in a.select_cells: continue
                if a.select_orders and task['order'] not in a.select_orders: continue
                if task.get('memory', {}).get('screening_excluded'):
                    print(task['name']+': excluded by pinned memory screen', flush=True)
                    continue
                if a.stage == 'first' and task['repeat'] != 0: continue
                if a.stage == 'repeats' and task['repeat'] == 0: continue
                if a.campaign != 'custom' and a.stage == 'repeats' and task['repeat'] > 0:
                    first = next(t for t in tasks if t['dimension'] == task['dimension']
                                 and t['order'] == task['order'] and t['cells'] == task['cells'] and t['repeat'] == 0)
                    checker = task_complete
                    if a.first_run_output_root:
                        first = dict(first, output=str(
                            a.first_run_output_root.resolve()/placement/first['name']))
                        checker = task_subset_complete
                    if not checker(first, a.variants):
                        print(task['name']+': waiting for successful first run', flush=True)
                        continue
                if not task_complete(task, a.variants):
                    failures = list((Path(task['output'])/'jobs').glob('*.json'))
                    if a.campaign != 'custom' and failures and not a.retry_failed:
                        print(task['name']+': failed/incomplete record; inspect before --retry-failed', flush=True)
                        continue
                    pending.append(index)
            if not pending:
                print(placement+': no eligible pending tasks in this stage; no allocation requested', flush=True)
                continue
            nodes, per_node = map(int, placement.split('x'))
            command = ['sbatch', '--parsable', '--nodes='+str(nodes), '--ntasks='+str(nodes*per_node),
                       '--ntasks-per-node='+str(per_node), '--array='+','.join(map(str, pending))+'%'+str(a.concurrent),
                       '--export=ALL',
                       '--time='+a.time, '--job-name='+a.application+'-'+placement,
                       '--output='+str(a.output_root/'logs'/(placement+'-%A_%a.log')),
                       '--error='+str(a.output_root/'logs'/(placement+'-%A_%a.err'))]
            for key in ['account', 'uenv', 'view', 'dependency']:
                if getattr(a, key): command += ['--'+key+'='+getattr(a, key)]
            command += [str(script), str(path)]
            # Export lists (even ALL,NAME=value) implicitly request login-environment
            # retrieval in Slurm. Supply values in sbatch's own environment instead.
            overrides = dict(FEM_ROOT=str(root), FEM_RUN_TIMEOUT=str(a.timeout),
                             SLURM_EXPORT_ENV='ALL')
            submission_env = dict(os.environ, **overrides)
            for key in ['SBATCH_GET_USER_ENV', 'SLURM_GET_USER_ENV']:
                submission_env.pop(key, None)
            memory_estimates = [tasks[index].get('memory', {}).get('screening_estimate_gib')
                                for index in pending]
            memory_estimates = [value for value in memory_estimates if value is not None]
            memory_note = (', maximum screening estimate %.2f GiB/GPU' % max(memory_estimates)
                           if memory_estimates else '')
            print('%s: %d pending allocations / %d planned, up to %d ordinary simulations%s' %
                  (placement, len(pending), len(tasks), len(pending)*len(a.variants), memory_note),
                  flush=True)
            preview = ['env', '-u', 'SBATCH_GET_USER_ENV', '-u', 'SLURM_GET_USER_ENV']
            preview += [key+'='+value for key, value in overrides.items()] + command
            print(' '.join(shlex.quote(s) for s in preview), flush=True)
            if a.dry_run: continue
            state_path = a.output_root/'plans'/(placement+'-submission.json')
            if state_path.exists():
                previous = json.loads(state_path.read_text())['job_id']
                status = subprocess.run(['squeue', '-h', '-j', previous, '-o', '%i'],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
                # Slurm may reject a canceled/purged array ID instead of returning
                # an empty queue. Only that specific response means it is inactive.
                if status.returncode and 'invalid job id specified' not in status.stderr.lower():
                    raise SystemExit('Cannot check prior allocation; refusing duplicate submission: '+status.stderr)
                if status.stdout.strip():
                    print('Array '+previous+' is still queued/running; skipped', flush=True)
                    continue
            result = subprocess.run(command, check=True, stdout=subprocess.PIPE, universal_newlines=True,
                                    env=submission_env)
            job_id = result.stdout.strip().split(';')[0]
            if not job_id.isdigit(): raise SystemExit('Unexpected sbatch response: '+result.stdout)
            temporary = state_path.with_suffix('.tmp')
            temporary.write_text(json.dumps(dict(job_id=job_id, command=command,
                                                environment_overrides=overrides), indent=2)+'\n')
            temporary.replace(state_path)
            print('Submitted array '+job_id, flush=True)

if __name__ == '__main__': main()
