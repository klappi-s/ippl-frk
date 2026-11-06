#!/usr/bin/env bash
# Run on Daint, in the CUDA/MPI software environment used for this checkout.
# Parse the whole driver before running commands. Bash can otherwise resume reading
# at a stale byte offset if this file is edited while a long build/test is running.
{
set -euo pipefail
stage=${1:-help}
case "$stage" in
  help|--help|-h)
    cat <<'EOF'
Usage: bash gh200_validate.sh STAGE
  build       Configure CUDA and build the application and focused correctness tests (-j 8).
  build-full  Also build P1-P5 operator/Poisson oracles and periodic tests.
  smoke       One GPU: reference tests, cache/reuse/particle checks, and small PIC runs.
  oracle-source One GPU: source-sampling regression in 1D-3D, both stiffness modes.
  oracle      One GPU: complete serial FEM oracle/periodic CTest selection (build-full first).
  mpi         Two GPUs: reuse, particle and halo checks; small two-rank PIC run.
  mpi-two-node Two nodes, one GPU/rank per node: placement, periodic/reuse/halo tests and ordinary PIC runs.
  sanitize    One GPU: complete memcheck reports for MPI init, cache and P3/P5 probes.
  sanitize-mpi-filtered Same probes; review complete reports with a narrow Cray MPI exception.
  all         build, then smoke (requires a Slurm GPU allocation).

Load the site's CUDA/MPI environment before invoking this script. Run stages inside
an allocation, or submit gh200_validate.slurm. No account/uenv is assumed.
Overrides: FEM_ROOT, FEM_SOURCE, FEM_BUILD, FEM_LOG_ROOT, FEM_DIMS, FEM_ORDERS,
FEM_CPUS_PER_TASK (default 8), FEM_TEST_TIMEOUT (default 900 seconds).
EOF
    exit 0;;
  build|build-full|smoke|oracle-source|oracle|mpi|mpi-two-node|sanitize|sanitize-mpi-filtered|all) ;;
  *) printf 'Unknown stage: %s\n' "$stage" >&2; exit 2;;
esac
scriptDir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ -d "$scriptDir/ippl-frk" ]]; then
  defaultRoot=$scriptDir
else
  defaultRoot=$(cd -- "$scriptDir/../../../.." && pwd)
