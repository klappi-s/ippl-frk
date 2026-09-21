#!/usr/bin/env python3
"""Sequential, rotated repeated-load comparisons; run after builds have finished."""
import argparse
import hashlib
import json
import subprocess
import shlex
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--executable',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--mpiexec',default='mpiexec')
p.add_argument('--launcher',help='Shell-quoted launch prefix, e.g. "srun -n {ranks}"; parsed without a shell')
p.add_argument('--ranks',type=int,default=1)
p.add_argument('--repeats',type=int,default=3)
p.add_argument('--repeat-offset',type=int,default=0)
p.add_argument('--orders',type=int,nargs='+',default=[2,3])
p.add_argument('--cells',type=int,default=128)
p.add_argument('--particles',type=int,default=32768)
p.add_argument('--dimension',type=int,default=1,choices=[1,2,3])
p.add_argument('--family',default='gll',choices=['gll','equispaced'])
p.add_argument('--initial-guess',default='previous',choices=['zero','previous'])
p.add_argument('--quadrature',default='9',choices=['9','exact'])
p.add_argument('--lengths',default='12.566370614359172')
p.add_argument('--steps',type=int,default=10)
p.add_argument('--tolerance',type=float,default=1e-10)
p.add_argument('--max-iterations',type=int,default=10000)
p.add_argument('--degree',type=int,default=8)
p.add_argument('--methods',nargs='+',default=['plain','jacobi','gauss-seidel','chebyshev'],
               choices=['plain','jacobi','newton','gauss-seidel','ssor','chebyshev','richardson','richardson_alt'])
p.add_argument('--richardson-iterations',type=int,default=4)
p.add_argument('--gs-outer',type=int,default=2)
p.add_argument('--timer-level',choices=['coarse','detailed'],default='detailed')
p.add_argument('--reuse',nargs='+',default=['off','operator','full'],choices=['off','operator','full'])
a=p.parse_args()
if min(a.ranks,a.repeats,a.particles,a.cells,a.max_iterations)<1 or a.steps<0 or a.repeat_offset<0 or not 0<a.tolerance<1:
    p.error('Invalid size, repeat count, steps, or solver tolerance.')
a.executable=a.executable.resolve();a.output=a.output.resolve()
launch=shlex.split(a.launcher.format(ranks=a.ranks)) if a.launcher is not None else [a.mpiexec,'-n',str(a.ranks)]
a.output.mkdir(parents=True,exist_ok=False)
methods=a.methods
if 'plain' not in methods:raise SystemExit('Include plain as the timing baseline.')
records=[]
for order in a.orders:
    for repeat in range(a.repeat_offset,a.repeat_offset+a.repeats):
        rotated=methods[repeat%len(methods):]+methods[:repeat%len(methods)]
        modes=a.reuse[repeat%len(a.reuse):]+a.reuse[:repeat%len(a.reuse)]
        for reuse in modes:
            name=f'p{order}_{reuse}_r{repeat}'
            command=launch+[str(a.executable),'--dim',str(a.dimension),'--order',str(order),
                     '--cells',str(a.cells),'--particles',str(a.particles),'--steps',str(a.steps),
                     '--lengths',a.lengths,'--interpolation_nodes',a.family,'--initial_guess',a.initial_guess,
                     '--quadrature_points',a.quadrature,'--reuse',reuse,'--timer_level',a.timer_level,'--replay',','.join(rotated),'--chebyshev_degree',str(a.degree),
                     '--richardson_iterations',str(a.richardson_iterations),
                     '--tolerance',str(a.tolerance),'--max_iterations',str(a.max_iterations),
                     '--gauss_seidel_outer_iterations',str(a.gs_outer),'--output',str(a.output/name)]
            print(name,flush=True)
            with (a.output/f'{name}.log').open('w') as log:
                result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=False)
            records.append(dict(name=name,order=order,reuse=reuse,repeat=repeat,
                                command=command,returncode=result.returncode))
            (a.output/'runs.json').write_text(json.dumps(records,indent=2)+'\n')
            if result.returncode:
                raise SystemExit(f'{name} failed: inspect its log; no successful timing inferred.')
(a.output/'provenance.json').write_text(json.dumps({
    'executable':str(a.executable),'executable_sha256':hashlib.sha256(a.executable.read_bytes()).hexdigest(),
    'runner_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    'launcher':launch,'tolerance':a.tolerance,'max_iterations':a.max_iterations,'repeat_offset':a.repeat_offset,
    'methods':methods,'richardson_iterations':a.richardson_iterations,'ranks':a.ranks,'repeats':a.repeats,'orders':a.orders,'cells':a.cells,'particles':a.particles,
    'dimension':a.dimension,'family':a.family,'initial_guess':a.initial_guess,'quadrature':a.quadrature,'lengths':a.lengths,
    'steps':a.steps,'timer_level':a.timer_level,'chebyshev_degree':a.degree,'gauss_seidel_outer':a.gs_outer},indent=2)+'\n')
