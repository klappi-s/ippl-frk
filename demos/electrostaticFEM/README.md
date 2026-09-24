# LandauDampingFEM

A periodic electrostatic PIC demo using the higher-order `FEMContainer`, existing
IPPL CG/PCG and particle migration. It measures repeated Poisson solves on a fixed
uniform mesh. Actual ORB repartitioning remains commented out in `advance()`;
`FEMContainer` redistribution must be implemented before enabling it.

## Build and run

Add these options to your existing IPPL configuration:

```sh
cmake -S /path/to/ippl-frk -B /path/to/build \
  -DIPPL_ENABLE_SOLVERS=ON -DIPPL_ENABLE_ELECTROSTATIC_FEM=ON
cmake --build /path/to/build --target LandauDampingFEM -j 8
mpiexec -n 1 /path/to/build/demos/electrostaticFEM/LandauDampingFEM \
  --dim 1 --order 3 --cells 128 --particles 32768 --steps 10 \
  --reuse full --output results/p3-cg
```

The executable does not depend on Alpine or FFT support. Dimensions 1–3, P1–P5
and both quadrature choices are compiled by default. To reduce compilation cost,
set `-DIPPL_ELECTROSTATIC_FEM_DIMS=1` (a semicolon-separated CMake list).
Likewise, `-DIPPL_ELECTROSTATIC_FEM_ORDERS=3` compiles only P3 for a focused study;
the default order list is `1;2;3;4;5`.
Ten timesteps perform **eleven PIC solves**, including the initial field solve.
An existing run's output directory is rejected.

| Option | Default / meaning |
|---|---|
| `--dim`, `--order` | `1`, `3`; supported ranges 1–3 and 1–5 |
| `--cells` | 64 in 1D, 16 per axis in 2D, 8 per axis in 3D; **cells**, not vertices |
| `--lengths` | `12.566370614359172`; scalar or comma-separated per axis |
| `--interpolation_nodes` | `gll` or `equispaced` |
| `--quadrature_points` | `9` or `exact` (= GL with p+1 points/axis); both integrate affine stiffness/mass exactly |
| `--particles`, `--seed` | 32768, 42; rank-dependent sampling streams; actual global count is checked |
| `--steps`, `--dt` | 10, 0.05 |
| `--alpha`, `--mode`, `--thermal_speed` | 0.05, 1, 1; integer wave mode and velocity standard deviation |
| `--initial_condition` | `single`: perturb x only; `alpine_product`: perturb every axis |
| `--solver`, `--preconditioner_type` | `plain`, `jacobi`; select `preconditioned` to enable PCG |
| `--tolerance`, `--max_iterations` | 1e-10, 10000 |
| `--poisson_stiffness_mode` | `constant_preserving`; `standard` is a diagnostic alternative |
| `--reuse` | `full`; alternatives `operator`, `off` |
| `--initial_guess` | `previous`; alternative `zero`, applied equally in replay |
| `--timer_level` | `coarse`; `detailed` additionally times each preconditioner application |
| `--diagnostics_every` | 1; initial and final diagnostics always recorded |
| `--replay` | Empty; e.g. `plain,jacobi,gauss-seidel,chebyshev` captures and replays loads |
| `--output` | `landau-fem`; use a separate directory for every run |

Every axis needs at least two cells and every rank must own elements. P1/P2 GLL
and equispaced interpolation coincide. Stretched boxes are supported. No adaptive
mesh, mixed boundary conditions or multigrid option is provided here.

Preconditioner controls retain the convergence-driver names:
`--newton_level 2`, `--chebyshev_degree 8`, `--richardson_iterations 4`,
`--gauss_seidel_inner_iterations 2`, `--gauss_seidel_outer_iterations 2`,
`--ssor_omega 1`. Available methods are Jacobi, Newton, Chebyshev, GS, SSOR and the
two Richardson variants. Even undamped Richardson sweep counts are rejected by
the demo: the four-sweep P3 periodic probes are indefinite. Odd counts remain
available for diagnosis; three sweeps pass the projected-matrix checks. Those
tests also cover Chebyshev degrees 8/16 and GS 2/2 and 2/1;
this is not a proof that every configurable smoother is valid or fast. PCG also
checks curvature, and every completed application solve checks a fresh residual.

