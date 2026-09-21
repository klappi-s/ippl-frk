#!/usr/bin/env python3
"""Validate a mesh campaign and plot iterations, costs and paired CG speedups."""
import argparse
import csv
import json
import os
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from replay_measurements import read_replay, read_pic_steps

METHODS = ['plain', 'jacobi', 'gs2/1', 'gs2/2', 'cheb8', 'cheb16']
LABELS = ['CG', 'Jacobi', 'GS 2/1', 'GS 2/2', 'Chebyshev 8', 'Chebyshev 16']
COLORS = ['#333333', '#2878a8', '#e08c32', '#ab542a', '#58a26a', '#7856a3']


def load_campaigns(directories):
    """Combine disjoint mesh extensions while preserving each run's provenance."""
    campaigns = [(directory.resolve(), json.loads((directory / 'campaign.json').read_text()))
                 for directory in directories]
    # Mesh selection, execution timeout and binary path/hash may differ. The
    # caller must assess source compatibility; differing binaries are disclosed.
    common = ['orders', 'particles', 'particles_per_cell', 'steps', 'repeats', 'ranks',
              'family', 'quadrature', 'initial_guess', 'timer_level', 'tolerance',
              'max_iterations', 'batches', 'replay_runner_sha256']
    reference = campaigns[0][1]['config']
    seen = set()
    for directory, manifest in campaigns:
        for key in common:
            if manifest['config'][key] != reference[key]:
                raise ValueError(f'Incompatible campaign setting {key}: {directory}')
        for job in manifest['jobs']:
            key = tuple(job[field] for field in ['dimension', 'order', 'cells', 'repeat', 'batch'])
            if key in seen:
                raise ValueError(f'Overlapping campaign job: {directory}, {job["name"]}')
            seen.add(key)
    return campaigns


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    p.add_argument('--include', type=Path, nargs='+', default=[],
                   help='Additional compatible campaigns with disjoint mesh cases')
    p.add_argument('--output', type=Path, help='Analysis directory; defaults to the first campaign')
    p.add_argument('--exclude-mesh', nargs='+', default=[], metavar='DIM:CELLS',
                   help='Explicitly omit these meshes from the analysis scope')
    p.add_argument('--exclusion-reason', help='Required explanation when omitting planned meshes')
    p.add_argument('--allow-incomplete', action='store_true', help='Produce a clearly labelled partial report')
    p.add_argument('--allow-regenerated-load-variation', action='store_true',
                   help='Allow varying regenerated loads; each candidate must still match its paired CG')
    a = p.parse_args()
    try:
        excluded = {tuple(map(int, value.split(':'))) for value in a.exclude_mesh}
        if any(len(value) != 2 or value[0] not in (1, 2, 3) or value[1] < 2 for value in excluded):
            raise ValueError()
    except ValueError:
        p.error('Excluded meshes must have the form DIM:CELLS, e.g. 3:16')
    if excluded and not a.exclusion_reason:
        p.error('--exclude-mesh requires --exclusion-reason')
    campaigns = load_campaigns([a.directory, *a.include])
    root = (a.output or a.directory).resolve()
    root.mkdir(parents=True, exist_ok=True)
    manifest = campaigns[0][1]
    expected = [(source, source_manifest, job) for source, source_manifest in campaigns
                for job in source_manifest['jobs']]
    available = {(job['dimension'], job['cells']) for _, _, job in expected}
    if not excluded <= available:
        p.error('An excluded mesh does not exist in the source campaigns')
    omitted = [f'{source.name}/{job["name"]}' for source, _, job in expected
               if (job['dimension'], job['cells']) in excluded]
    expected = [(source, source_manifest, job) for source, source_manifest, job in expected
                if (job['dimension'], job['cells']) not in excluded]
    records, missing, signatures, varied_loads = [], [], {}, []
    for source, source_manifest, job in expected:
        path = source / 'jobs' / (job['name'] + '.json')
        if not path.exists() or json.loads(path.read_text())['returncode']:
            missing.append(f'{source.name}/{job["name"]}')
            continue
        state = json.loads(path.read_text())
        directory = source / state['directory']
        run = json.loads((directory / 'runs.json').read_text())[0]
        config, rows, sequences, signature = read_replay(directory / run['name'])
        for field in ['dimension', 'order', 'particles']:
            if int(config[field]) != job[field]:
                raise ValueError(f'Application {field} differs from manifest')
        if int(config['ranks']) != source_manifest['config']['ranks'] or int(config['steps']) != source_manifest['config']['steps']:
            raise ValueError('Application ranks/steps differ from manifest')
        if any(int(config[f'cells_{d}']) != job['cells'] for d in range(job['dimension'])):
            raise ValueError('Application mesh differs from manifest')
        batch = source_manifest['config']['batches'][job['batch']]
        if set(rows) != set(batch['methods']):
            raise ValueError('Methods differ from the campaign manifest')
        key = (job['dimension'], job['order'], job['cells'])
        if key in signatures:
            if not np.allclose(signature, signatures[key], rtol=2e-11, atol=1e-13):
                if not a.allow_regenerated_load_variation:
                    raise ValueError('Regenerated loads differ across batches/repeats. Inspect sampling; '
                                     '--allow-regenerated-load-variation retains only within-batch parity.')
                varied_loads.append(job['name'])
        else:
            signatures[key] = signature
        for method, data in rows.items():
            # Keep the baseline batch's three CG samples; tuned comparisons use their own paired CG.
            if method == 'plain' and job['batch'] != 'baseline':
                continue
            label = f"gs2/{batch['outer']}" if method == 'gauss-seidel' else f"cheb{batch['degree']}" if method == 'chebyshev' else method
            cg = rows['plain']
            costs = np.array(sequences[method]['cumulative_seconds'])
            cg_costs = np.array(sequences['plain']['cumulative_seconds'])
            wins = np.flatnonzero(costs < cg_costs) if method != 'plain' else []
            gain = cg['recurring_seconds'] - data['recurring_seconds']
            extra_setup = max(0, data['first_setup_seconds'] - cg['first_setup_seconds'])
            records.append(dict(dimension=job['dimension'], order=job['order'], cells=job['cells'],
                                dofs=int(config['dofs']), particles=int(config['particles']), repeat=job['repeat'],
                                method=label, backend=config['backend'], ranks=int(config['ranks']),
                                quadrature=int(config['quadrature_points']), **data,
                                paired_speedup=cg['total_seconds']/data['total_seconds'],
                                recurring_speedup=cg['recurring_seconds']/data['recurring_seconds'],
                                first_winning_solve=int(wins[0])+1 if len(wins) else 0,
                                final_cumulative_win=bool(len(wins) and costs[-1] < cg_costs[-1]),
                                estimated_break_even=int(np.floor(extra_setup/gain))+1 if gain > 0 else 0,
                                cg_pic_seconds=read_pic_steps(directory / run['name'], int(config['ranks'])) if method == 'plain' else '',
                                directory=os.path.relpath(directory / run['name'], root),
                                campaign=os.path.relpath(source, root)))
    if missing and not a.allow_incomplete:
        raise SystemExit(f'{len(missing)} incomplete/failed jobs: ' + ', '.join(missing))
    if not records:
        raise SystemExit('No successful measurements')
    if len({(r['backend'], r['ranks']) for r in records}) != 1:
        raise ValueError('Cannot pool different backends or rank counts into mesh curves')
    with (root / 'summary.csv').open('w') as f:
        writer = csv.DictWriter(f, fieldnames=list(records[0]))
        writer.writeheader()
        writer.writerows(records)
    for order in sorted({r['order'] for r in records}):
        plot_order(root, order, records)
    sources = [dict(directory=os.path.relpath(source, root), manifest=source_manifest)
               for source, source_manifest in campaigns]
    (root / 'analysis_sources.json').write_text(json.dumps(sources, indent=2) + '\n')
    selection = dict(excluded_meshes=sorted(excluded), reason=a.exclusion_reason, omitted_jobs=omitted)
    (root / 'analysis_selection.json').write_text(json.dumps(selection, indent=2) + '\n')
    report(root, manifest, records, missing, varied_loads, sources, selection)