fi
root=${FEM_ROOT:-$defaultRoot}
sourceDir=${FEM_SOURCE:-$root/ippl-frk}
buildDir=${FEM_BUILD:-$root/build_cuda}
logRoot=${FEM_LOG_ROOT:-$root/gpu_validation}
[[ -f "$sourceDir/CMakeLists.txt" ]] || { printf 'Missing checkout: %s\n' "$sourceDir" >&2; exit 2; }
# This intentionally refuses a local build through the SSHFS mount.
command -v nvcc >/dev/null || { echo 'Load the cluster CUDA software environment (nvcc missing).' >&2; exit 2; }
command -v srun >/dev/null || { echo 'This script is for the cluster (srun missing).' >&2; exit 2; }
mkdir -p "$logRoot"
runDir=$(mktemp -d "$logRoot/$(date -u +%Y%m%dT%H%M%SZ)-${stage}-${SLURM_JOB_ID:-interactive}-XXXXXX")
exec > >(tee "$runDir/session.log") 2>&1
printf 'Results: %s\n' "$runDir"
trap 'rc=$?; printf "exit_code=%s\n" "$rc" > "$runDir/status.txt"; exit "$rc"' EXIT
run() {
  local name=$1; shift
  printf '\nRunning %s:' "$name"; printf ' %q' "$@"; printf '\n'
  printf '%q ' "$@" >> "$runDir/commands.sh"; printf '\n' >> "$runDir/commands.sh"
  local rc=0
  "$@" 2>&1 | tee "$runDir/$name.log" || rc=$?
  printf '%s\t%s\n' "$name" "$rc" >> "$runDir/stages.tsv"
  return "$rc"
}
# Long CUDA translation units can be silent for many minutes. Observe the build
# process tree without changing the compiler command, options or exit status.
runBuild() (
  local name=$1
  local buildPid=$BASHPID
  if ! command -v python3 >/dev/null || ! command -v ps >/dev/null; then
    echo 'Build progress monitoring unavailable (python3 or ps missing).'
    run "$@"
    exit "$?"
  fi
  python3 -u - "$buildPid" <<'PY' > >(tee "$runDir/$name-progress.log") 2>&1 &
import datetime
import os
import subprocess
import sys
import time

root = int(sys.argv[1])
started = time.monotonic()
print('[build-watch] Every 30 s: CPU_TIME is cumulative CPU time; RSS_KiB is current resident memory.')
print('[build-watch] CPU time increasing shows compiler activity, not a completion estimate.')
while os.path.exists(f'/proc/{root}'):
    try:
        result = subprocess.run(
            ['ps', '-eo', 'pid=,ppid=,etimes=,time=,pcpu=,rss=,stat=,comm='],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            universal_newlines=True)
        rows = [line.split(maxsplit=7) for line in result.stdout.splitlines()]
        descendants = {root}
        while True:
            added = {int(row[0]) for row in rows
                     if int(row[1]) in descendants and int(row[0]) != os.getpid()}
            if added <= descendants:
                break
            descendants |= added
        stamp = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')
        print(f'[build-watch] {stamp} elapsed={time.monotonic() - started:.0f}s')
        print('[build-watch] PID PPID ELAPSED_s CPU_TIME CPU_% RSS_KiB STATE COMMAND')
        for row in rows:
            if int(row[0]) in descendants:
                print('[build-watch] ' + ' '.join(row))
    except (OSError, subprocess.SubprocessError, ValueError) as error:
        print(f'[build-watch] Process snapshot unavailable: {error}')
        break
    time.sleep(30)
PY
  local monitorPid=$!
  trap 'rc=$?; kill "$monitorPid" 2>/dev/null || :; wait "$monitorPid" 2>/dev/null || :; exit "$rc"' EXIT
  run "$@"
)
{
  date -u; hostname; uname -m
  printf 'source=%s\nbuild=%s\njob=%s\nCUDA_VISIBLE_DEVICES=%s\n' \
    "$sourceDir" "$buildDir" "${SLURM_JOB_ID:-}" "${CUDA_VISIBLE_DEVICES:-}"
  printf 'nodes=%s\ntasks=%s\nnodelist=%s\nMPICH_GPU_SUPPORT_ENABLED=%s\n' \
    "${SLURM_JOB_NUM_NODES:-}" "${SLURM_NTASKS:-}" "${SLURM_JOB_NODELIST:-}" \
    "${MPICH_GPU_SUPPORT_ENABLED:-unset}"
  cmake --version; nvcc --version
  git -C "$sourceDir" rev-parse HEAD
  git -C "$sourceDir" status --short
  if command -v uenv >/dev/null; then uenv status || true; fi
  if command -v module >/dev/null; then module list 2>&1 || true; fi
} > "$runDir/environment.txt" 2>&1
# Record tracked changes as well as actual bytes, including new test sources.
git -C "$sourceDir" diff --binary HEAD > "$runDir/source.patch"
(cd "$sourceDir" && find src/FEM src/PoissonSolvers demos/electrostaticFEM unit_tests/FEM/wFEMContainer \
  unit_tests/FEM/Quadrature \
  test/solver/fem_wContainer \
  -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name 'CMakeLists.txt' \) \
  -print0 | sort -z | xargs -0 sha256sum) > "$runDir/source-sha256.txt"
cp -- "${BASH_SOURCE[0]}" "$runDir/driver.sh"
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-1}
cpus=${FEM_CPUS_PER_TASK:-8}
launcher=(srun --nodes=1 --ntasks=1 --cpus-per-task="$cpus" --gpus-per-task=1 --cpu-bind=cores)
unitDir=$buildDir/unit_tests/FEM/wFEMContainer_build
app=$buildDir/demos/electrostaticFEM/LandauDampingFEM
baseTargets=(Quadrature FEMDevice FEMReferenceOracle1D FEMReferenceOracle2D FEMReferenceOracle3D FEMReuse
  FEMParticleCoupling FEMConstantPreservation FEMPreconditionerProperties FEMThinHalo LandauDampingFEM)