## Numerical contract

Normalized units use electron q/m = −1, ion density 1 and Maxwellian velocities.
The standard box has k=0.5 and electron density `1+alpha*cos(k*x)`. Particle
charge is `-volume/actual_particle_count`. Leapfrog uses a half kick, drift,
periodic migration, field solve, then half kick.

All higher-order vertex, edge, face and interior DOFs participate. Deposition
forms `b_i=sum(q_p*phi_i(x_p))+integral(phi_i)` directly. It does **not** divide
by a lumped mass and does **not** pass through nodal load assembly a second time.
Gathering evaluates `E=-grad(phi_h)` at particle positions. These operations live
in `src/FEM/FEMContainerInterpolate.h` and share the existing FEM basis/mapping.

Periodic endpoints are aliases, leaving `product(p*cells)` independent DOFs.
The solver checks total charge, projects numerical roundoff and solves on the
algebraic mean-zero subspace. It then subtracts the **physical** FEM mean from
the potential. `solveLoadVector(const Field&)` preserves the supplied load;
the established `solve()` API still assembles a load from nodal source samples.

Periodic Newton/Chebyshev use conservative analytical spectral bounds, avoiding
the nullspace all-ones power seed. With element Jacobian determinant J, longest
period L and degree p, the lower bound is
`0.5*(2*pi/L)^2*J/(p+1)^(3*dim)`; the upper bound is `1.01*2^dim*||K_element||_inf`.
The lower estimate follows from periodic Poincare and the tensor-polynomial
point-evaluation bound after removing the physical mean; the upper estimate
bounds incident element row sums. The interval is checked against a small full
periodic spectrum. It is deliberately conservative and may weaken polynomial
preconditioning; no arbitrary fallback interval is used in the periodic path.

## Reuse and measurements

| Mode | Retained between solves |
|---|---|
| `off` | No prepared operator/preconditioner state; element integration repeats during actions |
| `operator` | Congruent element matrix, periodic constant vector and integration weights |
| `full` | Also diagonal, preconditioner object/scratch and Krylov workspace containers |

An operator signature tracks geometry, family, quadrature, stiffness mode,
boundary topology and local partition. Preconditioner parameters have a separate
signature. Changed load coefficients, tolerance or initial guess retain setup.
`setRhs()` explicitly rebinds space/storage and invalidates; direct space/layout
mutation requires reinitialization/invalidation. Arbitrary runtime repartitioning
is unsupported. Persistent workspace containers still coexist with temporary
fields returned by the existing expression/operator APIs; this is not an
allocation-free Krylov implementation.

Operator result containers copy the input's prepared entity layouts through
`cloneStructure()`. They allocate independent zeroed values with the same
`NO_FACE` boundary defaults as a fresh container, without repartitioning or
rediscovering neighbors. The returned fields own their entity-layout objects;
the original mesh and vertex layout retain their normal lifetime requirements.

Replay first evolves one canonical **plain-CG PIC trajectory**, stores its eleven
assembled loads in memory and then solves the exact same stored sequence for each
listed method. Each method starts with fresh caches and zero potential; later
guesses follow `--initial_guess`. Capture is outside solve timing. Memory scales
as the number of saved loads times distributed field storage. Replay is an
experiment, not a restart/checkpoint format.

Each run writes:

| File | Contents |
|---|---|
| `configuration.txt` | Resolved geometry, physics, numerical controls, backend/ranks, source revision and explicit options |
| `solves-rankN.csv` | Per-solve iterations, recursive/fresh residuals, RHS norm, setup/Krylov/full-solve timings, application counts and spectral bounds |
| `steps-rankN.csv` | PIC stages, including deposition/gather local work, halos and ownership checks; particle imbalance |
| `physics.csv` | Charge, mean potential, fundamental electric amplitude, quadrature and weak field energies, kinetic energy, energy drift, momentum |
| `timer-summary.csv` | One row per timer and rank: call count, total/min/max/mean/stddev seconds and stored sample count |
| `timers.csv` | Only with `--timer_recording raw`: the first bounded number of samples per timer/rank |

