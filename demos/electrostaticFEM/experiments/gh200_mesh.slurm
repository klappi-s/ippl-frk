#!/usr/bin/env bash
# Submit from an allocated/available GH200 software environment; account and uenv
# are supplied by the caller. See MESH_STUDY.md before the first GPU run.
#SBATCH --job-name=fem-mesh
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=4
#SBATCH --gpus-per-task=1
#SBATCH --cpus-per-task=72
#SBATCH --exclusive
#SBATCH --time=02:00:00
#SBATCH --output=fem-mesh-%j.log

set -euo pipefail
: "${FEM_EXPERIMENTS:?Set FEM_EXPERIMENTS to the absolute demos/electrostaticFEM/experiments directory}"
: "${FEM_EXECUTABLE:?Set FEM_EXECUTABLE to the absolute CUDA LandauDampingFEM executable}"
: "${FEM_OUTPUT:?Set FEM_OUTPUT to a new absolute campaign directory on shared scratch}"
: "${SLURM_NTASKS:?Submit this script through Slurm}"

export OMP_NUM_THREADS="${FEM_OMP_THREADS:-1}"
# Slurm provides one visible GPU per rank. Do not replace CUDA_VISIBLE_DEVICES
# with a global device index after Slurm has bound/restricted devices.
launcher="${FEM_LAUNCHER:-srun -n {ranks} --cpu-bind=cores}"
python_command="${FEM_PYTHON:-python3}"

# Optional command-line sweep arguments override defaults, e.g.
# sbatch ... gh200_mesh.slurm --cells-3d 32 48 64 --particles-per-cell 32
# Each replay batch uses all allocated ranks; batches run sequentially.
"$python_command" "$FEM_EXPERIMENTS/run_mesh_sweep.py" \
    --executable "$FEM_EXECUTABLE" --output "$FEM_OUTPUT" \
    --dimensions 3 --orders 3 --cells-3d 16 24 32 --particles-per-cell 64 \
    --ranks "$SLURM_NTASKS" --launcher "$launcher" --resume "$@"

# Plotting can instead be done after downloading the results. It needs NumPy
# and Matplotlib, whereas run_mesh_sweep.py/run_reuse.py use only Python's stdlib.
if [[ "${FEM_ANALYZE:-0}" == 1 ]]; then
    "$python_command" "$FEM_EXPERIMENTS/analyze_mesh_sweep.py" "$FEM_OUTPUT"
fi
