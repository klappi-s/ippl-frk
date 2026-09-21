#!/usr/bin/env python3
"""Read per-rank timings; export standalone iteration/cost/cumulative/physics plots."""
import argparse
import csv
import json
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);a=p.parse_args()
root=a.directory.resolve();runs=json.loads((root/'runs.json').read_text())
provenance=json.loads((root/'provenance.json').read_text())
records=[];sequences={};iteration_sequences={};source_signatures={}
detailed=provenance.get('timer_level','detailed')=='detailed'
for run in runs:
    if run['returncode']: continue
    directory=root/run['name']; per_rank={}
    for file in sorted(directory.glob('solves-rank*.csv')):
        rows=list(csv.DictReader(file.open()))
        for method in sorted({r['method'] for r in rows if r['phase']=='replay'}):
            selected=[r for r in rows if r['phase']=='replay' and r['method']==method]
            selected.sort(key=lambda r:int(r['solve']))
            if any(r['success']!='1' for r in selected): raise ValueError(f'Failed solve in {file}')
            if [int(r['solve']) for r in selected]!=list(range(len(selected))): raise ValueError('Incomplete solve sequence')
            per_rank.setdefault(method,[]).append(selected)
    for method,ranks in per_rank.items():
        lengths={len(r) for r in ranks}
        if len(lengths)!=1: raise ValueError('Ranks have different solve counts')
        times=np.array([[float(r['solve_seconds']) for r in rank] for rank in ranks])
        # Sum locally before selecting the critical rank; components come from that same rank.
        critical=int(np.argmax(times.sum(axis=1)));rows=ranks[critical]
        number=len(rows)
        signature=np.array([float(r['rhs_norm']) for r in rows])
        source_key=(run['order'],run['repeat'])
        if source_key in source_signatures:
            np.testing.assert_allclose(signature,source_signatures[source_key],rtol=2e-11,atol=1e-13)
        else: source_signatures[source_key]=signature
        def total(key): return sum(float(r[key]) for r in rows)
        operator_setup=total('operator_setup_seconds');pc_setup=total('preconditioner_setup_seconds')
        applications=total('preconditioner_apply_seconds');krylov=total('krylov_seconds')
        solve=total('solve_seconds')
        record={**{k:run[k] for k in ['name','order','reuse','repeat']},'method':method,
                'solves':number,'iterations':sum(int(r['iterations']) for r in rows),
                'solve_seconds':solve,'rank_total_min':float(times.sum(axis=1).min()),
                'rank_total_mean':float(times.sum(axis=1).mean()),'rank_total_max':float(times.sum(axis=1).max()),'first_solve_seconds':max(float(rank[0]['solve_seconds']) for rank in ranks),
                'recurring_seconds':float(np.max(times[:,1:].sum(axis=1)))/(number-1) if number>1 else 0,
                'first_setup_seconds':float(rows[0]['operator_setup_seconds'])+float(rows[0]['preconditioner_setup_seconds']),
                'operator_setup_seconds':operator_setup,'preconditioner_setup_seconds':pc_setup,
                'preconditioner_apply_seconds':applications,'krylov_other_seconds':krylov-applications,
                'solver_other_seconds':solve-operator_setup-pc_setup-krylov,
                'operator_setups':total('operator_setups'),'preconditioner_setups':total('preconditioner_setups'),
                'max_fresh_residual':max(float(r['fresh_relative_residual']) for r in rows)}
        if min(record['krylov_other_seconds'],record['solver_other_seconds']) < -1e-6:
            raise ValueError('Inconsistent timer nesting')
        expected_operator=number if run['reuse']=='off' else 1
        expected_preconditioner=0 if method=='plain' else (1 if run['reuse']=='full' else number)
        if (record['operator_setups']!=expected_operator
                or record['preconditioner_setups']!=expected_preconditioner):
            raise ValueError('Unexpected setup count during fixed-operator replay')
        records.append(record)
        sequences[(run['order'],run['reuse'],method,run['repeat'])]=times.cumsum(axis=1).max(axis=0)
        iteration_sequences[(run['order'],run['reuse'],method,run['repeat'])]=np.array([int(r['iterations']) for r in rows])
with (root/'summary.csv').open('w') as f:
    writer=csv.DictWriter(f,fieldnames=list(records[0]));writer.writeheader();writer.writerows(records)
orders=sorted({r['order'] for r in records})
modes=[m for m in ['off','operator','full'] if any(r['reuse']==m for r in records)]
comparison_mode='full' if 'full' in modes else modes[-1]
method_order=['plain','jacobi','gauss-seidel','chebyshev','newton','ssor','richardson','richardson_alt']
methods=[m for m in method_order if any(r['method']==m for r in records)]
colors=dict(zip(method_order,['#343a40','#277da8','#e08b31','#5b8c45','#9467bd','#8c564b','#d62728','#e377c2']))
plt.rcParams.update({'figure.dpi':130,'axes.spines.top':False,'axes.spines.right':False})
def save(fig,name):
    fig.tight_layout()
    fig.savefig(root/f'{name}.png',bbox_inches='tight')
    fig.savefig(root/f'{name}.pdf',bbox_inches='tight')
    plt.close(fig)