`--timer_recording summary` is the default. Internal timers maintain online
statistics and retain no per-event vectors. `--timer_recording raw` with
`--timer_sample_limit 1024` opts into bounded diagnostic capture (the limit must
be positive). Summary statistics always cover **all** calls; the raw CSV is a
prefix sample, not a complete event trace or an unbiased sample of the run.
`stored_samples` makes truncation visible. `--timer_level coarse|detailed` remains
independent: it controls the additional fenced FEM action timings. The shared
IPPL timer API keeps its legacy unlimited default for other applications; this
demo sets its policy before constructing simulation objects.

`solve_seconds` includes setup and Krylov work. `krylov_seconds` includes
preconditioner applications: do not add them together. Detailed action/application
seconds are zero when disabled. Action times include their halo work; diagonal
and spectral preparation are children of preconditioner setup, not additional
costs. Setup seconds include a small cache-check overhead even on cache hits. Fresh residual verification is separate; the
acceptance threshold is `max(20*tolerance,5e-11)` relative to the load norm
(absolute for zero load). A max-iteration exit is treated as unsuccessful.
Quadrature energy is checked against `0.5*potential^T*load` at solve tolerance.
The Fourier diagnostic contains a cosine: GL(p+1) does not integrate it exactly.
Its quadrature error should be checked on very coarse meshes or large wave modes.
`step_total` excludes the subsequent imbalance measurement and buffered CSV
serialization; those costs have separate timers. Timings use fences, without
per-iteration MPI timing barriers.
Solve/step/physics CSV rows remain buffered until the run finishes, so their
storage grows with the number of solves/steps. Summary timer storage depends only
on the number of timer labels and ranks, not the number of calls. Timer export
occurs after all solves and is excluded from solve/Krylov timers but included in
the runner's total batch wall time. Long production runs still need a bounded
flush policy for the solve/step/physics records and captured replay loads.

Use maximum-rank latency after summing locally across solves, not sums of
separately maximized child stages. The analysis scripts preserve this distinction
and emit PDF/PNG figures plus raw CSV summaries.

```sh
python experiments/run_reuse.py --executable /path/to/LandauDampingFEM \
  --output results/reuse
python experiments/analyze_reuse.py results/reuse
# Complete PIC comparison (separate trajectories):
python experiments/run_pic.py --executable /path/to/LandauDampingFEM \
  --output results/pic
# Optional parameter comparison:
python experiments/run_reuse.py --executable /path/to/LandauDampingFEM \
  --output results/tuned --reuse full --degree 16 --gs-outer 1
# Interleaved instrumentation comparison:
python experiments/check_timer_overhead.py --executable /path/to/LandauDampingFEM \
  --output results/timer-overhead
# Separate physical check; requires NumPy, SciPy and Matplotlib:
python experiments/check_landau.py --executable /path/to/LandauDampingFEM \
  --output results/landau
```

The reuse runner rotates method/mode order, performs three sequential repetitions
and records commands and executable SHA256. Run it without concurrent builds.
Compare a coarse-timer repeat to quantify detailed instrumentation overhead.
Fewer iterations alone are not a speedup: include setup, applications and complete
PIC cost. A ten-step timing pilot cannot establish a Landau damping rate or a
long-run energy guarantee. The separate physical script fits early electric
amplitude peaks against a SciPy dispersion root and varies dt, cells and particles.

Correctness tests are `FEMPeriodic`, `FEMParticleCoupling`, `FEMReuse`,
`FEMPreconditionerProperties`, plus the existing Dirichlet/oracle regressions.
Periodic matrix and coupling references tensorize existing published Basix 1D
data; no large generated data headers or new package release are required.

For mesh trends in 1D–3D and later GH200 runs, see
[MESH_STUDY.md](experiments/MESH_STUDY.md). The resumable `run_mesh_sweep.py`
uses the same replay runner, and `analyze_mesh_sweep.py` produces mesh/iteration/
cost/speedup plots. A Slurm template supplies one GPU per rank; GPU execution
still requires validation in the target environment.
