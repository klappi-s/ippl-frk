# Mesh trends and later GH200 studies

The scripts use `LandauDampingFEM` and the existing replay machinery. They add no
solver implementation. `run_mesh_sweep.py` calls `run_reuse.py` in two batches:

| Batch | Configurations |
|---|---|
| baseline | CG, Jacobi, GS inner/outer 2/2, Chebyshev degree 8 |
| tuned | CG, GS 2/1, Chebyshev degree 16 |

CG appears in each batch so every candidate has a paired same-load baseline.
Three repetitions rotate methods and alternate batch order. Each method starts
with fresh caches, then reuses setup across eleven captured loads. All runs use
`constant_preserving`, full reuse, tolerance 1e-10 and previous-potential guesses
unless overridden. Multigrid, Newton and duplicate SSOR/Richardson variants are
outside this focused sweep; earlier results did not make them priority candidates.

## Local Serial campaign

```sh
python run_mesh_sweep.py --executable /path/to/LandauDampingFEM \
  --output results/serial-mesh
python analyze_mesh_sweep.py results/serial-mesh
```

Defaults are P3 GLL; GL(p+1); one MPI rank; 32,768 particles; ten timesteps;
1D cells 128/256/512/1024, 2D 8/16/32 per axis, and 3D 3/4/6 per axis. The box
has length 4π in each direction. Independent periodic DOFs are `(p*cells)^dim`.
GL(p+1) exactly integrates the affine constant-coefficient stiffness; it does not
make the cosine-based Fourier diagnostic exact. The fixed particle count isolates
mesh work and keeps the same initial ensemble within a Serial dimensional sweep;
particles per cell decrease with refinement. This is not an accuracy-convergence
experiment. Use `--particles-per-cell N` for a different, explicitly recorded policy.

Useful options include `--dimensions 3 --cells-3d 8 12 16`, `--orders 2 3`,
`--family equispaced`, `--repeats 5`, `--steps 10`, `--initial-guess zero`,
`--timer-level detailed`, `--ranks 4` and
`--launcher 'srun -n {ranks} --cpu-bind=cores /path/to/site-wrapper'`.
The launch prefix is tokenized without a shell; shell pipelines/substitutions
are not executed. MPI wrappers and options can be supplied without editing scripts.

`--dry-run` creates a reviewable `campaign.json` and lists indexed jobs.
`--resume` skips completed jobs, rejects a changed configuration/binary/replay
runner, and retains interrupted/failed attempts when retrying. `--job-index N`
runs just one manifest job, allowing independent Slurm array allocations. Avoid
running the same job index concurrently. `--timeout` bounds each whole batch;
timeouts are failures, not timing samples. Default execution is sequential.

To extend an existing study without rerunning or altering its measurements, run
the additional meshes into a new directory and combine them during analysis:

```sh
python run_mesh_sweep.py --executable /path/to/LandauDampingFEM \
  --output results/larger-mesh --dimensions 2 3 \
  --cells-2d 64 --cells-3d 8 16 --timeout 14400
python analyze_mesh_sweep.py results/serial-mesh \
  --include results/larger-mesh --output results/combined
```

The analyzer rejects overlapping cases and incompatible solver/particle settings.
It preserves source manifests and raw-data paths in the combined output. Different
binary hashes are explicitly disclosed: verify source compatibility before
pooling results from rebuilt executables. Original measurements remain intact.
If the scope is deliberately reduced, use e.g. `--exclude-mesh 3:16
--exclusion-reason 'Omitted by runtime decision'`. The report and
`analysis_selection.json` list the omissions separately from failed/missing jobs;
the original campaign plan remains intact.

The analyzer checks all planned jobs, solve counts, success flags, fresh residuals,
fixed-operator setup counts and matching RHS-norm sequences. Exact load identity
within a batch comes from reusing the same in-memory fields. Across batches the
canonical CG trajectory is regenerated with the same configuration and seed;
RHS norms provide a consistency check, not a collision-proof field checksum.
GPU random sampling may regenerate different sequences between batches/repeats.
The default analyzer stops on that difference. After inspecting it, use
`--allow-regenerated-load-variation` to analyze paired comparisons and explicitly
label the differing loads; within-batch candidate/CG parity is still required.

Outputs:

- `summary.csv`: one configuration/repetition per row, iterations, first/recurring/
  total costs, paired speedups, work counts, residuals and measured/modelled crossings.
- `p3_mesh_trends.png/.pdf`: iterations, recurring seconds and paired speedups
  against cells per axis, separated by dimension. Bands are min–max, not confidence intervals.
- `p3_setup_and_total.png/.pdf`: first setup and complete eleven-solve cost.
- `p3_dof_cost.png/.pdf`: dimension comparisons normalized by DOFs and outer
  iterations; these include all solver work, not just a matrix-vector product.
- `README.md`, `campaign.json`, `jobs/*.json`: summary, provenance and exact commands.
- Each attempt directory: original application configuration and per-rank CSVs.

Rank totals are summed before selecting the maximum. Application time is already
inside Krylov time. Fresh residual diagnostics and canonical load capture are
outside replay solve timing. Canonical PIC results are **plain CG only**; the
scripts do not fabricate PCG timestep costs by substituting replay times.
For selected full-PIC comparisons, the existing runner now also accepts dimensions,
quadrature and method subsets, for example:

```sh
python run_pic.py --executable /path/to/LandauDampingFEM --output results/pic-3d \
  --dimension 3 --cells 6 --quadrature exact --methods plain jacobi
python -m unittest test_replay_measurements.py
```