def subset(order,reuse,method): return [r for r in records if r['order']==order and r['reuse']==reuse and r['method']==method]
fig,axes=plt.subplots(len(orders),1,figsize=(9,3.3*len(orders)),squeeze=False)
for ax,order in zip(axes[:,0],orders):
    for mi,method in enumerate(methods):
        values=[np.median([r['iterations'] for r in subset(order,reuse,method)]) for reuse in modes]
        ax.bar(np.arange(len(modes))+(mi-(len(methods)-1)/2)*(.8/len(methods)),values,width=.8/len(methods),label=method,color=colors[method])
    ax.set_xticks(range(len(modes)),modes);ax.set_ylabel('Total iterations');ax.set_title(f'P{order}: identical-load replay');ax.legend(ncols=4)
save(fig,'iterations')
fig,axes=plt.subplots(len(orders),1,figsize=(10,3.7*len(orders)),squeeze=False)
components=['operator_setup_seconds','preconditioner_setup_seconds','preconditioner_apply_seconds','krylov_other_seconds','solver_other_seconds']
labels=['Operator setup','Preconditioner setup','Preconditioner applications' if detailed else 'Applications (not separately timed)',
        'Other Krylov' if detailed else 'Krylov, including applications','Other solver']
for ax,order in zip(axes[:,0],orders):
    groups=[(reuse,method) for reuse in modes for method in methods];bottom=np.zeros(len(groups))
    for component,label in zip(components,labels):
        values=np.array([np.mean([r[component] for r in subset(order,reuse,method)]) for reuse,method in groups])
        ax.bar(range(len(groups)),values,bottom=bottom,label=label);bottom+=values
    ax.set_xticks(range(len(groups)),[f'{r}\n{m}' for r,m in groups],rotation=30,ha='right')
    ax.set_ylabel('Seconds, mean of repeats');ax.set_title(f'P{order}: complete replay cost, disjoint components')
    ax.set_ylim(0,float(bottom.max())*1.3);ax.legend(fontsize=8,ncols=3,loc='upper left')
save(fig,'solver_cost')
fig,axes=plt.subplots(len(orders),1,figsize=(9,3.5*len(orders)),squeeze=False)
for ax,order in zip(axes[:,0],orders):
    for method in methods:
        values=np.array([v for (p,reuse,m,rep),v in sequences.items() if p==order and reuse==comparison_mode and m==method])
        x=np.arange(1,values.shape[1]+1);ax.plot(x,np.median(values,axis=0),label=method,color=colors[method])
        ax.fill_between(x,values.min(axis=0),values.max(axis=0),color=colors[method],alpha=.15)
    ax.set_title(f'P{order}: reuse={comparison_mode}, including first setup');ax.set_xlabel('Poisson solves');ax.set_ylabel('Cumulative seconds');ax.legend()
save(fig,'cumulative')
fig,axes=plt.subplots(len(orders),1,figsize=(9,3.3*len(orders)),squeeze=False)
for ax,order in zip(axes[:,0],orders):
    peak_iterations=0
    for method in methods:
        values=np.array([v for (p,reuse,m,rep),v in iteration_sequences.items() if p==order and reuse==comparison_mode and m==method])
        peak_iterations=max(peak_iterations,float(values.max()))
        x=np.arange(values.shape[1]);ax.plot(x,np.median(values,axis=0),label=method,color=colors[method],marker='.')
        ax.fill_between(x,values.min(axis=0),values.max(axis=0),color=colors[method],alpha=.15)
    ax.set_title(f'P{order}: reuse={comparison_mode}, iterations for each captured load');ax.set_xlabel('Solve (0 = initial)');ax.set_ylabel('CG/PCG iterations')
    ax.set_ylim(0,peak_iterations*1.35);ax.legend(ncols=4,fontsize=8,loc='upper left')
save(fig,'iterations_by_solve')
reference=root/runs[0]['name'];physics=list(csv.DictReader((reference/'physics.csv').open()))
fig,axes=plt.subplots(1,3,figsize=(12,3.4));t=[float(r['time']) for r in physics]
for ax,key,label in zip(axes,['Ex_mode_amplitude','relative_energy_drift','net_charge'],['Fundamental E amplitude','Relative energy drift','Net charge']):
    ax.plot(t,[float(r[key]) for r in physics],marker='.');ax.set_xlabel('Time');ax.set_ylabel(label)
