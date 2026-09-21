#!/usr/bin/env python3
"""Small end-to-end checks of dimension/order/family/geometry/MPI dispatch."""
import argparse
import csv
import json
from pathlib import Path
import subprocess
import numpy as np

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--executable',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
p.add_argument('--mpiexec',default='mpiexec');p.add_argument('--reuse-runs',action='store_true');a=p.parse_args()
a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=a.reuse_runs);a.executable=a.executable.resolve()
records=[]
# P4/P5 early; both families; stretched transverse geometry; both quadratures.
cases=[(1,1,'gll','exact',1),(1,4,'gll','exact',2),(1,5,'equispaced','exact',2),
       (2,3,'gll','exact',2),(2,5,'equispaced','exact',1),
       (3,1,'gll','9',2),(3,3,'equispaced','exact',2),(3,5,'gll','exact',1),
       (1,3,'gll','9',1),(1,3,'gll','exact',1)]
for dim,order,family,quad,ranks in cases:
    name=f'd{dim}_p{order}_{family}_q{quad}_r{ranks}'
    lengths=','.join(str(v) for v in [4*np.pi,7,9][:dim])
    command=[a.mpiexec,'-n',str(ranks),str(a.executable),'--dim',str(dim),'--order',str(order),
             '--cells','4','--lengths',lengths,'--particles','512','--steps','2',
             '--interpolation_nodes',family,'--quadrature_points',quad,'--reuse','full',
             '--replay','plain,jacobi','--output',str(a.output/name)]
    print(name,flush=True)
    if not a.reuse_runs:
        with (a.output/f'{name}.log').open('w') as log:
            subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
    for file in (a.output/name).glob('solves-rank*.csv'):
        rows=list(csv.DictReader(file.open()))
        assert len(rows)==9 and all(r['success']=='1' for r in rows)
    records.append(dict(name=name,command=command))
    (a.output/'runs.json').write_text(json.dumps(records,indent=2)+'\n')
arrays=[]
for quad in ['9','exact']:
    data=list(csv.DictReader((a.output/f'd1_p3_gll_q{quad}_r1'/'physics.csv').open()))
    arrays.append(np.array([[float(r[k]) for k in ['Ex_mode_amplitude','field_energy','kinetic_energy']] for r in data]))
# Both rules integrate stiffness and polynomial field energy exactly, so the
# trajectories/energies must agree. The Fourier diagnostic contains cos(k*x),
# hence GL(p+1) is not exact for it; record its quadrature difference separately.
np.testing.assert_allclose(arrays[0][:,1:],arrays[1][:,1:],rtol=1e-9,atol=1e-11)
(a.output/'quadrature_comparison.json').write_text(json.dumps({
    'max_energy_difference':float(np.abs(arrays[0][:,1:]-arrays[1][:,1:]).max()),
    'max_fourier_diagnostic_difference':float(np.abs(arrays[0][:,0]-arrays[1][:,0]).max()),
    'note':'Fourier E*cos(k*x) is nonpolynomial; exact denotes stiffness/mass integration.'},indent=2)+'\n')
# Fail before work/collectives for invalid numeric options and an empty partition.
for index,(ranks,extra) in enumerate([(1,['--cells','-3']),(1,['--tolerance','1e-10garbage']),
                                    (3,['--cells','2']),
                                    (1,['--solver','preconditioned','--preconditioner_type','richardson'])]):
    command=[a.mpiexec,'-n',str(ranks),str(a.executable),'--output',str(a.output/f'rejected{index}')]+extra
    with (a.output/f'rejected{index}.log').open('w') as log:
        result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,timeout=30)
    assert result.returncode!=0,command
# Refusing an existing output must not overwrite the previous CSV on the error path.
protected=a.output/'d1_p3_gll_qexact_r1'
file=protected/'solves-rank0.csv';before=file.read_bytes()
with (a.output/'existing-output.log').open('w') as log:
    result=subprocess.run([a.mpiexec,'-n','1',str(a.executable),'--output',str(protected)],stdout=log,stderr=subprocess.STDOUT)
assert result.returncode!=0 and file.read_bytes()==before
print('All variant, quadrature and rejection checks passed.')
