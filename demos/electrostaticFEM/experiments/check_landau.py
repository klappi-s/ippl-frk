#!/usr/bin/env python3
"""Run timestep/mesh/particle sensitivity checks against a kinetic dispersion root.

Requires NumPy, SciPy and Matplotlib. This measures an early-time amplitude
rate; it does not turn a noisy PIC realization into an exact oracle.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import numpy as np
import scipy
from scipy.optimize import root
from scipy.special import wofz
from scipy.signal import find_peaks
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--executable',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--mpiexec',default='mpiexec')
p.add_argument('--particles',type=int,default=524288)
p.add_argument('--analyze-only',action='store_true')
a=p.parse_args();a.output=a.output.resolve();a.executable=a.executable.resolve()
a.output.mkdir(parents=True,exist_ok=a.analyze_only)
# Normalization: omega_p=1, v_th=standard deviation=1, k=0.5.
# Z(zeta)=i*sqrt(pi)*wofz(zeta), epsilon=1+(1+zeta*Z)/(k*v_th)^2.
# Sources: Fitzpatrick, Plasma Dispersion Function, and SciPy's wofz documentation.
# https://farside.ph.utexas.edu/teaching/plasma/Plasmahtml/node92.html
# https://docs.scipy.org/doc/scipy/reference/generated/scipy.special.wofz.html
k=0.5

def dielectric(w):
    z=(w[0]+1j*w[1])/(np.sqrt(2)*k)
    value=1+(1+z*1j*np.sqrt(np.pi)*wofz(z))/(k*k)
    return [value.real,value.imag]
solution=root(dielectric,[1.4,-0.15],tol=1e-12)
if not solution.success or np.linalg.norm(dielectric(solution.x))>1e-10:
    raise RuntimeError('Dispersion root did not converge')
omega,gamma=solution.x
cases=[('baseline',64,0.05,a.particles),('half_dt',64,0.025,a.particles),
       ('double_cells',128,0.05,a.particles),('double_particles',64,0.05,2*a.particles)]
records=[]
fig,axes=plt.subplots(1,2,figsize=(12,4))
for index,(name,cells,dt,particles) in enumerate(cases):
    command=[a.mpiexec,'-n','1',str(a.executable),'--dim','1','--order','3','--cells',str(cells),
             '--dt',str(dt),'--particles',str(particles),'--steps',str(round(12/dt)),
             '--quadrature_points','exact','--reuse','full','--output',str(a.output/name)]
    print(name,flush=True)
    if not a.analyze_only:
        with (a.output/f'{name}.log').open('w') as log:
            subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
    data=list(csv.DictReader((a.output/name/'physics.csv').open()))
    t=np.array([float(r['time']) for r in data]);amplitude=np.array([float(r['Ex_mode_amplitude']) for r in data])
    peaks,_=find_peaks(amplitude,distance=max(1,int(.7*np.pi/omega/dt)),prominence=.03*amplitude.max())
    peaks=peaks[(t[peaks]>=1.5)&(t[peaks]<=10)]
    if len(peaks)<3:raise RuntimeError('Insufficient resolved peaks for early-time fit')
    slope,intercept=np.polyfit(t[peaks],np.log(amplitude[peaks]),1)
    drift=np.array([float(r['relative_energy_drift']) for r in data])
    charge=max(abs(float(r['net_charge'])) for r in data)
    energy_difference=max(abs(float(r['field_energy'])-float(r['weak_field_energy'])) for r in data)
    records.append(dict(name=name,cells=cells,dt=dt,particles=particles,command=command,
                        fitted_amplitude_rate=float(slope),reference_rate=float(gamma),
                        relative_rate_error=float(abs(slope/gamma-1)),
                        peak_times=t[peaks].tolist(),peak_amplitudes=amplitude[peaks].tolist(),
                        max_abs_energy_drift=float(abs(drift).max()),max_abs_charge=charge,
                        max_field_energy_difference=energy_difference))
    color=plt.get_cmap('tab10')(index)
    axes[0].semilogy(t,amplitude,label=name,color=color);axes[0].plot(t[peaks],amplitude[peaks],'o',ms=3,color=color)
    axes[1].plot(t,drift,label=name,color=color)
    if name=='baseline':axes[0].semilogy(t,amplitude[peaks[0]]*np.exp(gamma*(t-t[peaks[0]])),'k--',label='Reference slope, anchored at first peak')
    (a.output/'results.json').write_text(json.dumps(records,indent=2)+'\n')
axes[0].set(xlabel='Time',ylabel='Fundamental electric amplitude',title='Early-time damping; markers are fitted peaks')
axes[1].set(xlabel='Time',ylabel='Relative total-energy drift',title='Particle + FEM field energy')
for ax in axes:ax.legend(fontsize=8);ax.spines[['top','right']].set_visible(False)
fig.tight_layout();fig.savefig(a.output/'landau.png',dpi=160);fig.savefig(a.output/'landau.pdf')
provenance={'reference_omega':float(omega),'reference_gamma':float(gamma),
            'dispersion_residual':dielectric(solution.x),'numpy':np.__version__,'scipy':scipy.__version__,
            'matplotlib':matplotlib.__version__,'executable_sha256':hashlib.sha256(a.executable.read_bytes()).hexdigest(),
            'script_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
(a.output/'provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')
text=['# Landau damping sensitivity check','',
      f'Independent Maxwellian dispersion root: omega = {omega:.10f}, amplitude damping gamma = {gamma:.10f}.',
      'Rates fit log amplitudes at resolved maxima between t=1.5 and t=10. Energy decay has twice the amplitude rate.',
      'Random sampling and nonlinear effects remain; this is an early-time consistency check, not an exact PIC oracle.','',
      '| Run | Cells | dt | Particles | Fitted gamma | Relative rate error | Maximum energy drift |',
      '|---|---:|---:|---:|---:|---:|---:|']
for r in records:
    text.append(f"| {r['name']} | {r['cells']} | {r['dt']} | {r['particles']} | {r['fitted_amplitude_rate']:.6f} | {r['relative_rate_error']:.1%} | {r['max_abs_energy_drift']:.3g} |")
text+=['','![Damping and energy](landau.png)','',
       'Reference: [Fitzpatrick, plasma dispersion function](https://farside.ph.utexas.edu/teaching/plasma/Plasmahtml/node92.html),',
       '[SciPy Faddeeva function](https://docs.scipy.org/doc/scipy/reference/generated/scipy.special.wofz.html).']
(a.output/'README.md').write_text('\n'.join(text)+'\n')
