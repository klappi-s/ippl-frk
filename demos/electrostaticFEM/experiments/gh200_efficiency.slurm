#!/usr/bin/env bash
# Supply account/uenv at submission, or use the configured cluster-root copy.
#SBATCH --job-name=fem-efficiency
#SBATCH --nodes=1
#SBATCH --ntasks=4
#SBATCH --ntasks-per-node=4
#SBATCH --gpus-per-task=1
#SBATCH --cpus-per-task=72
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=04:00:00
#SBATCH --output=fem-efficiency-%j.log
#SBATCH --error=fem-efficiency-%j.err

{
set -euo pipefail
: "${FEM_ROOT:?Set FEM_ROOT to the directory containing ippl-frk and build_cuda}"
: "${SLURM_JOB_ID:?Submit through Slurm}"
[[ ${SLURM_JOB_NUM_NODES:-0}:${SLURM_NTASKS:-0} == 1:4 ]] || {
  echo 'This build/check job requires one node with four GPU ranks.' >&2; exit 2;
}
buildDir=${FEM_BUILD:-$FEM_ROOT/build_cuda}
experiments=$FEM_ROOT/ippl-frk/demos/electrostaticFEM/experiments
mkdir -p "$FEM_ROOT/gpu_validation"
runDir=$(mktemp -d "$FEM_ROOT/gpu_validation/efficiency-${SLURM_JOB_ID}-XXXXXX")
exec > >(tee "$runDir/session.log") 2>&1
trap 'rc=$?; printf "exit_code=%s\n" "$rc" > "$runDir/status.txt"; exit "$rc"' EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
trap 'exit 129' HUP
export OMP_NUM_THREADS=1
export WRAPPER=${WRAPPER:-/user-environment/wrapper-mpi.sh}
[[ -x $WRAPPER ]] || { echo "Missing wrapper: $WRAPPER" >&2; exit 2; }
echo "Evidence: $runDir"
echo 'Run only after other jobs using build_cuda have ended; this rebuild changes that executable.'
cp "${BASH_SOURCE[0]}" "$runDir/driver.slurm"
cp "$experiments/check_compact_timers.py" "$runDir/"
git -C "$FEM_ROOT/ippl-frk" diff --binary HEAD > "$runDir/source.patch"
sha256sum "$FEM_ROOT/ippl-frk/src/Utility/IpplTimings."* \
  "$FEM_ROOT/ippl-frk/src/FEM/FEMContainer."* \
  "$FEM_ROOT/ippl-frk/src/FEM/LagrangeSpace_wFEMContainer.hpp" > "$runDir/source-sha256.txt"
sha256sum "$WRAPPER" > "$runDir/wrapper-sha256.txt"
if [[ -f $FEM_ROOT/gpu-portability-source-sha256.txt ]]; then
  cp "$FEM_ROOT/gpu-portability-source-sha256.txt" "$runDir/"
  (cd "$FEM_ROOT/ippl-frk" && sha256sum -c "$runDir/gpu-portability-source-sha256.txt") \
    > "$runDir/source-verification.log"
fi
cmake -S "$FEM_ROOT/ippl-frk" -B "$buildDir"
cmake --build "$buildDir" --target IpplTimings FEMReuse FEMPeriodic FEMParticleCoupling \
  FEMDevice FEMOperatorOracle1D FEMOperatorOracle2D FEMOperatorOracle3D LandauDampingFEM -j 8
cp "$buildDir/CMakeCache.txt" "$runDir/CMakeCache.txt"
app=$buildDir/demos/electrostaticFEM/LandauDampingFEM
sha256sum "$app" > "$runDir/executable-sha256.txt"
launch() {
  local ranks=$1; shift
  srun --nodes=1 --ntasks="$ranks" --ntasks-per-node="$ranks" \
    --cpus-per-task=72 --gpus-per-task=1 --cpu-bind=cores --kill-on-bad-exit=1 \
    "$WRAPPER" "$@"
}
failures=0
check() {
  local name=$1; shift
  local rc=0
  "$@" > "$runDir/$name.log" 2>&1 || rc=$?
  cat "$runDir/$name.log"
  printf '%s exit_code=%s\n' "$name" "$rc" >> "$runDir/check-status.txt"
  if (( rc != 0 )); then failures=$((failures + 1)); fi
  return 0
}
for ranks in 1 2 4; do
  check "timers-r$ranks" launch "$ranks" "$buildDir/unit_tests/Utility/IpplTimings"
  check "reuse-r$ranks" launch "$ranks" "$buildDir/unit_tests/FEM/wFEMContainer_build/FEMReuse"
done
for mode in standard constant_preserving; do
  export IPPL_FEM_STIFFNESS_MODE=$mode
  for dim in 1 2 3; do
    check "operator-${dim}D-$mode" launch 1 \
      "$buildDir/unit_tests/FEM/wFEMContainer_build/FEMOperatorOracle${dim}D"
  done
done
unset IPPL_FEM_STIFFNESS_MODE
# The reference fixtures have four cells/axis and support one/two ranks in 1D.
for ranks in 1 2; do
  check "periodic-r$ranks" launch "$ranks" "$buildDir/unit_tests/FEM/wFEMContainer_build/FEMPeriodic" \
    '--gtest_filter=FEMPeriodic.P3_*'
  check "particles-r$ranks" launch "$ranks" "$buildDir/unit_tests/FEM/wFEMContainer_build/FEMParticleCoupling"
  check "device-r$ranks" launch "$ranks" "$buildDir/unit_tests/FEM/wFEMContainer_build/FEMDevice"
done
for ranks in 1 2 4; do
  for dim in 1 2 3; do
    case $dim in 1) cells=32;; 2) cells=8;; 3) cells=4;; esac
    for method in plain jacobi gauss-seidel chebyshev; do
    solver=preconditioned; preconditioner=$method
    if [[ $method == plain ]]; then solver=plain; preconditioner=jacobi; fi
    for recording in summary raw; do
      output=$runDir/d${dim}-r${ranks}-${method}-${recording}
      check "d${dim}-r${ranks}-${method}-${recording}" launch "$ranks" "$app" --dim "$dim" --order 3 --cells "$cells" \
        --particles 1024 --steps 2 --quadrature_points exact --reuse full \
        --solver "$solver" --preconditioner_type "$preconditioner" --gauss_seidel_outer_iterations 1 \
        --chebyshev_degree 8 --timer_level coarse --timer_recording "$recording" \
        --timer_sample_limit 4 --output "$output"
    done
    check "d${dim}-r${ranks}-${method}-comparison" python3 "$runDir/check_compact_timers.py" --ranks "$ranks" \
      --summary "$runDir/d${dim}-r${ranks}-${method}-summary" --raw "$runDir/d${dim}-r${ranks}-${method}-raw"
    done
  done
done
(( failures == 0 )) || { echo "$failures checks failed; see check-status.txt and per-check logs." >&2; exit 1; }
echo 'Efficiency checks passed. Use new campaign directories for all GPU counts.'
}
