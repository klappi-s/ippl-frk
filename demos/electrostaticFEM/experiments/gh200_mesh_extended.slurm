#!/usr/bin/env bash
# Ordinary PIC measurements on exclusive GH200 allocations.
# See ProjectManagement/2026-09-23_1608_GH200_PERFORMANCE_GUIDE.md in the FEM workspace.
#SBATCH --job-name=fem-mesh
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --ntasks-per-node=1
#SBATCH --gpus-per-task=1
#SBATCH --cpus-per-task=72
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=02:00:00
#SBATCH --output=fem-mesh-%j.log
#SBATCH --error=fem-mesh-%j.err

# Parse before execution; sync this file by atomic replacement.
{
set -euo pipefail
: "${FEM_ROOT:?Set FEM_ROOT to the cluster directory containing ippl-frk and build_cuda}"
: "${SLURM_NTASKS:?Submit this script through Slurm}"
: "${SLURM_JOB_NUM_NODES:?Submit this script through Slurm}"
: "${SLURM_JOB_ID:?Submit this script through Slurm}"
experiments=${FEM_EXPERIMENTS:-$FEM_ROOT/ippl-frk/demos/electrostaticFEM/experiments}
buildDir=${FEM_BUILD:-$FEM_ROOT/build_cuda}
application=${FEM_APPLICATION:-fem}
case "$application" in
  fem) defaultApp=$buildDir/demos/electrostaticFEM/LandauDampingFEM;;
  alpine) defaultApp=$buildDir/demos/alpine/LandauDamping;;
  *) echo "Unknown application: $application" >&2; exit 2;;
esac
app=${FEM_EXECUTABLE:-$defaultApp}
nodes=$SLURM_JOB_NUM_NODES
ranks=$SLURM_NTASKS
cpus=${SLURM_CPUS_PER_TASK:-72}
(( ranks > 0 && nodes > 0 && ranks % nodes == 0 )) || { echo 'Ranks must divide evenly across nodes.' >&2; exit 2; }
perNode=$((ranks / nodes))
case "$nodes:$perNode" in
  1:1|1:2|1:4|2:1|2:2|2:4|4:1|4:2|4:4|8:4|16:4|32:4) ;;
  *) echo 'Supported placements: 1/2/4 nodes x 1/2/4 GPUs, or 8/16/32x4.' >&2; exit 2;;
esac
output=${FEM_OUTPUT:-$FEM_ROOT/gpu_performance/$(date -u +%Y%m%dT%H%M%SZ)-${nodes}n-${perNode}gpn-$SLURM_JOB_ID}
mkdir -p "$output/allocations"
runDir=$(mktemp -d "$output/allocations/${SLURM_JOB_ID}-XXXXXX")
exec > >(tee "$runDir/session.log") 2>&1
trap 'rc=$?; printf "exit_code=%s\n" "$rc" > "$runDir/status.txt"; exit "$rc"' EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
trap 'exit 129' HUP
printf 'Campaign: %s\nAllocation evidence: %s\n' "$output" "$runDir"

