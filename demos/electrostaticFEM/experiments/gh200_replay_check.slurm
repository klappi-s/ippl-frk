#!/usr/bin/env bash
# Reuse the established exclusive allocation, wrapper and CUDA preflight.
# Account/uenv are supplied at submission or in the configured cluster-root copy.
#SBATCH --job-name=fem-replay-check
#SBATCH --nodes=2
#SBATCH --ntasks=8
#SBATCH --ntasks-per-node=4
#SBATCH --gpus-per-task=1
#SBATCH --cpus-per-task=72
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=04:00:00
#SBATCH --output=fem-replay-check-%j.log
#SBATCH --error=fem-replay-check-%j.err

{
set -euo pipefail
: "${FEM_ROOT:?Set FEM_ROOT to the cluster directory containing ippl-frk and build_cuda}"
: "${SLURM_JOB_ID:?Submit through Slurm}"
export FEM_REPLAY_CHECK=1
export FEM_REPLAY_CHECK_SCRIPT="${BASH_SOURCE[0]}"
export FEM_OUTPUT=${FEM_OUTPUT:-$FEM_ROOT/gpu_performance/replay-check-$SLURM_JOB_ID}
experiments=${FEM_EXPERIMENTS:-$FEM_ROOT/ippl-frk/demos/electrostaticFEM/experiments}
exec bash "$experiments/gh200_mesh.slurm" "$@"
}
