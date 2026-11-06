#!/usr/bin/env python3
"""Dispatch one immutable Slurm array recipe through the existing allocation driver."""
import json
import fcntl
import hashlib
import os
from pathlib import Path
import sys

def main():
    path = Path(sys.argv[1]).resolve()
    plan = json.loads(path.read_text())
    task = plan['tasks'][int(os.environ['SLURM_ARRAY_TASK_ID'])]
    nodes, per_node = map(int, plan['placement'].split('x'))
    assert int(os.environ['SLURM_NTASKS']) == nodes*per_node
    assert int(os.environ['SLURM_JOB_NUM_NODES']) == nodes
    env = dict(os.environ, FEM_APPLICATION=plan['application'], FEM_OUTPUT=task['output'])
    root = Path(os.environ['FEM_ROOT'])
    experiments = Path(os.environ.get('FEM_EXPERIMENTS', str(root/'ippl-frk/demos/electrostaticFEM/experiments')))
    build = Path(os.environ.get('FEM_BUILD', str(root/'build_cuda')))
    app = Path(os.environ.get('FEM_EXECUTABLE', str(build/('demos/alpine/LandauDamping' if plan['application'] == 'alpine'
                                                        else 'demos/electrostaticFEM/LandauDampingFEM'))))
    wrapper = Path(os.environ.get('WRAPPER', '/user-environment/wrapper-mpi.sh'))
    files = dict(executable=app, wrapper=wrapper, runner=experiments/'run_mesh_sweep.py',
                 reader=experiments/'ordinary_measurements.py', dispatcher=Path(__file__),
                 variant_registry=experiments/'fd_pcg_variants.py',
                 monitor=experiments/'rank_memory.py', allocation_driver=experiments/'gh200_mesh.slurm')
    pin = dict(application=plan['application'], **{k: hashlib.sha256(p.read_bytes()).hexdigest() for k,p in files.items()})
    study = path.parent.parent
    with (study/'study-pin.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        target = study/'study-pin.json'
        if target.exists():
            if json.loads(target.read_text()) != pin:
                raise SystemExit('Study binary/wrapper/runner changed; use a new output root')
        else:
            temporary = target.with_suffix('.tmp')
            temporary.write_text(json.dumps(pin, indent=2)+'\n')
            temporary.replace(target)
    os.execvpe('bash', ['bash', str(experiments/'gh200_mesh.slurm')] + task['arguments']
              + ['--timeout', os.environ.get('FEM_RUN_TIMEOUT', '9000')], env)

if __name__ == '__main__': main()