def plot_order(root, order, records):
    rows = [r for r in records if r['order'] == order]
    dims = sorted({r['dimension'] for r in rows})
    plt.rcParams.update({'figure.dpi': 140, 'axes.spines.top': False, 'axes.spines.right': False})
    fig, axes = plt.subplots(3, len(dims), figsize=(5*len(dims), 10), squeeze=False)
    for col, dim in enumerate(dims):
        sizes = sorted({r['cells'] for r in rows if r['dimension'] == dim})
        for method, label, color in zip(METHODS, LABELS, COLORS):
            groups = [(n, [r for r in rows if r['dimension'] == dim and r['cells'] == n and r['method'] == method]) for n in sizes]
            groups = [(n, group) for n, group in groups if group]
            if not groups:
                continue
            active_sizes, samples = zip(*groups)
            for row, metric in enumerate(['recurring_iterations', 'recurring_seconds', 'recurring_speedup']):
                ax = axes[row, col]
                med = [np.median([r[metric] for r in group]) for group in samples]
                low = [min(r[metric] for r in group) for group in samples]
                high = [max(r[metric] for r in group) for group in samples]
                ax.plot(active_sizes, med, color=color, marker='o', markersize=4, label=label)
                ax.fill_between(active_sizes, low, high, color=color, alpha=.12)
                ax.set_xscale('log', base=2)
                if row < 2:
                    ax.set_yscale('log')
                ax.set_xticks(sizes, [str(n) for n in sizes])
                ax.grid(True, alpha=.2)
        axes[0, col].set_title(f'{dim}D — P{order}, full reuse')
        axes[0, col].set_ylabel('Iterations / recurring solve')
        axes[1, col].set_ylabel('Seconds / recurring solve')
        axes[2, col].set_ylabel('Speedup over paired CG')
        axes[2, col].set_xlabel('Cells per axis')
        axes[2, col].axhline(1, color='black', linestyle=':', linewidth=1)
        axes[2, col].set_ylim(bottom=0)
    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, loc='upper center', ncols=6, frameon=False)
    fig.tight_layout(rect=(0, 0, 1, .94))
    save(fig, root, f'p{order}_mesh_trends')

    fig, axes = plt.subplots(2, len(dims), figsize=(5*len(dims), 7), squeeze=False)
    for col, dim in enumerate(dims):
        sizes = sorted({r['cells'] for r in rows if r['dimension'] == dim})
        for method, label, color in zip(METHODS, LABELS, COLORS):
            for row, metric in enumerate(['total_seconds', 'first_setup_seconds']):
                selected = [(n, [r for r in rows if r['dimension'] == dim and r['cells'] == n and r['method'] == method]) for n in sizes]
                selected = [(n, group) for n, group in selected if group]
                if not selected:
                    continue
                active_sizes, groups = zip(*selected)
                values = [np.median([r[metric] for r in group]) for group in groups]
                axes[row, col].plot(active_sizes, values, marker='o', markersize=4, color=color, label=label)
                axes[row, col].set_xscale('log', base=2)
                axes[row, col].set_yscale('log')
                axes[row, col].set_xticks(sizes, [str(n) for n in sizes])
                axes[row, col].grid(True, alpha=.2)
        axes[0, col].set_title(f'{dim}D — P{order}')
        axes[0, col].set_ylabel('Seconds for complete replay')
        axes[1, col].set_ylabel('First operator + PC setup (s)')
        axes[1, col].set_xlabel('Cells per axis')
    fig.legend(*axes[0, 0].get_legend_handles_labels(), loc='upper center', ncols=6, frameon=False)
    fig.tight_layout(rect=(0, 0, 1, .96))
    save(fig, root, f'p{order}_setup_and_total')

    fig, axes = plt.subplots(2, 3, figsize=(14, 7), squeeze=False)
    for col, (method, label) in enumerate([('plain', 'CG'), ('jacobi', 'Jacobi'), ('cheb16', 'Chebyshev 16')]):
        for dim, color in zip(dims, ['#2878a8', '#e08c32', '#7856a3']):
            sizes = sorted({r['cells'] for r in rows if r['dimension'] == dim and r['method'] == method})
            groups = [[r for r in rows if r['dimension'] == dim and r['cells'] == n and r['method'] == method] for n in sizes]
            if not groups:
                continue
            dofs = [group[0]['dofs'] for group in groups]
            for row in range(2):
                values = [np.median([r['recurring_seconds']/r['dofs']/(r['recurring_iterations'] if row else 1)
                                     for r in group]) for group in groups]
                axes[row, col].plot(dofs, values, color=color, marker='o', label=f'{dim}D')
                axes[row, col].set_xscale('log'); axes[row, col].set_yscale('log')
                axes[row, col].grid(True, alpha=.2)
        axes[0, col].set_title(label)
        axes[0, col].set_ylabel('Recurring seconds / DOF')
        axes[1, col].set_ylabel('Seconds / (DOF × outer iteration)')
        axes[1, col].set_xlabel('Independent periodic DOFs')
        axes[0, col].legend()
    fig.suptitle(f'P{order}: dimension comparison (all solver work included, not pure matvec timing)')
    fig.tight_layout()
    save(fig, root, f'p{order}_dof_cost')


