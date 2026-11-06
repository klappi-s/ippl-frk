# FEM and finite-difference performance campaigns

**Updated 07 October 2026, 15:06 CEST (Europe/Zurich).**

This directory contains one maintained entry point per application:

- `submit_fem_campaign.py` submits higher-order FEM placement and
  preconditioner studies using `LandauDampingFEM`.
- `submit_fd_pcg_campaign.py` submits Alpine finite-difference placement and
  PCG studies using `LandauDamping`.

Both delegate plan creation and Slurm submission to `submit_campaigns.py`.
`run_mesh_sweep.py` launches each solver configuration in a fresh application
process, and `ordinary_measurements.py` validates the resulting solve, timestep,
physics, timer, and memory records. Do not invoke these internal layers directly
for a normal GH200 campaign.

Every measured process uses eight particles per geometric cell, one
initialization solve, ten ordinary timesteps, three independent repetitions,
exclusive nodes, one MPI rank per GPU, and the site MPI wrapper. Load balancing
remains disabled. By default, first and repeat stages are separate: repeat
tasks are admitted after repetition zero passes numerical and memory checks.
Explicit `--stage all` queues all three repetitions independently in one array;
each run still undergoes the same validation.

## Common setup

From the Daint workspace containing `ippl-frk` and `build_cuda`:

```bash
cd /users/sklapproth/phd/fem
export FEM_ROOT="$PWD"
export WRAPPER=/user-environment/wrapper-mpi.sh
EXPERIMENTS="$FEM_ROOT/ippl-frk/demos/electrostaticFEM/experiments"
```

Use `--dry-run` first when changing cells, orders, variants, or placements. A
campaign root is immutable with respect to its executable, wrapper, runner,
variant definitions, and numerical recipe. Resume with the same command and
root; use a new root after changing any pinned input.

## FEM campaigns

`submit_fem_campaign.py` supports 3D P1/P2/P3 and two phases:

- `placement` fixes the method to plain CG and writes below `placement/cg`.
- `tune` requires either a named `--bundle` or an explicit paired variant set
  containing `cg` and at least one of `jacobi`, `gs21`, `gs22`, `cheb2`,
  `cheb4`, `cheb8`, `cheb16`.

The named bundles are `controls` (CG/Jacobi), `gauss-seidel` (CG plus 2/1 and
2/2), `chebyshev-low` (CG plus degrees 2/4), `chebyshev-high` (CG plus degrees
8/16), and `all`. An explicit set receives a content-derived directory unless
`--label` gives it a stable name. The FEM solver supports more preconditioner
families, but the maintained campaign registry currently validates only these
eight variants; Newton, Richardson, SSOR and FEM multigrid are not part of this
grid.

The supported small meshes are 16³/32³/64³. Their historical nine default
placements remain; explicit FEM searches also accept 8/16 nodes with 1/2/4
GPUs per node. The supported large meshes are 128³/256³. Their historical
1×2/1×4/2×4/4×4/8×4 defaults remain; explicit searches also accept
2×1/4×1/8×1/16×1, 2×2/4×2/8×2/16×2 and 16×4. Source-based memory
screening remains active; the full-volume diagnostics exclude P3 256³ on 1×2.

Example placement study:

```bash
FEM_CAMPAIGN="$FEM_ROOT/gpu_performance/$(date +%Y-%m-%d_%H%M)_fem"

python3 "$EXPERIMENTS/submit_fem_campaign.py" --root "$FEM_CAMPAIGN" \
  --phase placement --cells 16 32 64 --orders 1 2 3 \
  --placements 1x1 1x2 1x4 2x1 2x2 2x4 4x1 4x2 4x4 \
  --stage first

# After all required first runs pass:
python3 "$EXPERIMENTS/submit_fem_campaign.py" --root "$FEM_CAMPAIGN" \
  --phase placement --cells 16 32 64 --orders 1 2 3 \
  --placements 1x1 1x2 1x4 2x1 2x2 2x4 4x1 4x2 4x4 \
  --stage repeats
```

Example focused preconditioner comparison:

```bash
python3 "$EXPERIMENTS/submit_fem_campaign.py" --root "$FEM_CAMPAIGN" \
  --phase tune --cells 64 --orders 2 3 --placements 1x4 4x4 \
  --variants cg jacobi gs21 cheb4 --stage first
```

