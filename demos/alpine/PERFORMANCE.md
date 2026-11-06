# LandauDamping CG/PCG measurements

The ordinary campaign uses the existing **3D** Alpine application and
`ippl::PoissonCG`. See the shared [campaign manual](../electrostaticFEM/experiments/MESH_STUDY.md)
for submission, mesh lists, GPU placements, resumability and analysis.

The current workflow first measures plain FD CG with the dedicated `placement`
phase, then tunes PCG on shortlisted allocations. Use
`submit_fd_pcg_campaign.py` from the shared experiment directory for both stages;
its output keeps placement results separate from parameter tuning. The
application options below are unchanged. All performance runs use eight particles
per cell: pass `8*nx*ny*nz` as the total particle argument. The campaign submitter
derives this automatically. Use its staged `first` and `repeats` submissions; see
the shared manual for examples, compact memory monitoring, resumability and
capacity checks. The placement grid starts with 1×2 rather than 1×1 for the large
problems.
At 512³, the current payload floor averages 44.5 GiB per GPU on 1×2, before
halos, temporary storage and imbalance. All allocations still need peak-memory checks.

The existing positional CLI is retained. For example:

```bash
mpiexec -n 2 ./LandauDamping 16 16 16 32768 10 PCG 1.0 LeapFrog chebyshev 8 \
  --benchmark-output /absolute/path/to/new-run \
  --benchmark-tolerance 1e-10 --benchmark-max-iterations 10000 --benchmark-dt 0.05
```

Use `CG 1.0 LeapFrog` for plain CG, `PCG 1.0 LeapFrog jacobi` for Jacobi, or
`PCG 1.0 LeapFrog gauss-seidel 2 1 1` for GS inner=2, outer=1, communication=1.
Benchmark mode requires CG/PCG and balancing threshold 1.0. It initializes the
potential to zero, performs one physical initial solve and the requested steps,
and uses previous potentials afterward. It skips the legacy dummy warmup. Each
configuration is a fresh application process; there is no replay or solver switch.

`--benchmark-output` isolates all output in a new run directory and enables
summary-only timers. It writes `configuration.txt`, `solves-rankN.csv`,
`steps-rankN.csv`, `physics.csv` and `timer-summary.csv`, alongside normal Alpine
diagnostics. Solve timing surrounds the existing solver call, including its
electric-field gradient. Fresh `rho+laplace(phi)` residual verification is timed
separately. Timestep timing includes normal particle work, verification and dump
diagnostics. Initial solver construction is recorded separately; no finer setup/
application breakdown is fabricated. The full solver and preconditioner objects
persist across timesteps; their existing field/layout refresh behavior is retained.
The multigrid hierarchy now also persists: a collective mesh/layout/BC signature triggers
a rebuild only after a real change. `solves-rankN.csv` records cumulative
`preconditioner_setups`, `preconditioner_levels`, and
`preconditioner_coarsest_local_extent`; a static-mesh run must report one setup in every
row. Other preconditioners retain their scratch fields and still perform their inexpensive
layout refresh before each solve.

The current all-family workflow is documented in the shared campaign manual and
`ProjectManagement/2026-09-25_0036_FD_PCG_TUNING_PLAN.md`. It tunes every family at both
256³ and 512³, transfers finalists to 128³ and 1024³, and permits full 1024³ tuning after
the required large-particle-count preflight.

Periodic FD PCG requires an SPD preconditioner. For Richardson paths using the full periodic
Laplacian, use an odd number of undamped sweeps: the campaign scans `1,3,5,7`. An even count
annihilates the Jacobi-scaled Nyquist mode and can make the internal preconditioned residual look
converged while the fresh residual is large. The communication-free block variant retains the
historical `1,2,4,8` scan, subject to the same fresh-residual check.

CG's recursive residual is an L2 norm. PCG's is a preconditioned norm relative to
its initial value, so compare the **fresh_relative_residual** column when judging
achieved accuracy. The common gate is `max(20*tolerance,5e-11)`. Benchmark mode
overrides the legacy tolerance (1e-4) and mesh-dependent dt; the defaults without
this mode are unchanged. Physics CSV energy is the existing **Ex-only** diagnostic,
not FEM's total electric-field energy. The product perturbation, random sampling,
charge conservation checks and leapfrog path remain the normal Alpine implementation.