def save(fig, root, name):
    for extension in ['png', 'pdf']:
        fig.savefig(root / f'{name}.{extension}', bbox_inches='tight')
    plt.close(fig)


def report(root, manifest, records, missing, varied_loads, sources, selection):
    config = manifest['config']
    text = ['# FEM mesh sweep', '',
            f"{'PARTIAL: ' + str(len(missing)) + ' jobs missing/failed. ' if missing else 'All included jobs completed. '}"
            f"{config['repeats']} repetitions, {config['ranks']} rank(s), {records[0]['backend']}; "
            f"P{','.join(map(str, config['orders']))}, {config['family']}, quadrature={config['quadrature']}, "
            f"{config['steps']} timesteps / {config['steps']+1} loads, {config['initial_guess']} guess, full reuse.", '',
            'Recurring cost includes preconditioner applications; complete replay also includes first setup. '
            'Rank totals are summed locally before taking their maximum. Speedup uses the CG replay in '
            'the same batch/repetition; greater than one is faster. Bands show min–max, not confidence intervals.', '',
            'Particle counts, independent DOFs, residuals, work counts, first/recurring times, paired speedups '
            'and observed/modelled crossings are retained in `summary.csv`. Canonical PIC timing is CG only '
            'and includes load capture; no PCG PIC time is inferred from these replay results.', '',
            '| Dimension | Cells/axis | Independent DOFs | Particles |', '|---|---:|---:|---:|']
    for dim, order, n in sorted({(r['dimension'], r['order'], r['cells']) for r in records}):
        r = next(r for r in records if (r['dimension'], r['order'], r['cells']) == (dim, order, n))
        text.append(f"| {dim}D P{order} | {n} | {r['dofs']} | {r['particles']} |")
    text += ['', '## Largest mesh in each dimension', '',
             '| Case | Method | Recurring iterations | Recurring seconds | Paired speedup (median; range) | Complete-sequence final wins |',
             '|---|---|---:|---:|---:|---:|']
    for dim, order in sorted({(r['dimension'], r['order']) for r in records}):
        n = max(r['cells'] for r in records if r['dimension'] == dim and r['order'] == order)
        for method, label in zip(METHODS, LABELS):
            rows = [r for r in records if (r['dimension'], r['order'], r['cells'], r['method']) == (dim, order, n, method)]
            if not rows:
                continue
            speeds = [r['recurring_speedup'] for r in rows]
            wins = sum(r['final_cumulative_win'] for r in rows)
            winning_text = 'baseline' if method == 'plain' else f'{wins}/{len(rows)}'
            text.append(f"| {dim}D P{order}, {n} | {label} | {np.median([r['recurring_iterations'] for r in rows]):.1f} | "
                        f"{np.median([r['recurring_seconds'] for r in rows]):.6g} | "
                        f"{np.median(speeds):.3f}; {min(speeds):.3f}–{max(speeds):.3f} | {winning_text} |")
    for order in sorted({r['order'] for r in records}):
        text += ['', f'![P{order} mesh trends](p{order}_mesh_trends.png)',
                 f'![P{order} setup and total](p{order}_setup_and_total.png)',
                 f'![P{order} cost normalized by DOFs](p{order}_dof_cost.png)']
    text += ['', f"Maximum fresh relative residual: {max(r['max_fresh_residual'] for r in records):.6g}.", '',
             'Source campaign manifests are preserved in `analysis_sources.json`; each source campaign '
             'retains its configurations, binary/runner hashes, commands and job exit statuses. '
             'Raw application files remain in their original attempt directories. '
             'A failed attempt is preserved and excluded, not counted as a fast solver.', '',
             'Changing mesh or dimension changes the discrete load and spectrum. Fixed-particle mesh '
             'sweeps are performance comparisons, not evidence of physical accuracy convergence. '
             'CUDA, MPI and different rank counts can change sampling and reductions; require correctness '
             'checks on each backend before using its timing results.']
    text += ['', 'Source campaigns:', '']
    for source in sources:
        directory = source['directory']
        text += [f'- [{directory}]({directory}/campaign.json)']
    if len({s['manifest']['config']['executable_sha256'] for s in sources}) > 1:
        text += ['', '**Multiple executable hashes:** these campaigns used different binary builds. '
                 'Check the accompanying study report for source compatibility before interpreting '
                 'combined trends; matching configuration alone does not establish equivalence.']
    if missing:
        text += ['', 'Missing/failed jobs: ' + ', '.join(missing)]
    if selection['omitted_jobs']:
        text += ['', f"**Explicitly omitted {len(selection['omitted_jobs'])} planned jobs:** {selection['reason']} "
                 'The original plan remains in the source manifests; `analysis_selection.json` records '
                 'the omitted meshes/jobs. These are not measurements or convergence failures.']
    if varied_loads:
        text += ['', f'{len(varied_loads)} regenerated batch/repeat load sequences differed from the first '
                 'sequence at that mesh. Each candidate still used its paired CG loads. Between-batch '
                 'variation includes different sampled loads; these are not identical-input repeat timings.']
    (root / 'README.md').write_text('\n'.join(text) + '\n')
    (root / 'analysis_versions.json').write_text(json.dumps(dict(numpy=np.__version__, matplotlib=matplotlib.__version__), indent=2) + '\n')


if __name__ == '__main__':
    main()