`inspect_diagonal_scaling.py --output diagonal.json` is an optional Basix/NumPy
calculation of the P3 GLL diagonal spread across dimensions. It tensorizes exact
1D mass/stiffness contractions with periodic incidence counts. Diagonal spread
helps explain Jacobi's role; it is **not** the stiffness condition number.

## GH200 build and correctness gate

Use the site's supported compiler, CUDA, MPI and Kokkos environment. The existing
IPPL GH200 CI configuration in `ci/cscs/cuda/build_sm90.yml` is the repository's
starting point. Configure the same project with:

```sh
cmake -S /path/to/ippl-frk -B /path/to/build-gh200 \
  -DCMAKE_BUILD_TYPE=Release -DIPPL_PLATFORMS=CUDA \
  -DKokkos_ARCH_HOPPER90=ON -DCMAKE_CUDA_ARCHITECTURES=90 \
  -DIPPL_ENABLE_SOLVERS=ON -DIPPL_ENABLE_ELECTROSTATIC_FEM=ON \
  -DIPPL_ELECTROSTATIC_FEM_DIMS='1;2;3' -DIPPL_ELECTROSTATIC_FEM_ORDERS=3 \
  -DIPPL_ENABLE_UNIT_TESTS=ON
cmake --build /path/to/build-gh200 --target LandauDampingFEM \
  FEMReuse FEMPreconditionerProperties FEMParticleCoupling -j 8
```

The selectable order list reduces template compilation for a focused study;
default remains P1–P5. Compiler/dependency prefixes and the CTest MPI launcher
must match the selected site environment. This is a configure template, **not a
GPU build already validated here**. No local CUDA or multi-node performance claim
is made by the Serial measurements.

First run the named unit tests and a small ten-step P3 demo on one GPU, then
repeat on two GPUs and across nodes. Use the configured CTest MPI wrapper, with
one GPU assigned to every rank. Check backend=`Cuda`, actual rank/particle counts,
fresh residuals, charge, gauge and weak/quadrature energy consistency. Compare
CG/PCG solutions and cache counts before starting a large timing sweep. Extend to
the periodic/oracle suites when debugging numerical or halo differences.

## Slurm execution

`gh200_mesh.slurm` follows the existing IPPL GH200 job structure: four ranks per
node, one GPU per rank, and 72 allocated CPU cores per task. Account, partition,
uenv/container and any MPI wrapper remain site/user choices. Current CSCS guidance
uses `--ntasks-per-node=4 --gpus-per-task=1` for one GH200 GPU per rank;
Slurm handles GPU visibility. See [CSCS Slurm documentation](https://docs.cscs.ch/running/slurm/#one-rank-per-gpu).

```sh
export FEM_EXPERIMENTS=/absolute/ippl-frk/demos/electrostaticFEM/experiments
export FEM_EXECUTABLE=/absolute/build-gh200/demos/electrostaticFEM/LandauDampingFEM
export FEM_OUTPUT=/shared/scratch/fem-gh200-one-node
# Add your site's environment/partition options to this submission:
sbatch --account=YOUR_ACCOUNT --nodes=1 "$FEM_EXPERIMENTS/gh200_mesh.slurm"
```

The batch shell runs one Python driver; **do not launch one driver per MPI rank**.
Each child `srun` uses the allocation's ranks. The default GPU sweep is 3D P3,
16³/24³/32³ cells, 64 particles/cell. These are starting workloads, not established
saturation sizes. Override the mesh at submission and use distinct outputs:

```sh
export FEM_OUTPUT=/shared/scratch/fem-gh200-four-nodes
sbatch --account=YOUR_ACCOUNT --nodes=4 "$FEM_EXPERIMENTS/gh200_mesh.slurm" \
  --cells-3d 32 48 64 --particles-per-cell 32
```

For strong scaling, keep global cells/particles fixed while changing node count.
For weak scaling, keep cells/particles per rank approximately fixed and record
the actual partition. Do not mix these with the mesh-refinement plots. Particle
sampling currently depends on rank/backend: identical seeds do not guarantee
identical global loads between CPU/GPU or different rank counts. Strict
cross-backend same-load comparisons need an exported/replayed common load archive
or a deterministic rank-independent source; that is a separate extension.

Use a fresh output directory per backend/rank-count/configuration. Run plotting
after measurement or after downloading the data; set `FEM_ANALYZE=1` only if the
batch environment has NumPy and Matplotlib. Runners themselves use the standard
Python library. Retained loads, field temporaries and buffered timers/CSV data
consume memory; scale in steps and record memory before very large runs.

## Expected GPU differences

The mathematical mesh dependence remains, and identical matrices/loads should
give broadly similar iteration counts. **Wall-time rankings may change.** GPUs
reward enough parallel work and efficient memory traffic; tiny fields and many
short kernels expose launch overhead. Jacobi is simple but adds field operations;
Chebyshev trades extra operator actions for fewer outer iterations/reductions.
The current GS implementation also performs several field/triangular operations;
its name alone does not imply an efficient GPU triangular solve. These are
expectations to test, not predictions of a winner. See [NVIDIA's performance
guide](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/).

Begin with coarse timers and profile selected cases with Kokkos Tools/Nsight.
Existing IPPL timer fences and the new detailed action fences can serialize GPU
work; do not compare differently instrumented builds as if the algorithm alone
changed. Preserve fences required for correct end-to-end timing and record any
profiling/fence changes. [Kokkos explains asynchronous timing and fences](https://kokkos.org/kokkos-core-wiki/API/core/parallel-dispatch/fence.html).
Across nodes, halo exchanges and global reductions add another tradeoff: fewer
outer iterations can help, while extra preconditioner operator actions also
communicate. Measure both.
