#!/usr/bin/env bash
#SBATCH --job-name=pic-build
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --gpus-per-task=1
#SBATCH --cpus-per-task=72
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=04:00:00
#SBATCH --output=pic-build-%j.log
#SBATCH --error=pic-build-%j.err
set -euo pipefail
: "${FEM_ROOT:?Set FEM_ROOT to the directory containing ippl-frk and build_cuda}"
build=${FEM_BUILD:-$FEM_ROOT/build_cuda}
[[ -f $build/CMakeCache.txt ]] || { echo 'Configure the CUDA build first.' >&2; exit 2; }
cmake -S "$FEM_ROOT/ippl-frk" -B "$build" -DIPPL_ENABLE_ALPINE=ON -DIPPL_ENABLE_ELECTROSTATIC_FEM=ON
cmake --build "$build" -j 8 --target LandauDampingFEM LandauDamping
export WRAPPER=${WRAPPER:-/user-environment/wrapper-mpi.sh}
experiments=${FEM_EXPERIMENTS:-$FEM_ROOT/ippl-frk/demos/electrostaticFEM/experiments}
launcher="srun --nodes=1 --ntasks=1 --ntasks-per-node=1 --cpus-per-task=72 --gpus-per-task=1 --cpu-bind=cores $WRAPPER"
for application in fem alpine; do
  executable=$build/demos/electrostaticFEM/LandauDampingFEM
  [[ $application != alpine ]] || executable=$build/demos/alpine/LandauDamping
  variants=(cg jacobi gs21 gs22 cheb2 cheb4 cheb8 cheb16)
  if [[ $application == alpine ]]; then
    variants=(cg jacobi newton5 cheb31 rich4c0 richa5 gs22c0 ssor22w157 mg22w080m4c0)
  fi
  python3 "$experiments/run_mesh_sweep.py" --application "$application" --executable "$executable" \
    --output "$FEM_ROOT/gpu_validation/ordinary-build-$SLURM_JOB_ID-$application" \
    --dimensions 3 --orders 2 --cells-3d 4 --particles 512 --steps 10 --repeats 1 \
    --variants "${variants[@]}" --ranks 1 --launcher "$launcher" \
    --expected-backend Cuda --timeout 300
done
