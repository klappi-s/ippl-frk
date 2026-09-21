#!/usr/bin/env python3
"""Compare complete ten-step PIC trajectories with equal full solver reuse."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import shlex
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--executable',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
p.add_argument('--mpiexec',default='mpiexec');p.add_argument('--ranks',type=int,default=1)
p.add_argument('--launcher',help='Launch prefix with optional {ranks}; parsed without a shell')
p.add_argument('--dimension',type=int,choices=[1,2,3],default=1)
p.add_argument('--quadrature',choices=['9','exact'],default='9')
p.add_argument('--methods',nargs='+',choices=['plain','jacobi','gauss-seidel','chebyshev'],
               default=['plain','jacobi','gauss-seidel','chebyshev'])
p.add_argument('--repeats',type=int,default=3);p.add_argument('--order',type=int,default=3)
p.add_argument('--cells',type=int,default=128);p.add_argument('--particles',type=int,default=32768)
p.add_argument('--degree',type=int,default=8);p.add_argument('--gs-outer',type=int,default=2)
a=p.parse_args();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=False);a.executable=a.executable.resolve()
methods=a.methods;records=[]
if 'plain' not in methods:raise SystemExit('Include plain as the timing baseline.')
launch=shlex.split(a.launcher.format(ranks=a.ranks)) if a.launcher is not None else [a.mpiexec,'-n',str(a.ranks)]
for repeat in range(a.repeats):
    for method in methods[repeat%len(methods):]+methods[:repeat%len(methods)]:
        name=f'{method}_r{repeat}'
        command=launch+[str(a.executable),'--dim',str(a.dimension),'--quadrature_points',a.quadrature,'--order',str(a.order),'--cells',str(a.cells),
                 '--particles',str(a.particles),'--steps','10','--reuse','full','--solver',
                 'plain' if method=='plain' else 'preconditioned','--preconditioner_type',
                 'jacobi' if method=='plain' else method,'--timer_level','coarse',
                 '--chebyshev_degree',str(a.degree),'--gauss_seidel_outer_iterations',str(a.gs_outer),
                 '--output',str(a.output/name)]
        print(name,flush=True)
        with (a.output/f'{name}.log').open('w') as log:
            subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
        times=[];initialization=[]
        for file in sorted((a.output/name).glob('steps-rank*.csv')):
            rows=list(csv.DictReader(file.open()))
            initialization.append(sum(float(r['seconds']) for r in rows if r['stage']=='initialization'))
            times.append({stage:sum(float(r['seconds']) for r in rows if int(r['step'])>0 and r['stage']==stage)
                          for stage in {r['stage'] for r in rows}})
        critical=max(times,key=lambda r:r['step_total'])
        solves=list(csv.DictReader((a.output/name/'solves-rank0.csv').open()))
        assert len(solves)==11 and all(r['success']=='1' for r in solves)
        records.append(dict(name=name,method=method,repeat=repeat,command=command,
                            step_seconds=critical['step_total'],solve_seconds=critical['solve'],
                            initialization_seconds=max(initialization),
                            iterations=sum(int(r['iterations']) for r in solves),stages=critical))
        (a.output/'runs.json').write_text(json.dumps(records,indent=2)+'\n')
stages=['kick','drift','particle_update','deposition','background','solve','gather','residual_check','diagnostics']
fig,ax=plt.subplots(figsize=(9,4));bottom=np.zeros(len(methods))
for stage in stages:
    values=np.array([np.mean([r['stages'].get(stage,0) for r in records if r['method']==m]) for m in methods])
    ax.bar(range(len(methods)),values,bottom=bottom,label=stage);bottom+=values
means=[];low=[];high=[]
for method in methods:
    values=np.array([r['step_seconds'] for r in records if r['method']==method]);means.append(values.mean())
    low.append(values.mean()-values.min());high.append(values.max()-values.mean())
ax.errorbar(range(len(methods)),means,yerr=[low,high],fmt='ko',label='Direct total, min–max')
ax.set_xticks(range(len(methods)),methods);ax.set_ylabel('Seconds for ten steps');ax.set_title(f'{a.dimension}D P{a.order}: complete PIC cost, full reuse, {a.ranks} rank(s)')
ax.set_ylim(0,max(m+h for m,h in zip(means,high))*1.4)
ax.legend(ncols=3,fontsize=8,loc='upper left');fig.tight_layout();fig.savefig(a.output/'pic.png',dpi=160);fig.savefig(a.output/'pic.pdf')
text=['# Full PIC comparison','',
      f'{a.dimension}D P{a.order}, {a.cells} cells/axis, {a.particles} particles, quadrature={a.quadrature}; Chebyshev degree {a.degree}, GS 2/{a.gs_outer}.',
      'Same initial samples/configuration, separate trajectories. Coarse timers; full reuse. Initial field solve is excluded from ten-step cost.',
      'Bars show mean disjoint components; direct totals and min/max repeats are overlaid.','',
      '| Method | Total iterations (11 solves, median) | Ten-step seconds (median) | Min–max seconds |','|---|---:|---:|---:|']
for method in methods:
    rows=[r for r in records if r['method']==method];values=[r['step_seconds'] for r in rows]
    text.append(f"| {method} | {np.median([r['iterations'] for r in rows]):.0f} | {np.median(values):.6g} | {min(values):.6g}–{max(values):.6g} |")
text+=['','![PIC costs](pic.png)']
(a.output/'README.md').write_text('\n'.join(text)+'\n')
(a.output/'provenance.json').write_text(json.dumps({'executable_sha256':hashlib.sha256(a.executable.read_bytes()).hexdigest(),
                                                  'dimension':a.dimension,'order':a.order,'cells':a.cells,'particles':a.particles,
                                                  'quadrature':a.quadrature,'degree':a.degree,'gs_outer':a.gs_outer,'ranks':a.ranks,'launcher':launch,
                                                  'numpy':np.__version__,'matplotlib':matplotlib.__version__},indent=2)+'\n')
