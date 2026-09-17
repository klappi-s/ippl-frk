# FEM oracle data

The pinned **ipplTestData** dependency supplies external numerical expectations
for ordinary IPPL GoogleTests. Its Python generator uses Basix for basis tables,
DOLFINx/UFL for assembly, and SciPy for constrained solutions, with independent
analytical checks. No IPPL result supplies an expected value.

Numerical arrays live in compact runtime files outside IPPL's source tree. Small
generated headers describe the records; the standard C++20 loader needs no Python
or additional parsing library. `Oracle/` contains the schema, loader, dependency
lock, fixtures, and [ordered file/case catalog](Oracle/oracles.md). The generator,
environment YAML, and release tooling live in the separate `ipplTestData` source
package at [klappi-s/ipplTestData](https://github.com/klappi-s/ipplTestData).
See its [storage report](https://github.com/klappi-s/ipplTestData/blob/master/STORAGE_REPORT.md).

**Current coverage:** P1–P5 in dimensions 1–3, equispaced and GLL,
plus translated/stretched P3 meshes: 36 cases. `FEMReferenceOracle.cpp` runs
ordinary IPPL GoogleTests in double and float. `FEMOperatorOracle.cpp` uses the
same registry for field assembly and diagnostics. `FEMPoissonOracle.cpp` tests
complete CG/Jacobi solves in double for all 36 cases, with zero/constant Dirichlet
data and a separate zero-load solve from a nonzero initial guess.

## Run the tests

With `IPPL_ENABLE_UNIT_TESTS=ON`, CMake fetches the pinned release during
configuration. Keep your usual IPPL toolchain and dependency settings. From the
**fem workspace**, this also clears any earlier local oracle overrides:

```bash
cmake -S ipplFEM/ippl-frk -B ipplFEM/build_serial \
  -DIPPL_ENABLE_UNIT_TESTS=ON -DIPPL_FEM_ORACLE_DOWNLOAD=ON \
  -DIPPL_FEM_ORACLE_ARCHIVE= -DIPPL_FEM_ORACLE_ROOT=
```

`Oracle/ipplTestData.lock.json` pins the archive and manifest SHA-256 hashes.
CMake verifies every payload and the schema, including local overrides. The lock targets the `v1.0.0` data asset in
[klappi-s/ipplTestData](https://github.com/klappi-s/ipplTestData/releases).
CMake downloads/extracts it automatically when no local override or installed
package is provided. The published release and a fresh pinned CMake download were
verified on 16 September 2026; all payloads match the dataset that passed the 104
CTest registrations. Downloaded-cache reuse also passed with FetchContent fully
disconnected. The default extraction directory is
`<build>/_deps/ippl_fem_oracle_<archive-sha256>-src/`. Later builds reuse it;
tests read its `data/*.bin` files at runtime. Builds without unit tests do not
fetch these data, and ordinary builds/tests need no sibling `ipplTestData` checkout.

For offline configuration, supply an exact release archive through
`IPPL_FEM_ORACLE_ARCHIVE`, an extracted `IPPL_FEM_ORACLE_ROOT`, or an installed
package on `CMAKE_PREFIX_PATH`, and set `IPPL_FEM_ORACLE_DOWNLOAD=OFF`.

From the **fem workspace**, with FFT, solvers, and unit tests enabled:

```bash
cmake --build ipplFEM/build_serial --target FEMReferenceOracle1D FEMReferenceOracle2D FEMReferenceOracle3D -j 12
cmake --build ipplFEM/build_serial --target FEMOperatorOracle1D FEMOperatorOracle2D FEMOperatorOracle3D -j 12
cmake --build ipplFEM/build_serial --target FEMPoissonOracle1D FEMPoissonOracle2D FEMPoissonOracle3D FEMOracleRuntimeTest -j 12
ctest --test-dir ipplFEM/build_serial -R '^FEM((Reference|Operator|Poisson)Oracle|OracleRuntimeTest)' -j 4 --output-on-failure
```

Every registration uses the normal MPI launcher and configured Kokkos backend.
Reference/single-cell tests use one rank; whole-field operator and solve tests also
run on two ranks. Targeted four-rank 3D tests check at least two split axes. Separate dimension targets bound compilation memory. The 3D column loop is
partitioned across 16 CTest runs of the same binary; all columns still run exactly
once, and each run retains the normal 60-second timeout. Use the regex above to
include these runs. The three load-related stages are also separated by precision
to keep P5 within the same limit. `-j 4` runs independent MPI test launches concurrently. Ordinary
builds/tests use the pinned runtime files and do not run Python. The operator suite compiles one translation unit per generated type, and a single
loader object is shared by reference, operator, and solver targets. This bounds compiler memory without
changing runtime coverage. The 3D solve suite uses
GoogleTest sharding across 24 launches per rank count; every generated solve runs
once. Four-rank solves select stretched P3 and both P5 families in 12 shards.
`FEMThinHalo` separately checks thin 2D/3D partitions and corner accumulation.
Sharding follows dynamically registered cases, so adding a case cannot silently
omit its solve. CTest `PROCESSORS` accounts for MPI rank counts.

The reference suite checks:

- Physical/reference coordinates, transformations, Jacobians and determinants.
- Local/global DOF indices, entity types/offsets, owned cells, boundary flags,
  and the device mirror's numbering against external coordinates/connectivity.
- Host basis values/gradients, device basis values, nodal Kronecker identities,
  and reproduction of all tensor monomials through coordinate degree p.
- Quadrature points/weights/exactness and reference/physical mass and stiffness
  matrices, using IPPL's basis, gradients, quadrature, and Poisson `EvalFunctor`.

Element matrices here are integrals of the production primitives, computed
without a field adapter. The operator suite then checks `evaluateAx` and
`evaluateLoadVector` through actual fields. Gradient
tabulation follows the current host assembly path; device basis/mapping checks
execute Kokkos kernels. A serial build does not establish GPU correctness.

Double comparisons use approximately `1e-12` absolute and `1e-10` relative
accuracy. Float basis/gradient comparisons use separate tolerances around
`1e-5`; matrix tolerance is `1e-6 + 4*epsilon_float*nQuad*sqrt(A_ii*A_jj)` to
account for accumulation and near-zero entries. These are fixed test criteria.

## Operator, load, boundary, and norm checks

The operator suite uses `NO_FACE` to compare **every column** of each single-cell
stiffness and mass matrix through `evaluateAx`. Nodal impulse loads are checked
against the same mass columns, followed by an asymmetric nodal source.
The one-cell geometry has the spacing and origin of the corresponding patch.

Whole-field tests cover:

- Raw stiffness/mass actions, including physical boundaries, plus impulses for
  every available vertex/edge/face/interior orientation.
- Constant and asymmetric loads, retaining source samples at constrained solution
  DOFs. The source is `1 + sum((d+1)*(2*x_d - 3*x_d^2 + 0.5*x_d^(p+1)))`.
  It lies above interpolation degree p; its analytical integral is deliberately
  different from the nodal-interpolant load.
- The existing convergence driver's `assignSourceToField` with its
  `HighOrderPolynomial` source, both node families, and translated/stretched boxes.
- Zero and constant Dirichlet operator stages, `A_FC*g`, and `b_F-A_FC*g`, with
  `g=0.75` supplied as boundary coefficients. The solver's BC setup is separate.
- `D*x`, `D^-1*x`, strict global lower/upper actions, and off-diagonal action.
  Lower means **row > column**; upper means **row < column** in x-fastest numbering.
  These splits refer to the free block; constant-BC helpers retain their existing
  convention of copying the input on constrained rows, once per owner.
- Coefficient L1/L2, maximum absolute coefficient (using existing min/max), and
  inner product with one/two deliberately poisoned ghost layers. Physical L² and
  nonzero absolute error use `computeErrorL2`; relative error is computed separately.
  The diagnostic exact function is `1 + sum((d+1)*x_d*(1-x_d)*(1+2*x_d))`.

The small distributed adapter visits each owned entity once using its free axes and
x-fastest interior offsets, independently of DOFHandler's lookup tables. Production
`fillFromGlobalCoefficients` is checked through this adapter using both asymmetric
coefficients and external coordinates. Mirrors are copied explicitly between host
and configured Kokkos memory. Owned subdomain coordinates determine the global
indices; two MPI allreduces gather coefficients and prove exactly one owner per
DOF. Ghost slots never contribute to the comparison.

Operator tolerances use `2e-12 + 1e-10*scale` in double and
`1e-6 + 4*epsilon_float*nQuad*scale` in float. For global actions, scale is the
sum of absolute CSR terms; element columns use `sqrt(A_ii*A_jj)`. This accounts
for quadrature accumulation and cancellation without weakening checks at zero.
The convergence-source sampler has a separate float forward-error allowance:
`1e-6 + epsilon_float*sampledConvergenceScale`. Python derives the scale from
32 times the absolute source terms plus the source gradient times an eight-epsilon
coordinate-formation bound on the box. This is necessary near cancellation: at
`(7/36,3,-0.75)` the source combines terms about `-88.32`, `5.07`, and `83.36` to
produce `-0.10927`; a roughly `9e-6` float discrepancy is roundoff, not a changed
source or mapping. The immutable double source expectations remain unchanged.
The sampling allowance propagates through the external mass matrix for its load
check. Python also checks this bound against NumPy float sampling.

## Complete solves

Each registry case has six named solves: CG and Jacobi-PCG for zero Dirichlet,
constant `g=0.75`, and zero source with zero Dirichlet. All use fresh fields;
source samples and the raw load remain immutable in the generated data. The
initial guess is asymmetric and deliberately has incorrect boundary values.

The tests compare full coefficients with SciPy/DOLFINx solutions, prescribed DOFs
separately, and free residuals from both **generated CSR A** and production
`evaluateAx`. They also check finite results, the 600-iteration ceiling, and an
IPPL/MPI coefficient error norm with poisoned ghosts. Solver-reported convergence
alone does not establish correctness. Coefficient error limits are
`1e-11 + 1e-9*||u_external||`; free residual limits are `1e-10 + 1e-9*||b_F||`.
The configured solver tolerance is `1e-12`, with an absolute fallback at zero RHS.

Dirichlet metadata belongs to the LHS; RHS input is the nodal source. The solver
assigns owned boundary coefficients, assembles the nodal load, subtracts `A_FC*g`,
and applies homogeneous constraints to Krylov directions. Supported boundary
scope here is one common constant on all faces; fully periodic behavior is not
validated by these tests. Mixed or inconsistent face data is rejected explicitly.

## Recreate the data

From the separate **ipplTestData source checkout**, create/activate the
pinned generation environment, then run:

```bash
micromamba create -f oracle-environment.yml
micromamba activate ippl-fem-oracles
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1
python generator/create_FEMOracleData.py --archive releases/ipplTestData-1.0.0.tar.gz
python generator/create_FEMOracleData.py --check --archive releases/ipplTestData-1.0.0.tar.gz
python -m unittest discover -s tests -v
```

A C compiler is also needed for FFCx's generated assembly kernels. The tested
environment was the workspace's existing `.fenicsMambaEnv` on Linux x86_64:
Python 3.12.13, Basix/DOLFINx/FFCx 0.11.0, UFL 2026.1.0, NumPy 2.4.6,
SciPy 1.17.1, mpi4py 4.1.2, MPICH 5.0.1, PETSc/petsc4py 3.25.2 (real scalars),
OpenBLAS 0.3.33, and CFFI 2.0.0; the JIT C compiler was GCC 16.2.1. The YAML pins this numerical stack; the original
environment was reused, rather than freshly installed from the YAML.

For that existing environment, run from the **fem workspace**:

```bash
UCX_TLS=self UCX_LOG_LEVEL=error OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 \
  .fenicsMambaEnv/bin/python \
  ipplTestData/generator/create_FEMOracleData.py --check \
  --archive ipplTestData/releases/ipplTestData-1.0.0.tar.gz
```

`UCX_TLS=self` allows this serial generator to initialize the existing MPICH/UCX
stack in a sandbox that disallows sockets. It is not a setting for MPI unit-test
runs. Generation requires one rank and uses `MPI.COMM_SELF`; the resulting data
contains no partition-specific information.

All numerical checks run before a package is written. `--check` generates into a
temporary directory and returns a failure if the stored file or archive differs or is
missing; it never updates it. `--output-dir PATH` writes or checks an alternative
location. FFCx uses a fresh temporary cache on each invocation, cleaned up on exit.

The manifest records the generator, storage writer, schema, and
environment-file SHA-256 hashes and the complete ordered case registry, plus imported Python package versions. It uses
round-trippable doubles, with no timestamps or absolute paths. Byte identity is
checked in the tested environment; a different compiler/platform or changed
dependencies can change rounding and require review. Never hand-edit an expected
number to accommodate an IPPL failure.
Archive byte identity also depends on the compression backend: the tested
generation environment uses zlib runtime 1.3.2. Packaging with the system Python's
zlib-ng produces different compressed bytes even for identical numerical data.

## The first two problems

Solve `-u'' = f_h` on `[0,1]` with four equal cells, degree three, and either
equispaced or GLL interpolation. The analytical source is
`f(x) = 1 + 2*x - 3*x^2 + 0.5*x^4`; `f_h` is its nodal FE interpolant.
There are separate solutions with `u(0)=u(1)=0` and `u(0)=u(1)=0.75`.

Both use **five Gauss–Legendre points per cell**, exactness degree nine, mapped
to `[0,1]`. The same explicit points/weights are passed to DOLFINx as custom UFL
quadrature. Source values at constrained solution DOFs are retained.

The interpolation families represent the same polynomial space, but their
interpolants of this quartic source differ. Their primary discrete solutions are
therefore allowed to differ. A separate generator check uses the exactly
represented source `-2+12*x`, whose cubic solution is `x*(1-x)*(1+2*x)`, and
verifies the solution at common physical quadrature points for each family.

## Runtime contract and access

- `Oracle/FEMOracleData.h` is the unchanged version-3 schema, including field shapes.
- Package `data/fem-{1,2,3}d.bin` contains losslessly deduplicated f64/u32 arrays.
  The corresponding JSON lists every record and array location for inspection.
- Package `include/FEMOracleBindings_{1,2,3}D.h` contains small descriptors, not large
  numerical arrays. `FEMOracleTestData.cpp` loads one immutable dataset per
  dimension and MPI process; spans stay valid for the process lifetime.
- Package `include/FEMOracleRegistry.h` and all data come from **one Python CASE_REGISTRY**.
  CMake derives compiled types from it; GoogleTests verify complete reference and
  operator coverage. Families and geometry remain runtime cases.
- `referenceCases()`, `operatorCases()`, and `legacyCases()` expose the records.
  The latter retains the two original P3 1D examples; ordinary solves consume
  `operatorCases()`, whose `boundaries[].solution` covers every configuration.
- CMake checks cryptographic integrity; `FEMOracleRuntime.h` checks file layout,
  dimensions, lengths, and span bounds. `FEMOracleRuntimeTest` covers malformed
  input and exact bit preservation. The runtime does not recalculate SHA-256.
- Arrays are row-major, with the last index varying fastest. All indices are
  **zero-based**, unlike the older reference toolkit's text files.
- Global DOFs, cells and quadrature points use x-fastest order. Local entities
  follow the existing reference toolkit's box convention: vertices, axis-grouped
  edges, faces XY/YZ/XZ, then cell interiors. In 1D this is left vertex, right
  vertex, then interiors. GLL global coordinates are not uniformly spaced;
  coordinate matching explicitly accounts for their axis nodes.
- Entity axis masks identify the free/interior directions geometrically; entity
  offsets and origins can be checked against `DOFHandler` without assuming its
  tuple numbering. `boundaryAxes` stores geometric boundary flags per global DOF.

```cpp
#include "Oracle/FEMOracleTestData.h"

const auto& c = fem_oracle::legacyCases()[0];
const auto nodes = c.reference.nodes;        // local coordinates, vertex-first
const auto f = c.source.nodalValues;         // original source samples
const auto b = c.load.preBc;                 // assembled load, including boundaries
const auto& bc = c.boundaries[1];            // nonzero constant Dirichlet
const auto expectedSolution = bc.solution;  // full global coefficient vector
```

Reference-only cases expose `reference`, `mesh`, `boundaryAxes`,
`physicalStiffness`, and `physicalMass`. They contain five external basis probes
and full element matrices. Nodal Kronecker entries are checked mathematically;
large quadrature basis tables are omitted from these cases to avoid redundant
data. Both reference and physical element matrices are cross-checked with
DOLFINx single-cell assembly. Physical matrices are shared by all uniform cells.

The complete 1D cases also include full quadrature basis tables and global
matrices in sorted CSR.
`load.analyticalSource` is a diagnostic for integrating the analytical quartic
directly; **`load.preBc` is the load IPPL should produce**.

For each boundary case, `freeDofs=F`, `constrainedDofs=C`, and
`prescribedValues=g`. `lift=A_FC*g` and `liftedRhs=b_F-lift` contain **free rows
only**. The global raw matrix is never modified. Every operator boundary case contains the full global solution vector.
Triangular actions use strict lower/upper parts of `A_FF`, defined by row/column
index, independently of any current IPPL helper naming.

`op.probe` is an asymmetric coefficient vector. Its operator actions, diagonal
actions, coefficient norms, physical FE norm, and nonzero error against the cubic
analytical function are available. The physical and coefficient norms are
different quantities; `relativeL2Error=absoluteL2Error/exactL2`.

## Checks performed during generation

| Check | What it establishes |
|---|---|
| Known analytical P1 mass/stiffness | Reference-interval normalization and stiffness sign. |
| Basix nodes vs SciPy GLL nodes/equispaced coordinates | Correct interpolation variant and local permutation. |
| Kronecker values, partition of unity, polynomial/gradient reproduction through p | Basis values and derivatives, including highest-degree modes. |
| Quadrature moments and repetition with two more GL points | Sufficient integration of the exported polynomial quantities. |
| Global coordinates, connectivity, shared-vertex incidence, exterior DOFs | Numbering and boundary classification, including nonuniform GLL spacing. |
| Local Basix contractions vs DOLFINx single-cell/global assembly | Reference/physical scaling and assembly conversion. |
| `b=M*fDof`, with analytical-load disagreement required | The load tests source interpolation and preserves boundary samples. |
| Symmetry, constant nullspace, Cholesky of mass/free stiffness | Basic mathematical properties of the assembled matrices. |
| SciPy reduced solve vs DOLFINx BC application/PETSc LU; free residuals | Boundary lifting and both final solutions. |
| Manufactured cubic and constant-boundary shift | Analytical solution checks independent of stored oracle values. |
| Physical norms/errors vs higher quadrature and mass quadratic form | Diagnostic definitions and nontrivial error integration. |

Basix is also used by DOLFINx, so these are independent assembly routes, not
independent basis libraries. The analytical checks provide additional anchors.
Python checks validate the generated dataset; the C++ suites check reference
operations, field assembly, and complete solves against it.

Verified on 11 September 2026 in the serial Release build: all **100 operator/solve
CTest registrations** pass, together with the three reference targets (183
GoogleTests), three thin-halo runs, and 12 existing FEM/field regression suites.
The 216 distinct full solves run on both one and two MPI ranks; 24 selected 3D
solves additionally run on four ranks. Four-rank fixtures verify two split axes.
All individual registrations retain the 60-second timeout.

On 12 September, the runtime migration passed all **104 CTest registrations**
(the existing 103 reference/operator/solver runs plus the reader regression).
It preserved every original field bit-for-bit and regenerated all files and the
archive byte-identically. See the external
storage report for package sizes, loading/build measurements, and current
verification. All 104 registrations passed again on 16 September after adapting
the package, installed configuration, and release lock to `ipplTestData`.
No Python FEM stack is used by ordinary C++ builds or test execution.

The regression work exposed and repaired these production defects:

- Lower/upper global index comparisons were swapped; lower now means row > column.
  This changes Gauss–Seidel/SSOR sweep direction. Their convergence remains outside
  this CG/Jacobi acceptance scope.
- Container min/max attempted unsupported reductions on whole `DOFArray` values.
  They now reduce scalar owned DOFs through Kokkos and MPI.
- The Poisson solver used the assembled RHS for nonzero Dirichlet lifting and
  restored only BC types in callbacks. It now uses prescribed LHS coefficients and
  homogeneous Krylov constraints. Plain FEM CG gained PCG's zero-RHS stopping rule.
- Neighbor directions were inferred from overlap width, misclassifying thin parallel
  axes. `FieldLayout` now uses relative domain positions; halo exchange accepts
  these thin domains. A direct regression validates directions collectively before
  communication, then checks fill and accumulation through four ranks. Ordinary
  periodic/nonperiodic field-halo regressions are included in verification.
- `FEMContainer` discarded explicit boundary offsets/slopes and omitted them from
  copies. Setting, copying, deep copying, assigning, and restoring BC metadata now
  have a direct regression, including an explicitly prescribed zero.
- Constant boundary identity rows were multiplied at MPI interfaces during halo
  accumulation. A shared owned-boundary kernel restores each identity row once.
  Triangular helpers now use full accumulation: trimming an entity-index boundary
  discarded valid edge/face DOFs that were not physical Dirichlet DOFs. Inverse
  diagonal checks also require zero output at zero-Dirichlet rows even when input
  coefficients there are nonzero.

No expected matrix, source, or load was changed to accommodate an IPPL failure.
The boundary assignment runs over owned entity views in the configured Kokkos
execution space; it transfers no field data to the host. Existing halo exchange
and MPI reductions remain responsible for interface assembly and norms. Corrected
neighbor classification changes MPI tags/direction filtering for thin axes; the
new corner tests verify that contributions are neither lost nor duplicated. The
intended numerical change is correct enforcement of prescribed values and their
free-equation lift, without changing the raw stiffness or load discretization.

GPU execution is still unverified: this workspace has a serial Kokkos build and
no available HIP/ROCm toolchain or GPU-enabled IPPL build. Host inspection does
find AMD compute device nodes; sandbox-only device inspection was insufficient. Host/device mirrors in the test
adapter are explicit, but this does not establish whole-solver GPU portability.
Multigrid, mixed/periodic boundary validation, and P6+ numerical stress remain
outside this acceptance scope.