export OMP_NUM_THREADS="${FEM_OMP_THREADS:-1}"
python_command="${FEM_PYTHON:-python3}"
"$python_command" -c 'import sys; assert sys.version_info >= (3, 6), "Python 3.6+ required"'
[[ -x "$app" ]] || { echo "Missing CUDA executable: $app" >&2; exit 2; }
export WRAPPER="${WRAPPER:-/user-environment/wrapper-mpi.sh}"
[[ $WRAPPER == /* && -f "$WRAPPER" && -r "$WRAPPER" && -x "$WRAPPER" ]] || {
  echo "WRAPPER must name an absolute, readable executable file: $WRAPPER" >&2; exit 2;
}
# Every step uses the same explicit placement, including on an exclusive node
# where the allocation itself may expose all four GPUs to the batch shell.
launch=(srun --nodes="$nodes" --ntasks="$ranks" --ntasks-per-node="$perNode"
  --cpus-per-task="$cpus" --gpus-per-task=1 --cpu-bind=cores --kill-on-bad-exit=1)
# The runner parses with shlex.split and formats {ranks}. Preserve arbitrary
# wrapper paths as one argument, including literal braces and shell metacharacters.
launcher=$("$python_command" -c 'import shlex, sys; print(" ".join(shlex.quote(a) for a in sys.argv[1:]).replace("{", "{{").replace("}", "}}"))' \
  "${launch[@]}" "$WRAPPER")
{
  date -u; hostname
  printf 'nodes=%s\nranks=%s\nranks_per_node=%s\ncpus_per_rank=%s\n' "$nodes" "$ranks" "$perNode" "$cpus"
  printf 'OMP_NUM_THREADS=%s\nCUDA_LAUNCH_BLOCKING=%s\nMPICH_GPU_SUPPORT_ENABLED=%s\n' \
    "$OMP_NUM_THREADS" "${CUDA_LAUNCH_BLOCKING:-unset}" "${MPICH_GPU_SUPPORT_ENABLED:-unset}"
  printf 'WRAPPER=%s\n' "$WRAPPER"
  "$python_command" --version
  scontrol show job "$SLURM_JOB_ID"
} > "$runDir/allocation.txt" 2>&1
cp "${BASH_SOURCE[0]}" "$runDir/driver.slurm"
cp "$buildDir/CMakeCache.txt" "$runDir/CMakeCache.txt"
if [[ -n ${FEM_BASELINE:-} ]]; then
  cp "$FEM_BASELINE/source-revision.txt" "$FEM_BASELINE/source.patch" "$runDir/"
  cp "$FEM_BASELINE/baseline.json" "$runDir/baseline.json"
else
  git -C "$FEM_ROOT/ippl-frk" rev-parse HEAD > "$runDir/source-revision.txt"
  git -C "$FEM_ROOT/ippl-frk" diff --binary HEAD > "$runDir/source.patch"
fi
# Freeze the runner, validator, and their local registry dependency for this
# allocation.  The copied scripts must be importable without consulting the
# live source tree so the recorded hashes describe the code that actually ran.
mkdir "$runDir/tools"
cp "$experiments/run_mesh_sweep.py" \
   "$experiments/ordinary_measurements.py" \
   "$experiments/fd_pcg_variants.py" \
   "$experiments/rank_memory.py" \
   "$runDir/tools/"
sha256sum "$app" "$runDir/tools/"*.py > "$runDir/sha256.txt"
"$python_command" - "$WRAPPER" "$runDir" "$output" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

wrapper, directory, campaign = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
data = wrapper.read_bytes()
record = dict(path=str(wrapper), sha256=hashlib.sha256(data).hexdigest())
text = json.dumps(record, indent=2) + '\n'
(directory / 'mpi-wrapper.sh').write_bytes(data)
(directory / 'mpi-wrapper.json').write_text(text)
pin = campaign / 'mpi-wrapper.json'
if pin.exists():
    if json.loads(pin.read_text()) != record:
        raise SystemExit('MPI wrapper changed; use a new campaign directory.')
elif (campaign / 'campaign.json').exists():
    raise SystemExit('Existing campaign has no MPI wrapper record; use a new directory.')
else:
    with pin.open('x') as stream:
        stream.write(text)
print('MPI wrapper: %s (SHA256 %s)' % (record['path'], record['sha256']))
PY

timeout "${FEM_PREFLIGHT_TIMEOUT:-300}" "${launch[@]}" "$WRAPPER" "$python_command" -c '
import json, os, socket, sys
from pathlib import Path
rank = os.environ["SLURM_PROCID"]
record = dict(rank=int(rank), local_rank=int(os.environ["SLURM_LOCALID"]),
              host=socket.gethostname(), visible_gpu=os.environ.get("CUDA_VISIBLE_DEVICES", ""),
              cpus=sorted(os.sched_getaffinity(0)),
              omp_num_threads=os.environ.get("OMP_NUM_THREADS", ""),
              mpich_gpu_support=os.environ.get("MPICH_GPU_SUPPORT_ENABLED", ""))
(Path(sys.argv[1]) / ("placement-rank" + rank + ".json")).write_text(json.dumps(record, indent=2) + "\n")
' "$runDir"
"$python_command" - "$runDir" "$nodes" "$perNode" <<'PY'
import json
from pathlib import Path
import sys

directory, nodes, per_node = Path(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
rows = [json.loads((directory / ('placement-rank%d.json' % rank)).read_text())
        for rank in range(nodes * per_node)]
hosts = {row['host'] for row in rows}
assert len(hosts) == nodes and {row['rank'] for row in rows} == set(range(len(rows)))
for host in hosts:
    local = [row for row in rows if row['host'] == host]
    assert len(local) == per_node and {row['local_rank'] for row in local} == set(range(per_node))
    assert all(row['cpus'] and row['visible_gpu'] not in ('', '-1')
               and ',' not in row['visible_gpu'] for row in local)
(directory / 'placement.json').write_text(json.dumps(rows, indent=2) + '\n')
print('Verified %d nodes x %d GPU ranks/node.' % (nodes, per_node))
PY
timeout "${FEM_PREFLIGHT_TIMEOUT:-300}" "${launch[@]}" --label nvidia-smi > "$runDir/gpus-before.log"
# Separate allocation preflight; never part of the measured ten-step records.
"$python_command" "$runDir/tools/run_mesh_sweep.py" \
  --application "$application" --executable "$app" --output "$runDir/preflight" \
  --dimensions 3 --orders 1 2 3 --cells-3d 16 --particles-per-cell 8 --steps 1 --repeats 1 \
  --variants cg --ranks "$ranks" --launcher "$launcher" --expected-backend Cuda \
  --rank-monitor "$runDir/tools/rank_memory.py" --memory-estimate-gib 2 \
  --timeout "${FEM_PREFLIGHT_TIMEOUT:-300}" > "$runDir/preflight.log" 2>&1
printf 'CUDA ordinary-run preflight passed on all allocated ranks.\n'

status=0
"$python_command" "$runDir/tools/run_mesh_sweep.py" "$@" \
  --application "$application" --executable "$app" --output "$output" \
  --ranks "$ranks" --nodes "$nodes" --gpus-per-node "$perNode" \
  --launcher "$launcher" --expected-backend Cuda --resume || status=$?
timeout "${FEM_PREFLIGHT_TIMEOUT:-300}" "${launch[@]}" --label nvidia-smi > "$runDir/gpus-after.log" || true
exit "$status"
}