configureBuild() {
  run configure cmake -S "$sourceDir" -B "$buildDir" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=20 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DIPPL_ENABLE_SOLVERS=ON -DIPPL_ENABLE_FFT=ON -DIPPL_ENABLE_TESTS=ON \
    -DIPPL_ENABLE_UNIT_TESTS=ON -DIPPL_ENABLE_ELECTROSTATIC_FEM=ON \
    -DIPPL_PLATFORMS=cuda -DKokkos_ARCH_HOPPER90=ON -DCMAKE_CUDA_ARCHITECTURES=90 \
    -DHeffte_VERSION=git.v2.4.1 \
    "-DIPPL_ELECTROSTATIC_FEM_DIMS=${FEM_DIMS:-1;2;3}" \
    "-DIPPL_ELECTROSTATIC_FEM_ORDERS=${FEM_ORDERS:-1;2;3;4;5}" \
    -DMPIEXEC_EXECUTABLE="$(command -v srun)" -DMPIEXEC_NUMPROC_FLAG=-n \
    "-DMPIEXEC_PREFLAGS=--gpus-per-task=1;--cpus-per-task=$cpus;--cpu-bind=cores" -DMPIEXEC_POSTFLAGS=
  cp "$buildDir/CMakeCache.txt" "$runDir/CMakeCache.txt"
  [[ ! -f "$buildDir/compile_commands.json" ]] || cp "$buildDir/compile_commands.json" "$runDir/compile_commands.json"
  runBuild build cmake --build "$buildDir" -j 8 --target "${baseTargets[@]}"
}
needAllocation() {
  [[ -n ${SLURM_JOB_ID:-} ]] || { echo 'Run this stage inside a Slurm GPU allocation.' >&2; exit 2; }
  run gpu-info "${launcher[@]}" nvidia-smi
  [[ -x "$unitDir/FEMDevice" ]] || { echo 'Run the build stage first.' >&2; exit 2; }
  run device "${launcher[@]}" "$unitDir/FEMDevice" "--gtest_output=xml:$runDir/device.xml"
  grep -q 'FEM execution space: Cuda' "$runDir/device.log" || {
    echo 'Refusing GPU validation: FEMDevice did not report the CUDA execution space.' >&2; exit 2;
  }
}
unit() {
  local target=$1; shift
  run "$target" timeout "${FEM_TEST_TIMEOUT:-900}" "${launcher[@]}" "$unitDir/$target" \
    "--gtest_output=xml:$runDir/$target.xml" "$@"
}
picRuns() {
  local name=$1 methods=$2 method solver preconditioner rc=0
  shift 2
  for method in $methods; do
    solver=preconditioned; preconditioner=$method
    if [[ $method == plain ]]; then solver=plain; preconditioner=jacobi; fi
    run "$name-$method" timeout "${FEM_TEST_TIMEOUT:-900}" "${launcher[@]}" "$app" "$@" \
      --solver "$solver" --preconditioner_type "$preconditioner" \
      --output "$runDir/$name-$method" || rc=$?
  done
  return "$rc"
}
smoke() {
  needAllocation
  for target in FEMReferenceOracle1D FEMReferenceOracle2D FEMReferenceOracle3D FEMReuse \
                FEMParticleCoupling FEMConstantPreservation FEMPreconditionerProperties; do
    unit "$target"
  done
  # Small correctness runs, not performance measurements. Exercise Q=P+1 and Q9,
  # both families and preconditioner reuse, including the largest P5 device tables.
  picRuns pic-1d 'plain jacobi chebyshev' --dim 1 --order 3 --cells 32 --particles 2048 --steps 3 \
    --quadrature_points exact
  picRuns pic-2d 'plain jacobi' --dim 2 --order 3 --cells 4 --particles 1024 --steps 2 \
    --interpolation_nodes equispaced --quadrature_points exact
  picRuns pic-3d-p5 'plain jacobi' --dim 3 --order 5 --cells 2 --particles 512 --steps 1 --quadrature_points 9
}
case "$stage" in
  build|all) configureBuild; [[ $stage != all ]] || smoke;;
  build-full)
    configureBuild
    runBuild build-oracles cmake --build "$buildDir" -j 8 --target FEMPeriodic FEMOracleRuntimeTest \
      FEMOperatorOracle1D FEMOperatorOracle2D FEMOperatorOracle3D \
      FEMPoissonOracle1D FEMPoissonOracle2D FEMPoissonOracle3D;;
  smoke) smoke;;
  oracle-source)
    needAllocation
    for dim in 1 2 3; do
      for mode in standard constant_preserving; do
        name="source-${dim}d-${mode}"
        run "$name" timeout "${FEM_TEST_TIMEOUT:-900}" "${launcher[@]}" \
          env IPPL_FEM_STIFFNESS_MODE="$mode" "$unitDir/FEMOperatorOracle${dim}D" \
          '--gtest_filter=*SourceSamplingUsesPhysicalNodesAndSelectedFamily' \
          "--gtest_output=xml:$runDir/$name.xml"
      done
    done;;
  oracle)
    needAllocation
    # Run every selected test even after failures; keep CTest's final failure status.
    run oracles ctest --test-dir "$buildDir" -j 1 --output-on-failure \
      --output-junit "$runDir/oracles.xml" \
      -R '^(Quadrature$|FEMReferenceOracle|FEMOperatorOracle|FEMPoissonOracle|FEMPeriodic$|FEMOracleRuntimeTest$)' \
      -E '(_mpi[24]|_MPI[24])'
    ;;
  mpi)
    needAllocation
    launcher=(srun --nodes=1 --ntasks=2 --cpus-per-task="$cpus" --gpus-per-task=1 --cpu-bind=cores)
    # GoogleTest XML filenames are omitted here to avoid two ranks writing one file.
    for target in FEMReuse FEMParticleCoupling FEMThinHalo; do
      run "$target-mpi2" timeout "${FEM_TEST_TIMEOUT:-900}" "${launcher[@]}" "$unitDir/$target"
    done
    picRuns pic-mpi2 'plain jacobi' --dim 1 --order 3 --cells 16 --particles 1024 --steps 3 \
      --quadrature_points exact;;
  mpi-two-node)
    [[ ${SLURM_JOB_NUM_NODES:-0} == 2 && ${SLURM_NTASKS:-0} == 2 ]] || {
      echo 'Use sbatch --nodes=2 --ntasks=2 --ntasks-per-node=1 gh200_validate.slurm mpi-two-node' >&2
      exit 2
    }
    command -v python3 >/dev/null || { echo 'python3 is required for placement verification.' >&2; exit 2; }
    needAllocation
    launcher=(srun --nodes=2 --ntasks=2 --ntasks-per-node=1 --cpus-per-task="$cpus"
      --gpus-per-task=1 --cpu-bind=cores --kill-on-bad-exit=1)
    run placement timeout "${FEM_TEST_TIMEOUT:-900}" "${launcher[@]}" bash -c \
      'printf "%s\t%s\t%s\t%s\n" "$SLURM_PROCID" "$SLURM_LOCALID" "$(hostname)" "${CUDA_VISIBLE_DEVICES:-unset}"'
    run placement-check python3 - "$runDir/placement.log" <<'PY'