save(fig,'physics')
text=['# LandauDampingFEM repeated-load measurements','',
      f"{provenance.get('dimension',1)}D, {provenance['cells']} cells/axis, {provenance['particles']} particles, {provenance['ranks']} rank(s); {provenance.get('family','gll')}, GL{provenance.get('quadrature','9')}, {provenance.get('initial_guess','previous')} guess, {provenance.get('timer_level','detailed')} timers.",
      f"Chebyshev degree {provenance['chebyshev_degree']}; GS inner 2 / outer {provenance['gauss_seidel_outer']}.",'',
      'All methods replay the same captured load sequence within each run. Timing bands show the range of repeats.',
      'The complete solver cost includes setup and applications; diagnostic residual checks are separate.','',
      f'| Order | Method, reuse={comparison_mode} | Iterations | Total seconds (median) | Recurring seconds/solve (median) | Observed cumulative crossing vs CG |',
      '|---|---|---:|---:|---:|---|']
for order in orders:
    for method in methods:
        rows=subset(order,comparison_mode,method);crossings=[]
        for row in rows:
            key=(order,comparison_mode,method,row['repeat']);cg=(order,comparison_mode,'plain',row['repeat'])
            winning=np.flatnonzero(sequences[key]<sequences[cg]) if method!='plain' else []
            crossings.append(str(int(winning[0])+1) if len(winning) else 'none')
        text.append(f"| P{order} | {method} | {np.median([r['iterations'] for r in rows]):.0f} | {np.median([r['solve_seconds'] for r in rows]):.6g} | {np.median([r['recurring_seconds'] for r in rows]):.6g} | {', '.join(crossings) if method!='plain' else 'baseline'} |")
text += ['', '| Order | Method | Extra first setup S (s) | Recurring C−P (s) | Estimated first winning solve |',
         '|---|---|---:|---:|---|']
for order in orders:
    cg=subset(order,comparison_mode,'plain')
    c=np.median([r['recurring_seconds'] for r in cg])
    setup=np.median([r['first_setup_seconds'] for r in cg])
    for method in methods:
        if method=='plain':continue
        rows=subset(order,comparison_mode,method)
        extra=max(0,np.median([r['first_setup_seconds'] for r in rows])-setup)
        gain=c-np.median([r['recurring_seconds'] for r in rows])
        estimate=str(int(np.floor(extra/gain))+1) if gain>0 else 'no amortization benefit'
        text.append(f'| P{order} | {method} | {extra:.6g} | {gain:.6g} | {estimate} |')
text += ['', 'The estimate assumes stationary recurring cost; the measured cumulative curves include the actual changing loads.',
         '', 'A first crossing need not persist. “None” means none observed in this sequence; it is not an extrapolated claim.',
         '', '![Iterations](iterations.png)','![Iterations by solve](iterations_by_solve.png)','![Solver cost](solver_cost.png)','![Cumulative cost](cumulative.png)',
         '![Physical diagnostics](physics.png)','![PIC stages](pic_stages.png)','', 'Raw runs, commands and executable hash are retained alongside `summary.csv`.']
(root/'README.md').write_text('\n'.join(text)+'\n')
(root/'analysis_versions.json').write_text(json.dumps({'numpy':np.__version__,'matplotlib':matplotlib.__version__},indent=2)+'\n')

# Complete PIC cost comes from direct step_total intervals; children are disjoint.
stages=['kick','drift','particle_update','deposition','background','solve','gather','residual_check','diagnostics','capture_load']
fig,axes=plt.subplots(len(orders),1,figsize=(10,3.6*len(orders)),squeeze=False)
for ax,order in zip(axes[:,0],orders):
    totals={m:[] for m in modes};parts={m:[] for m in modes}
    for run in runs:
        if run['order']!=order or run['returncode']:continue
        rank_data=[]
        for file in sorted((root/run['name']).glob('steps-rank*.csv')):
            entries=list(csv.DictReader(file.open()))
            sums={stage:sum(float(r['seconds']) for r in entries if r['stage']==stage and int(r['step'])>0) for stage in stages+['step_total']}
            rank_data.append(sums)
        critical=max(rank_data,key=lambda r:r['step_total'])
        count=len({r['step'] for r in entries if int(r['step'])>0})
        totals[run['reuse']].append(critical['step_total']/count)
        parts[run['reuse']].append([critical[stage]/count for stage in stages])
    bottom=np.zeros(len(modes))
    for i,stage in enumerate(stages):
        values=np.array([np.mean(parts[m],axis=0)[i] for m in modes])
        ax.bar(range(len(modes)),values,bottom=bottom,label=stage);bottom+=values
    ax.plot(range(len(modes)),[np.mean(totals[m]) for m in modes],'ko',label='Direct step total')
    ax.set_xticks(range(len(modes)),modes);ax.set_ylabel('Seconds per step');ax.set_title(f'P{order}: canonical plain-CG PIC, mean across repeats')
    ax.set_ylim(0,float(bottom.max())*1.4);ax.legend(ncols=4,fontsize=8,loc='upper left')
save(fig,'pic_stages')
