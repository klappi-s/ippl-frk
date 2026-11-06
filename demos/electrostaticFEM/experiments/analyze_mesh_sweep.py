#!/usr/bin/env python3
"""Recheck ordinary campaigns and emit CSVs; optional plots, no generated Markdown."""
import argparse
import csv
import json
from pathlib import Path
import statistics
from ordinary_measurements import inspect_run

def write_csv(path, rows):
    if not rows: return
    with path.open('w') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(dict.fromkeys(k for row in rows for k in row)))
        writer.writeheader(); writer.writerows(rows)

def analyze(roots, output):
    records, missing, seen, configurations = [], [], set(), {}
    manifests = sorted({p for root in roots for p in root.rglob('campaign.json')
                        if 'allocations' not in p.relative_to(root).parts})
    for path in manifests:
        manifest = json.loads(path.read_text())
        if manifest.get('schema') != 2: raise ValueError('Legacy/incompatible campaign: '+str(path))
        c = manifest['config']
        key = c['application']
        contract = {k: c[k] for k in ['executable_sha256', 'driver_sha256', 'reader_sha256', 'steps',
                    'tolerance', 'dt', 'max_iterations', 'expected_backend', 'variant_definitions']}
        contract['rank_monitor_sha256'] = c.get('rank_monitor_sha256')
        if key in configurations and configurations[key] != contract:
            raise ValueError('Incompatible binary/numerical/measurement policy for '+key)
        configurations[key] = contract
        for job in manifest['jobs']:
            identity = (key, c['ranks'], job['dimension'], job['order'], job['cells'], job['particles'], job['variant'], job['repeat'])
            # Two GPUs on two nodes are a separate placement experiment.
            states = path.parent/'jobs'/(job['name']+'.json')
            nodes, per_node = c.get('nodes'), c.get('gpus_per_node')
            if nodes is not None:
                if nodes*per_node != c['ranks']: raise ValueError('Invalid recorded placement: '+str(path))
                placement = '%dx%d' % (nodes, per_node)
            else:
                # Older ordinary runs did not record this in the manifest.
                evidence = list((path.parent/'allocations').glob('*/placement.json'))
                if evidence:
                    hosts = {r['host'] for r in json.loads(evidence[0].read_text())}
                    nodes, per_node = len(hosts), c['ranks']//len(hosts)
                    placement = '%dx%d' % (nodes, per_node)
                else:
                    placement, nodes = str(c['ranks'])+'r-unknown-placement', None
            identity += (placement,)
            if identity in seen: raise ValueError('Duplicate measurement: '+str(identity))
            seen.add(identity)
            state = json.loads(states.read_text()) if states.exists() else {}
            if state.get('returncode') != 0 or state.get('validation_error'):
                missing.append(dict(campaign=str(path.parent), job=job['name'],
                                    reason=state.get('validation_error') or str(state.get('returncode', 'unrecorded'))))
                continue
            measurement = inspect_run(path.parent/state['directory'], job, c)
            records.append(dict(application=key, placement=placement, ranks=c['ranks'], dimension=job['dimension'],
                order=job['order'], cells=job['cells'], particles=job['particles'], variant=job['variant'], repeat=job['repeat'],
                nodes=nodes, gpus_per_node=per_node,
                application_wall_seconds=state.get('wall_seconds'),
                reserved_node_seconds=nodes*state['wall_seconds'] if nodes and 'wall_seconds' in state else None,
                allocation=state.get('allocation', 'unknown'), directory=str(path.parent/state['directory']), **measurement))
    planned = sorted({p for root in roots for p in root.glob('plans/*.json') if not p.name.endswith('-submission.json')})
    for plan_path in planned:
        plan = json.loads(plan_path.read_text())
        for task in plan.get('tasks', []):
            directory = plan_path.parent.parent/plan['placement']/task['name']
            if not (directory/'campaign.json').exists():
                missing.append(dict(campaign=str(directory), job=task['name'], reason='not started'))
    if not manifests and not planned: raise ValueError('No ordinary campaign manifests or plans found')
    means = []
    keys = ['application', 'placement', 'ranks', 'dimension', 'order', 'cells', 'particles', 'variant']
    for identity in sorted({tuple(r[k] for k in keys) for r in records}):
        group = [r for r in records if tuple(r[k] for k in keys) == identity]
        row = dict(zip(keys, identity)); row['n'] = len(group)
        for metric in ['recurring_solve_seconds', 'recurring_step_seconds', 'recurring_iterations', 'setup_seconds',
                       'recurring_diagnostic_seconds', 'initialization_seconds', 'initial_solve_seconds',
                       'application_wall_seconds', 'reserved_node_seconds',
                       'sampled_device_peak_gib', 'host_process_peak_gib']:
            v = [r[metric] for r in group if r.get(metric) is not None]
            row[metric] = statistics.mean(v) if v else None
            row[metric+'_min'], row[metric+'_max'] = (min(v), max(v)) if v else (None, None)
        # Compare ordinary runs within the same mesh/repetition/allocation.
        ratios = []
        for r in group:
            controls = [b for b in records if all(b[k] == r[k] for k in keys if k != 'variant')
                        and b['variant'] == 'cg' and b['repeat'] == r['repeat'] and b['allocation'] == r['allocation']]
            if controls: ratios.append(controls[0]['recurring_step_seconds']/r['recurring_step_seconds'])
        row['paired_pic_speedup_n'] = len(ratios)
        row['paired_pic_speedup'] = statistics.mean(ratios) if ratios else ''
        means.append(row)
    output.mkdir(parents=True, exist_ok=True)
    write_csv(output/'runs.csv', records); write_csv(output/'means.csv', means)
    (output/'coverage.json').write_text(json.dumps(dict(successful=len(records), missing=missing, contracts=configurations), indent=2)+'\n')
    return means