Run the identical command with `--stage repeats` after its first runs pass.
Keep CG in each tuning bundle because it is the paired same-allocation control.

### Standard-CG placement extension: 64³ and 128³

The workspace helper `submit_fem_cg_extension.sh` uses the same FEM entry point
with `--stage all`, three orders and three repetitions. From the Daint
workspace, after syncing the helper and updated experiment scripts:

```bash
bash ./submit_fem_cg_extension.sh all --dry-run --account c41
bash ./submit_fem_cg_extension.sh all --account c41
```

Use `small` or `large` instead of `all` to submit only 64³ or 128³.
The 64³ extension uses 8×1/8×2/8×4/16×1/16×2/16×4; the 128³ extension
uses 2×1/4×1/8×1/16×1/2×2/4×2/8×2/16×2/16×4. Together they form
15 arrays with nine tasks each (135 measured simulations). Array indices
0–2 are P1 repetitions, 3–5 P2, and 6–8 P3. There is no first-run dependency.

Defaults: 59 minutes per task, 3,000 seconds per application, and three
simultaneous exclusive allocations per array. Set `FEM_CG_CONCURRENT=1` for
sequential execution within each array; all repetitions remain queued.
`FEM_CG_TIME`, `FEM_CG_TIMEOUT` and `FEM_CG_EXTENSION_CAMPAIGN` override the
limits or fixed output root. Defaults write below
`gpu_performance/2026-10-07_1501_fem_cg_placement_extension/placement/cg/n<cells>-<placement>`.
Reuse the same command/root to skip validated results and queued arrays.
Failed results need explicit `--retry-failed` after inspection.

The new root retains the original campaign records and source pins. The
allocation driver's accepted placements changed; retain the existing CUDA
executable to compare against the original results. The numerical runner,
validators, variant registry, and FD extended driver have not changed.
See the [extension guide](../../../../../ShortTermReports/2026-10-07_1506_FEM_CG_PLACEMENT_EXTENSION.md)
for deployment, direct Python commands and analysis.

### Maintained FEM parameter grid

The root helper covers the current eight FEM variants in four independent
bundles. Independent bundles prevent one slow or failed family from blocking
repeat admission for the others. From the Daint workspace root:

```bash
# Inspect and submit the small 16^3/32^3/64^3 grid.
bash ./submit_fem_parameter_grid.sh small first --dry-run
bash ./submit_fem_parameter_grid.sh small first

# After the eligible first cases finish:
bash ./submit_fem_parameter_grid.sh small repeats --dry-run
bash ./submit_fem_parameter_grid.sh small repeats

# Inspect and submit the large 128^3/256^3 grid.
bash ./submit_fem_parameter_grid.sh large first --dry-run
bash ./submit_fem_parameter_grid.sh large first

# After the eligible first cases finish:
bash ./submit_fem_parameter_grid.sh large repeats --dry-run
bash ./submit_fem_parameter_grid.sh large repeats
```

The default output root is
`gpu_performance/2026-09-30_1144_fem_parameter_grid`; set
`FEM_GRID_CAMPAIGN` before both stages to use another fixed root. The small grid
uses 1x1 and 2x1. The large grid uses 1x2/8x4 at 128^3, 1x2/8x4 for P1/P2 at
256^3, and 8x4/4x4 for P3 at 256^3. Thus the screened P3 256^3/1x2 case is not
planned. Defaults are two hours/6,000 seconds per small allocation/application
and eight hours/27,000 seconds for large; override them with
`FEM_SMALL_GRID_TIME`, `FEM_SMALL_RUN_TIMEOUT`, `FEM_LARGE_GRID_TIME`, and
`FEM_LARGE_RUN_TIMEOUT`.

The small first stage submits 24 arrays containing at most 198 ordinary
processes; its repeat stage contains at most 396. The large stages submit 20
arrays with at most 132 first and 264 repeat processes. Every array is exclusive
and throttled to one active task. Set `FEM_GRID_BUNDLES`, for example to
`"controls chebyshev-low"`, to submit only selected named bundles. Use the same
selection when requesting their repeats.

