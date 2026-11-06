#!/usr/bin/env bash
# Resources, account and uenv may be overridden by submit_campaigns.py / sbatch.
#SBATCH --job-name=pic-performance
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --ntasks-per-node=1
#SBATCH --gpus-per-task=1
#SBATCH --cpus-per-task=72
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=08:00:00
#SBATCH --signal=B:TERM@60

set -euo pipefail
: "${FEM_ROOT:?Set FEM_ROOT to the directory containing ippl-frk and build_cuda}"
experiments=${FEM_EXPERIMENTS:-$FEM_ROOT/ippl-frk/demos/electrostaticFEM/experiments}
exec "${FEM_PYTHON:-python3}" "$experiments/campaign_task.py" "$1"