def plot(means, output):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    groups = sorted({(r['application'], r['dimension'], r['order'], r['placement']) for r in means})
    for app, dim, order, placement in groups:
        rows = [r for r in means if (r['application'], r['dimension'], r['order'], r['placement']) == (app, dim, order, placement)]
        fig, axes = plt.subplots(1, 3, figsize=(13, 4), layout='constrained')
        for variant in sorted({r['variant'] for r in rows}):
            selected = sorted([r for r in rows if r['variant'] == variant], key=lambda r:r['cells'])
            x = [r['cells'] for r in selected]
            counts = sorted({r['n'] for r in selected})
            label = variant + ' (n=' + (str(counts[0]) if len(counts) == 1 else '%d–%d' % (counts[0], counts[-1])) + ')'
            for ax, key, ylabel in zip(axes, ['recurring_iterations', 'recurring_solve_seconds', 'recurring_step_seconds'],
                                      ['Iterations / solve', 'Solve seconds', 'PIC timestep seconds']):
                y = [r[key] for r in selected]
                ax.errorbar(x, y, yerr=[[r[key]-r[key+'_min'] for r in selected],
                    [r[key+'_max']-r[key] for r in selected]], marker='o', label=label)
                ax.set(xscale='log', yscale='log', xlabel='Cells per axis', ylabel=ylabel)
                ax.grid(alpha=.2)
        axes[0].legend(fontsize=8)
        discretization = 'FD' if app == 'alpine' else 'P%d' % order
        fig.suptitle('%s %dD %s, %s: ordinary PIC runs; means and ranges' % (app, dim, discretization, placement))
        for ext in ['png', 'pdf']: fig.savefig(output/('%s-d%d-p%d-%s.%s' % (app, dim, order, placement, ext)), dpi=160)
        plt.close(fig)
    # Allocation comparison preserves distinct placements with equal GPU counts.
    for app, dim, order, cells in sorted({(r['application'], r['dimension'], r['order'], r['cells']) for r in means}):
        rows = sorted([r for r in means if (r['application'], r['dimension'], r['order'], r['cells']) ==
                       (app, dim, order, cells) and r['variant'] == 'cg'], key=lambda r:(r['ranks'], r['placement']))
        if not rows: continue
        fig, axes = plt.subplots(1, 3, figsize=(14, 4), layout='constrained')
        for ax, metric, title in zip(axes, ['recurring_solve_seconds', 'recurring_step_seconds', 'reserved_node_seconds'],
                                    ['Solve seconds', 'PIC timestep seconds', 'Application node-seconds']):
            values = [r[metric] for r in rows]
            if any(v is None for v in values): continue
            ax.errorbar(range(len(rows)), values, yerr=[[r[metric]-r[metric+'_min'] for r in rows],
                        [r[metric+'_max']-r[metric] for r in rows]], fmt='o')
            ax.set_xticks(range(len(rows)), ['%s (n=%d)' % (r['placement'], r['n']) for r in rows], rotation=45)
            ax.set(ylabel=title, yscale='log'); ax.grid(alpha=.2)
        discretization = 'FD' if app == 'alpine' else 'P%d' % order
        fig.suptitle('%s %dD %s %d³: CG allocation comparison' % (app, dim, discretization, cells))
        for ext in ['png', 'pdf']: fig.savefig(output/('%s-d%d-p%d-n%d-allocations.%s' % (app, dim, order, cells, ext)), dpi=160)
        plt.close(fig)

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('roots', type=Path, nargs='+')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--plots', action='store_true')
    a = p.parse_args()
    means = analyze(a.roots, a.output)
    if a.plots: plot(means, a.output)
    print('Wrote', len(means), 'means to', a.output)