### Proposed FEM tuning grid

The next grid and its numerical prerequisites are documented in
[REPORT_FEM.md, Chapter 2](../../../../../ReportsMD/REPORT_FEM.md#2-parameter-grids-for-tuning).
It aligns GS/SSOR loop pairs and adds Newton, Richardson and SSOR coverage.
These broader configurations still require registry, option-forwarding and
validation support in the maintained FEM submitter.

## Finite-difference campaigns

The FD submitter has five phases:

- `smoke`: fixed 128³ family correctness check;
- `preflight`: fixed one-step 1024³ large-particle-count capacity check;
- `placement`: explicit plain-CG topology search below `placement/cg`;
- `tune`: a named family bundle or explicit variant subset;
- `transfer`/`confirm`: run an analyzer-generated shortlist.

Example 1024³ placement study after the preflight succeeds:

```bash
FD_CAMPAIGN="$FEM_ROOT/gpu_performance/2026-09-25_1705_fd_pcg"

python3 "$EXPERIMENTS/submit_fd_pcg_campaign.py" --root "$FD_CAMPAIGN" \
  --phase placement --cells 1024 --placements 4x4 8x4 16x4 \
  --stage first --time 00:59:00 --timeout 3000
```

Use `--stage repeats` after all three first runs pass. The current 32×4 path is
a debugging target: its first CG run passed, but both repeats failed with CUDA
illegal-memory accesses. Parameter tuning remains under `tune`, for example:

```bash
python3 "$EXPERIMENTS/submit_fd_pcg_campaign.py" --root "$FD_CAMPAIGN" \
  --phase tune --bundle ssor-all --cells 256 512 --stage first
```

An explicit variant subset normally receives a content-derived `custom-*`
directory. Give a maintained subset a stable descriptive directory with
`--label`, for example `--variants cg ssor11w100 ssor11w125 --label
ssor-factorial`. The label changes only the study path; the saved plan still
contains and validates the complete variant recipe.
Use `--substudy NAME` with `--label` when one logical study needs separately
resumable recipes below each mesh/placement root. This keeps the analyzer's
phase, label, and mesh anchor intact while preventing a known-failing diagnostic
point from blocking repetitions of the timing grid.
For an already completed broader first run, `--reuse-first-subset` admits a
`--stage repeats` substudy only when every requested variant has a successful
job state in the parent r0. Extra failed variants do not block that subset, and
the original first-run manifest remains unchanged.
If an earlier dry run saved the same execution recipe without subset-admission
metadata, the planner archives that draft in `plans/history/` and adds the
metadata. This migration is restricted to unsubmitted plans with no task
directories, logs, or study pin; changed execution recipes still fail.

The complete current FD variant registry is in `fd_pcg_variants.py`. It is
shared by plan generation, application command construction, validation, and
memory estimation. Invalid numerical results remain recorded and are never
timing samples. Do not use `--retry-failed` without first diagnosing the stored
failure.

### Recovering the interrupted multigrid factorial

The first `multigrid-factorial` allocations exposed parameter points that run
to 10,000 iterations. Ten such applications took 1,032--1,644 seconds and
failed. They exhausted the 30-minute debug allocations before later variants
could start. The normal `--resume`/`--retry-failed` path is unsuitable here
because it would run those known failures again.

Use the dedicated missing-only recovery from the Daint workspace root:

```bash
bash ./submit_multigrid_missing.sh --dry-run
bash ./submit_multigrid_missing.sh
```

The site wrapper supplies account `c41` and the same OPAL uenv/view as the
configured campaign launcher. Override these, when needed, with
`MG_RECOVERY_ACCOUNT`, `MG_RECOVERY_UENV`, and `MG_RECOVERY_VIEW`. Recovery
uses the `normal` partition with a 40-minute allocation limit; override those
defaults with `MG_RECOVERY_PARTITION` and `MG_RECOVERY_TIME`.

This scans the 24 first-stage smoothing bundles and submits only bundles that
still lack canonical `jobs/*.json` records. Within each submitted allocation,
`resume_missing_campaign.py` skips every recorded success and every recorded
failure, then runs only absent applications. Each newly attempted application
has a 180-second limit: the longest successful first-stage application observed
before recovery was 88.42 seconds, so this leaves margin while preventing a new
10,000-iteration point from consuming most of an allocation. A timeout is
recorded with exit code 124 and is not retried by another missing-only pass.

If Slurm ends an allocation while an application is running, its attempt log is
retained without a canonical job record. Run the same helper again after the
allocation has ended; only that interrupted application and any applications
after it are selected. The helper refuses to race an active original or
recovery allocation. Recovery writes into the original campaign tree, retains
the immutable plan and source pins, and remains compatible with the standard
analyzer. Do not add `--retry-failed` to this recovery.

Once every first-stage bundle has a canonical record for all thirteen variants,
launch factorial repeats with:

```bash
MG_SLURM_TIME=00:30:00 ./submit_multigrid_factorial.sh repeats --dry-run
MG_SLURM_TIME=00:30:00 ./submit_multigrid_factorial.sh repeats
```

The helper derives the successful variants independently for each mesh,
placement, and smoothing pair. It refuses an incomplete first stage, excludes
all recorded failures/timeouts, and stores r1/r2 below the corresponding
`smoothing-*/converged/` directory while reusing the successful parent r0
evidence. Repeat applications default to a 180-second timeout; override it with
`MG_RUN_TIMEOUT` only after inspecting a timeout.

Explicit 16×4/32×4 requests use `gh200_campaign_extended.slurm`,
`campaign_task_extended.py`, and `gh200_mesh_extended.slurm`. The older driver
is retained byte-for-byte because existing studies pin its hash. The extended
path applies to placement, transfer, and tuning phases. This duplication can be
consolidated after those studies no longer need to remain resumable.

## Analysis and validation

Analyze current FEM/ordinary campaigns with:

```bash
python3 "$EXPERIMENTS/analyze_mesh_sweep.py" CAMPAIGN_ROOT [CAMPAIGN_ROOT ...] \
  --output ANALYSIS_DIRECTORY --plots
```

Analyze the dedicated FD-PCG campaign with:

```bash
python3 "$EXPERIMENTS/analyze_fd_pcg.py" "$FD_CAMPAIGN" \
  --output "$FD_CAMPAIGN/analysis"
```

The analyzers report recurring and initial solve time, complete timestep time,
iterations, achieved fresh residual, setup cost, memory, and paired speedups.
Optional figures require Matplotlib; campaign submission and validation use only
the Python standard library.

`gh200_validate.sh`/`.slurm` provide the maintained CUDA, sanitizer, MPI, and
oracle checks. `gh200_efficiency.slurm` exercises compact timers, reuse, unit
tests, and representative PIC configurations. `check_compact_timers.py`,
`check_landau.py`, and `check_variants.py` remain focused diagnostic helpers.
Build with `-j 8`.

## Maintained implementation files

| Role | Files |
|---|---|
| Public submission | `submit_fem_campaign.py`, `submit_fd_pcg_campaign.py`, `submit_multigrid_missing.py`; root helper `submit_fem_parameter_grid.sh` |
| Shared planning/execution | `submit_campaigns.py`, `run_mesh_sweep.py`, `ordinary_measurements.py`, `fd_pcg_variants.py`, `rank_memory.py`, `successful_campaign_variants.py` |
| Slurm dispatch | `gh200_campaign*.slurm`, `campaign_task*.py`, `gh200_mesh*.slurm`, `gh200_build_performance.slurm` |
| Analysis | `analyze_mesh_sweep.py`, `analyze_fd_pcg.py` |
| Validation | `gh200_validate.sh`, `gh200_validate.slurm`, `gh200_efficiency.slurm`, `classify_mpi_memcheck.py`, `check_compact_timers.py`, `check_landau.py`, `check_variants.py` |
| Special recovery | `resume_missing_campaign.py` (missing states only; never retries recorded failures) |
| Regression tests | `test_phase1_campaigns.py`, `test_run_mesh_sweep.py`, `test_resume_missing_campaign.py`, `test_compact_timers.py` |

Historical replay analyzers, frozen-baseline preparation, the pre-rewrite FEM
DAG, and redundant application-specific shell wrappers are kept in the external
experiment archive rather than this active source directory.
