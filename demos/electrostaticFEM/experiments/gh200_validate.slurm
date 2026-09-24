#!/usr/bin/env bash
#SBATCH --job-name=fem-gpu-check
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --gpus-per-task=1
#SBATCH --cpus-per-task=8
#SBATCH --time=01:00:00
#SBATCH --output=fem-gpu-check-%j.log

# Add the account/partition/uenv options used by your Daint setup when submitting.
# For the mpi stage override --ntasks=2 (one GPU per task).
# Across nodes: sbatch --nodes=2 --ntasks=2 --ntasks-per-node=1 gh200_validate.slurm mpi-two-node
set -euo pipefail
: "${FEM_ROOT:?Export FEM_ROOT as the absolute cluster directory containing ippl-frk and build_cuda}"
export FEM_CPUS_PER_TASK=${SLURM_CPUS_PER_TASK:-8}
bash "$FEM_ROOT/gh200_validate.sh" "${1:-all}"