import json
from pathlib import Path
import sys

path = Path(sys.argv[1])
rows = [line.split('\t') for line in path.read_text().splitlines()]
valid = (len(rows) == 2 and all(len(row) == 4 for row in rows)
         and {row[0] for row in rows} == {'0', '1'}
         and len({row[2] for row in rows}) == 2
         and all(row[1] == '0' and row[2] and row[3] not in ('', 'unset', '-1')
                 and ',' not in row[3] for row in rows))
path.with_suffix('.json').write_text(json.dumps({'accepted': valid, 'ranks': rows}, indent=2) + '\n')
if not valid:
    raise SystemExit('Expected two distinct hosts, one local rank and one visible GPU per host.')
print('Verified two distinct hosts, one rank and one visible GPU per host.')
PY
    run gpu-info-two-node timeout "${FEM_TEST_TIMEOUT:-900}" "${launcher[@]}" --label nvidia-smi
    mpiStatus=0
    mpiUnit() {
      local target=$1; shift
      # Each MPI rank writes its own XML. Continue to later probes after failures.
      run "$target-two-node" timeout "${FEM_TEST_TIMEOUT:-900}" "${launcher[@]}" \
        bash -c 'exec "$1" "--gtest_output=xml:$2-rank${SLURM_PROCID:?}.xml" "${@:3}"' \
        _ "$unitDir/$target" "$runDir/$target-two-node" "$@"
    }
    for target in FEMDevice FEMReuse FEMParticleCoupling FEMThinHalo FEMPreconditionerProperties; do
      mpiUnit "$target" || mpiStatus=$?
    done
    mpiUnit FEMPeriodic '--gtest_filter=FEMPeriodic.P3_*:FEMPeriodic.P5_*' || mpiStatus=$?
    # Small communication/correctness probes. The P3 cases exercise the candidate
    # preconditioners before performance studies; P5 covers the larger 3D tables.
    for spec in '1 3 16 1024 3 gll' '2 3 4 1024 2 equispaced' \
                '3 3 4 1024 2 gll' '3 5 2 512 1 gll'; do
      read -r dim order cells particles steps family <<< "$spec"
      name="pic-two-node-${dim}d-p${order}"
      methods='plain jacobi gauss-seidel chebyshev'
      [[ $order != 5 ]] || methods='plain jacobi'
      picRuns "$name" "$methods" \
        --dim "$dim" --order "$order" --cells "$cells" --particles "$particles" --steps "$steps" \
        --interpolation_nodes "$family" --quadrature_points exact \
        --reuse full --poisson_stiffness_mode constant_preserving || mpiStatus=$?
    done
    if (( mpiStatus != 0 )); then
      printf '\nTwo-node probes completed with failures; inspect stages.tsv and per-rank XML in %s\n' "$runDir"
      exit "$mpiStatus"
    fi
    ;;
  sanitize|sanitize-mpi-filtered)
    needAllocation
    command -v compute-sanitizer >/dev/null || { echo 'compute-sanitizer is not on PATH.' >&2; exit 2; }
    run sanitizer-version compute-sanitizer --version
    export CUDA_LAUNCH_BLOCKING=1
    sanitizerStatus=0
    sanitizerErrorExit=99
    if [[ $stage == sanitize-mpi-filtered ]]; then
      command -v python3 >/dev/null || { echo 'python3 is required for report review.' >&2; exit 2; }
      cp "$sourceDir/demos/electrostaticFEM/experiments/classify_mpi_memcheck.py" \
        "$runDir/classify_mpi_memcheck.py"
      # Preserve application failures directly; the reviewer below independently
      # fails on any unreviewed report or missing/incomplete evidence. All CUDA
      # reporting stays enabled, with no native suppression or discarded records.
      sanitizerErrorExit=0
      echo 'MPI exception mode: complete reports are retained and checked after each probe.'
      echo 'Only reviewed Cray MPI diagnostics are accepted; other reports and application failures fail.'
    fi
    memcheck() {
      local name=$1 expectedTests=$2; shift 2
      local rc=0 reviewRc=0
      run "$name" timeout "${FEM_TEST_TIMEOUT:-900}" "${launcher[@]}" \
        compute-sanitizer --tool memcheck --check-exit-code yes \
        --error-exitcode="$sanitizerErrorExit" --print-limit 0 \
        --xml --save "$runDir/$name.xml" "$@" || rc=$?
      if [[ $stage == sanitize-mpi-filtered ]]; then
        run "$name-review" python3 "$runDir/classify_mpi_memcheck.py" \
          "$runDir/$name.xml" "$runDir/$name.log" --expected-tests "$expectedTests" \
          --output "$runDir/$name-review.json" || reviewRc=$?
        # A passing report review must never overwrite an application failure.
        if (( rc == 0 )); then rc=$reviewRc; fi
      fi
      return "$rc"
    }
    # A control with MPI/Kokkos initialization/finalization but no GoogleTest body
    # distinguishes startup reports from reports triggered by FEM operations.
    # Attempt all probes even if one fails; preserve a failing aggregate status.
    memcheck memcheck-mpi-init 0 "$unitDir/FEMDevice" '--gtest_filter=-*' || sanitizerStatus=$?
    memcheck memcheck-cache 2 "$unitDir/FEMDevice" || sanitizerStatus=$?
    memcheck memcheck-p3-p5 2 "$unitDir/FEMReferenceOracle3D" \
      '--gtest_filter=*DoubleP3*.DeviceBasisAndMappings:*DoubleP5*.DeviceBasisAndMappings' || sanitizerStatus=$?
    if (( sanitizerStatus != 0 )); then
      printf '\nSanitizer probes completed with failures; inspect all memcheck logs/XML in %s\n' "$runDir"
      exit "$sanitizerStatus"
    fi
    ;;
esac
printf '\nCompleted %s. Logs: %s\n' "$stage" "$runDir"
exit 0
}
