# IPPL (`ippl-frk`) Architecture Reference for AI Agents

> Purpose: a dense, navigable code map of the IPPL library at **`ipplFEM/ippl-frk/`**, optimized
> for an AI coding agent. It records *verbatim* public signatures, file:line anchors,
> copy-pasteable skeletons, build/run commands, and the non-obvious gotchas that waste agent time.
>
> This is a **fork** (`ippl-frk`, branch `FEMContainer`) of [IPPL-framework/ippl](https://github.com/IPPL-framework/ippl)
> with active **FEM** development. **Nodes1D is merged** (`7d88383f`, hygiene `2dcdb2f6`).
> This tree is the **future FEMContainer development copy**; `IPPLfem/ippl-frk` is the older
> working copy. When in doubt about a signature, open the cited file:line — the code is the
> source of truth.

---

## 0. TL;DR for an agent landing here

- **What IPPL is:** a C++20, header-heavy, performance-portable library for **particle-mesh** (PIC) and
  **finite-element** simulations. Parallelism = **Kokkos** (CPU/GPU kernels) + **MPI** (domain/particle decomposition) + **heFFTe** (FFT).
- **Everything lives in `namespace ippl`.** Almost every class is a template over `<T (float/double), unsigned Dim (1..6), ...>`.
- **Headers are `.h`, template implementations are `.hpp`** (included at the bottom of the matching `.h`). Edit the `.hpp` for logic, the `.h` for the interface.
- **Build:** `ipplFEM/build` (serial Release, FFT+SOLVERS+TESTS+UNIT_TESTS). Always `cmake --build . -j 8` (user's memory limit; never bare `-j`). See [§9](#9-build--run).
- **Entry header:** `#include "Ippl.h"` pulls in the core. Every program is wrapped in `ippl::initialize(argc, argv); { ... } ippl::finalize();`.
- **The FEM subsystem ([§7](#7-fem-finite-elements)) is the fork's focus and the target of later project tasks.**

### The 3 mental models

| Model | Core types | Use when |
|-------|-----------|----------|
| **Field / mesh (Eulerian)** | `Field`, `UniformCartesian`, `FieldLayout`, `BConds` | grid data, stencils, FFT/CG solves |
| **Particles (Lagrangian)** | `ParticleBase`, `ParticleAttrib`, `ParticleSpatialLayout` | PIC bunches, scatter/gather |
| **FEM** | `LagrangeSpace` / `NedelecSpace`, `FEMPoissonSolver`, `FEMVector`, `FEMContainer` | weak-form PDE assembly + CG |

---

## Table of Contents

1. [Conventions & coding rules](#1-conventions--coding-rules)
2. [Runtime: initialize / Comm / Inform / Timings](#2-runtime-initialize--comm--inform--timings)
3. [Index & Region space](#3-index--region-space)
4. [Mesh & FieldLayout (decomposition)](#4-mesh--fieldlayout-decomposition)
5. [Fields (BareField / Field / BConds)](#5-fields-barefield--field--bconds)
6. [Particles & interpolation](#6-particles--interpolation)
7. [FEM (finite elements)](#7-fem-finite-elements)
8. [Solvers (Poisson / Maxwell / linear)](#8-solvers-poisson--maxwell--linear)
9. [Build & run](#9-build--run)
10. [Mini-app skeleton (Manager pattern)](#10-mini-app-skeleton-manager-pattern)
11. [Gotchas & agent footguns](#11-gotchas--agent-footguns)
12. [File index](#12-file-index)

---

## 1. Conventions & coding rules

Source: `WORKFLOW.md`, `.clang-format`.

- **Naming:** variables `camelCase`; compile-time constants `CAPITAL`; **member variables suffixed `_m`** (e.g. `mesh_m`, `rhs_mp`).
- **Files:** interface in `Foo.h`, template impl in `Foo.hpp` (included at the end of `Foo.h`). Non-template `.cpp` only for `Utility/`, `Ippl.cpp`, etc.
- **Math portability (critical for GPU):**
  - Host-only code → `std::sqrt`, `std::sin`, ...
  - Device code or anything marked `KOKKOS_INLINE_FUNCTION` / `KOKKOS_FUNCTION` → **`Kokkos::sqrt`, `Kokkos::pow`, `Kokkos::abs`**, constants from **`Kokkos::numbers`**.
- **Kernels:** use IPPL wrappers `ippl::parallel_for` / `ippl::parallel_reduce` (`Utility/ParallelDispatch.h`), not raw `Kokkos::parallel_for`, when iterating fields — they pass an `ippl::Vector<index_type,Dim>` index bundle and integrate with range policies.
- **Formatting:** `clang-format` enforced via a git pre-commit hook. Install once: `hooks/create-hook-symlink.sh`. The hook reformats *staged* files. Use `--no-verify` only when partial-staging.
- **Tests required:** any non-bugfix contribution upstream needs a unit test (`UNIT_TESTS.md`). Tests are instantiated across **precision × dimension (1..6) × execution space** combinations.

---

## 2. Runtime: initialize / Comm / Inform / Timings

Globals (inline `std::unique_ptr`s) declared in `src/Ippl.h:20-31`, created in `src/Ippl.cpp`:

```cpp
namespace ippl {
    inline std::unique_ptr<mpi::Communicator> Comm;   // MPI wrapper (rank/size/reduce/...)
    inline std::unique_ptr<mpi::Environment>  Env;
    inline std::unique_ptr<Inform> Info, Warn, Error; // ranked logging
}
```

**Lifecycle (always wrap your program like this):**

```cpp
#include "Ippl.h"
int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);          // boots MPI env + Comm + Inform + Kokkos, parses CLI
    {
        Inform msg("MyApp");               // rank-0 only logging; terminate lines with endl
        // ... all simulation code in this scope so destructors run before finalize ...
    }
    ippl::finalize();                       // Comm->deleteAllBuffers(); Kokkos::finalize();
    return 0;
}
```

- `void ippl::initialize(int& argc, char* argv[], MPI_Comm = MPI_COMM_WORLD)` — `Ippl.h:33`.
- **CLI flags parsed by `initialize`:** `--info/-i <1..5>`, `--overallocate/-b <f>`, `--timer-fences on|off`, `--version/-v`, `--debug/-g`, `--help/-h`. Positional args are your app's.
- **`Comm` usage:** `ippl::Comm->rank()`, `->size()`, `->barrier()`, `->reduce(...)`, `->allreduce(...)`, `->abort()`. (`src/Communicate/Communicator.h`)
- **Logging:** `Inform msg(name);` (rank 0) or `Inform msg(name, INFORM_ALL_NODES);` (all ranks). Stream with `<<`, flush with `endl`. (`src/Utility/Inform.h`)
- **Profiling:**
  ```cpp
  static IpplTimings::TimerRef t = IpplTimings::getTimer("label");
  IpplTimings::startTimer(t); /* ... */ IpplTimings::stopTimer(t);
  IpplTimings::print();                       // MPI-reduced table on rank 0
  IpplTimings::print(std::string("timing.dat"));
  ```
  (`src/Utility/IpplTimings.h`)
- **Solver/config params:** `ippl::ParameterList` (`Utility/ParameterList.h`): `params.add("tolerance", 1e-13); solver.mergeParameters(params);`

---

## 3. Index & Region space

Two parallel concepts: **integer index space** (mesh/field) and **continuous region space** (particles/physical coordinates).

### Index (integer, inclusive ranges) — `src/Index/`

- `ippl::Index` (`Index.h:40`): a 1D **inclusive** strided range `[first..last]`.
  - `Index(n)` → `[0..n-1]`; `Index(f,l)` → `[f..l]`; `Index(f,l,s)` strided.
  - `min()/max()/length()/stride()/first()/last()`; set ops `intersect`, `grow(n)`, `touches`, `contains`.
- `ippl::NDIndex<Dim>` (`NDIndex.h:21`): `Dim` `Index`es = the global index domain.
  - `NDIndex(I0, I1, ...)` (Dim `Index` args) or `NDIndex(Vector<unsigned,Dim> sizes)` → `[0..size-1]` per axis.
  - `operator[](d)`, `size()` (product), `intersect`, `grow(ncells)`, `length()`, `first()`, `last()`.

> Mesh convention: `NDIndex` built from **vertex counts** (`nodesPerDim`). `numCells = nodes - 1`.

### Region (continuous, half-open) — `src/Region/`

- `ippl::PRegion<T>` = `[a, b)`; `ippl::NDRegion<T,Dim>` = `Dim` of them.
- `detail::RegionLayout<T,Dim,Mesh>` (`RegionLayout.h:36`): converts a `FieldLayout`+`Mesh` into per-rank physical `NDRegion`s; used to locate particles. Built from `FieldLayout` + `Mesh`.

---

## 4. Mesh & FieldLayout (decomposition)

### `UniformCartesian<T, Dim>` — `src/Meshes/UniformCartesian.h:14`

Uniform rectilinear grid (the default/only mesh in practice). `DefaultCentering = Cell`.

```cpp
UniformCartesian(const NDIndex<Dim>& ndi, const vector_type& hx, const vector_type& origin);
void initialize(const NDIndex<Dim>& ndi, const vector_type& hx, const vector_type& origin);
KOKKOS_INLINE_FUNCTION const vector_type& getMeshSpacing() const;   // hx
KOKKOS_INLINE_FUNCTION T getCellVolume() const;  T getMeshVolume() const;
KOKKOS_INLINE_FUNCTION vector_type getVertexPosition(const NDIndex<Dim>&) const; // origin + ndi.first()*hx
vector_type getOrigin() const;  void setOrigin(const vector_type&);
```

Base class `Mesh<T,Dim>` (`Meshes/Mesh.h`) defines `vector_type = Vector<T,Dim>`, `matrix_type`.
Centering tags in `Meshes/Centering.h`: `Cell`, `Vert`.

### `FieldLayout<Dim>` — `src/FieldLayout/FieldLayout.h:166`

Describes how the global `NDIndex<Dim>` is split across MPI ranks.

```cpp
FieldLayout(mpi::Communicator, const NDIndex<Dim>& domain,
            std::array<bool,Dim> isParallel, bool isAllPeriodic = false);
void initialize(const NDIndex<Dim>&, std::array<bool,Dim>, bool isAllPeriodic = false);

const NDIndex<Dim>& getDomain() const;                 // global
const NDIndex<Dim>& getLocalNDIndex() const;           // this rank's owned cells
std::array<bool,Dim> isParallel() const;
void updateLayout(const std::vector<NDIndex<Dim>>&);   // used by load balancer
```

- `isParallel[d] == true` ⇒ dimension `d` is partitioned across ranks; `false` ⇒ kept whole on every rank.
- Throws if MPI ranks > partitionable cells (`FieldLayout.hpp:97-99`).
- `SubFieldLayout<Dim>` (`FieldLayout/SubFieldLayout.h:39`): a layout restricted to a sub-domain (used by FEM `FEMContainer` entity fields).

---

## 5. Fields (BareField / Field / BConds)

### `BareField<T, Dim, ViewArgs...>` — `src/Field/BareField.h:40`

Kokkos-View-backed distributed array (no mesh, no BC). Participates in expression templates.

```cpp
using view_type = detail::ViewType<T, Dim, ViewArgs...>::view_type;  // e.g. Kokkos::View<double***>
BareField(Layout_t& layout, int nghost = 1);
void initialize(Layout_t&, int nghost = 1);
void updateLayout(Layout_t&, int nghost = 1);

view_type& getView();                  // mutable raw Kokkos view (write here)
const view_type& getView() const;
host_mirror_type getHostMirror() const;
const Domain_t& getOwned() const;      int getNghost() const;  Layout_t& getLayout() const;

// Halo exchange (MPI + periodic):
void fillHalo();                       // owned -> ghost
void accumulateHalo();                 // ghost -> owned (sum; used in FEM assembly & scatter)

// Assignment / expressions:
BareField& operator=(T x);
template <typename E, size_t N> BareField& operator=(const detail::Expression<E,N>&);
template <class... Args> KOKKOS_INLINE_FUNCTION T operator()(Args...) const; // READ-ONLY

// Reductions (MPI-reduced): sum(), max(), min(), prod()
// Iteration policy excluding ghosts:
template <class... P> auto getFieldRangePolicy(int nghost = 0) const;
```

> **View index space is local + ghost, not global.** View index `[nghost, nghost+localLen)` = owned cells; `[0,nghost)` and the tail are ghost layers. To recover a global index, add `field.getOwned()[d].first()` and subtract `nghost` (see the FEM RHS-assignment kernel in `test/solver/fem/TestNonhomDirichlet_2d.cpp:92-102`).

### `Field<T, Dim, Mesh, Centering, ViewArgs...>` — `src/Field/Field.h:18`

`BareField` + a `Mesh` pointer + a `BConds` container. **This is what solvers and most code use.**

```cpp
Field(Mesh_t& mesh, Layout_t& layout, int nghost = 1);
void initialize(Mesh_t&, Layout_t&, int nghost = 1);
KOKKOS_INLINE_FUNCTION Mesh_t& get_mesh() const;
void setFieldBC(BConds_t& bc);   BConds_t& getFieldBC();
T getVolumeIntegral() const;     T getVolumeAverage() const;
```

Common alias set:
```cpp
using Mesh_t   = ippl::UniformCartesian<double, Dim>;
using Field_t  = ippl::Field<double, Dim, Mesh_t, Cell>;
using BConds_t = ippl::BConds<Field_t, Dim>;
```

### Differential operators — `src/Field/FieldOperations.hpp`

`grad(u)`, `div(u)`, `laplace(u)`, `curl(u)` (3D), `hess(u)` — return expression nodes; they call `fillHalo()` + BC `apply()` first. Assign into another field: `e = laplace(u);`.

### Boundary conditions — `src/Field/BConds.h`, `BcTypes.h`

`BConds<Field,Dim>` is a `std::array<shared_ptr<BCondBase>, 2*Dim>` — one face per `d` (axis `face/2`, upper if `face&1`).

```cpp
BConds_t bc;
for (unsigned i = 0; i < 2*Dim; ++i)
    bc[i] = std::make_shared<ippl::ZeroFace<Field_t>>(i);       // Dirichlet 0
    // or ippl::ConstantFace<Field_t>(i, value)  (Dirichlet const)
    // or ippl::PeriodicFace<Field_t>(i)
    // or ippl::ExtrapolateFace<Field_t>(i, offset, slope)  / NoBcFace (default)
field.setFieldBC(bc);
bc.apply(field);
```

BC enum `FieldBC` (`BcTypes.h`): `PERIODIC_FACE, CONSTANT_FACE, ZERO_FACE, EXTRAPOLATE_FACE, NO_FACE`. Only **cell-centered** BCs are implemented.

### Vectors & expression templates

- `ippl::Vector<T, Dim>` (`Types/Vector.h:22`): small fixed-size vector, itself an expression. `dot(a,b)`, `cross(a,b)`, `norm(v)`, `[]`. Kokkos-friendly (`KOKKOS_FUNCTION` ctors).
- Field arithmetic is lazy: `a = 2.0*b + sin(c) - d;` builds an expression tree evaluated in one fused Kokkos kernel on assignment (`Expression/`, `BareField.hpp:219-226`).

### Reading/writing field data (the canonical kernel)

```cpp
auto view  = field.getView();                                   // mutable
auto ldom  = layout.getLocalNDIndex();
using idx_t = typename ippl::RangePolicy<Dim>::index_array_type; // ippl::Vector<int,Dim>
ippl::parallel_for("set", field.getFieldRangePolicy(),
    KOKKOS_LAMBDA(const idx_t& args) {
        // args are view-local indices (include ghost offset)
        apply(view, args) = /* value */;   // apply() indexes a view by an index bundle
    });
```

---

## 6. Particles & interpolation

### `ParticleBase<PLayout, IDProperties...>` — `src/Particle/ParticleBase.h:86`

Base for a particle bunch. Owns positions `R` and optional `ID`. **Derive from it** and register attributes:

```cpp
using PLayout_t  = ippl::ParticleSpatialLayout<double, Dim>;   // Mesh defaults to UniformCartesian
struct Bunch : public ippl::ParticleBase<PLayout_t> {
    ippl::ParticleAttrib<double>                 q;   // scalar attribute
    ippl::ParticleAttrib<ippl::Vector<double,Dim>> P; // vector attribute (velocity)
    Bunch(PLayout_t& pl) : ippl::ParticleBase<PLayout_t>(pl) { addAttribute(q); addAttribute(P); }
};
```

Key API:
```cpp
size_type getLocalNum() const;  size_type getTotalNum() const;
void create(size_type nLocal);          // COLLECTIVE: add particles
void globalCreate(size_type nTotal);    // COLLECTIVE: distribute evenly
void destroy(const Kokkos::View<bool*>& invalid, size_type n);  // COLLECTIVE
void setParticleBC(BC);                 // ippl::BC: PERIODIC | REFLECTIVE | SINK | NO
void update();                          // COLLECTIVE: apply BC + migrate particles across ranks
Layout_t& getLayout();
```

`R` and attributes are `ParticleAttrib<T>` (`Particle/ParticleAttrib.h`): Kokkos-view-backed, expression-capable, with MPI reductions `sum/max/min/prod` and `getView()`.

### `ParticleSpatialLayout<T, Dim, Mesh, ...>` — `src/Particle/ParticleSpatialLayout.h:44`

```cpp
ParticleSpatialLayout(FieldLayout<Dim>& fl, Mesh& mesh, bool fem = false);
template <class PC> void update(PC& pc);          // BC -> locate -> MPI migrate -> destroy/recv
void updateLayout(FieldLayout<Dim>&, Mesh&);
const RegionLayout_t& getRegionLayout() const;
```

### Scatter / gather (CIC) — `src/Interpolation/`

```cpp
// deposit particle attribute onto a field (then accumulateHalo internally):
scatter(bunch.q, rhoField, bunch.R);
// sample field at particle positions (fillHalo first internally):
gather(bunch.E, EField, bunch.R);   // overload: addToAttribute=false (overwrite) / true (+=)
```

EM current deposition (Yee-staggered): `assemble_current_yee(mesh, q, R0, R1, Jfield, policy, dt)` (`CurrentDeposition.hpp:18`).

### Load balancing — `src/Decomposition/OrthogonalRecursiveBisection.h`

```cpp
orb.initialize(fl, mesh, weightField);
bool ok = orb.binaryRepartition(bunch.R, fl, isFirstRepartition);  // CIC-weighted ORB
if (ok) { /* updateLayout(fl, mesh) on fields + bunch.update() */ }
```

### Canonical PIC step (see `test/particle/PICnd.cpp`)

```
R = R + dt*P;            // drift
bunch.update();          // migrate across ranks
scatter(q, rho, R);      // deposit
solver.solve();          // field solve (Poisson/Maxwell)
gather(E, Efield, R);    // interpolate force
P = P + dt*(q/m)*E;      // kick
```

---

## 7. FEM (finite elements)

This is the fork's focus and the target of later project tasks. FEM lives in `src/FEM/`, solvers in `src/PoissonSolvers/FEMPoissonSolver.h` & `PreconditionedFEMPoissonSolver.h`. FEM tests: `test/solver/fem/`, `unit_tests/FEM/`.

### 7.1 The layered design

```
Reference Element (geometry)         Quadrature (integration; calls Nodes1D)
  EdgeElement [0,1]                    GaussLegendreQuadrature(refEl)   // degree 2N-1, [-1,1]
  QuadrilateralElement [0,1]^2         GaussLobattoQuadrature(refEl)    // degree 2N-3, N>=2
  HexahedralElement [0,1]^3            MidpointQuadrature(refEl)        // degree 1, [0,1]
            \                          SelectableQuadrature(refEl, fam) // Track B runtime GL/GLL/mid
             v                        /
        FiniteElementSpace<T,Dim,NumElemDOFs,ElementType,QuadratureType,FieldLHS,FieldRHS>  (base)
             |                          |                         |
        LagrangeSpace            LagrangeSpace_wfc           NedelecSpace
        (P1, vertex Field)       (P>=1, FEMContainer)        (edge DOFs, FEMVector)
             |
        FEMPoissonSolver / PreconditionedFEMPoissonSolver  (wraps CG/PCG)
```

1D root finding is **not** in this layer: `src/Nodes1D/` (`ippl::nodes1d`) on native `[-1,1]`.
FEM quadrature is a tensor-product adapter onto the reference element `[0,1]^d`.

### 7.2 Reference elements — `src/FEM/Elements/`

`Element<T,Dim,NumVertices>` (`Element.h:16`) is metadata-only (no virtuals — Kokkos device dispatch avoided on purpose). Concrete elements, all on the unit reference domain with **diagonal** (axis-aligned) Jacobians:

| Class | Dim | Vertices | Ref domain |
|-------|-----|----------|-----------|
| `EdgeElement<T>` | 1 | 2 | `[0,1]` |
| `QuadrilateralElement<T>` | 2 | 4 | `[0,1]^2` |
| `HexahedralElement<T>` | 3 | 8 | `[0,1]^3` |

Key `KOKKOS_FUNCTION` methods (each element): `getLocalVertices()`, `getTransformationJacobian(globalVerts)`, `getInverseTransposeTransformationJacobian(globalVerts)`, `getDeterminantOfTransformationJacobian(globalVerts)`, `localToGlobal`, `globalToLocal`, `isPointInRefElement`.

### 7.3 Quadrature — `src/FEM/Quadrature/` (calls `src/Nodes1D/`)

`Quadrature<T, NumNodes1D, ElementType>` (`Quadrature.h:37`). Tensor product ⇒ `numElementNodes = NumNodes1D^Dim`.
`static_assert(NumNodes1D >= 1)` and `dim >= 1`. Base ctor initializes degree and native interval:

```cpp
Quadrature(const ElementType& ref_element, unsigned degree, T a, T b);  // Quadrature.h:60
virtual ~Quadrature() = default;
Vector<T,numElementNodes> getWeightsForRefElement() const;
Vector<Vector<T,dim>,numElementNodes> getIntegrationNodesForRefElement() const;
```

Affine remap in `Quadrature.hpp` delegates to `nodes1d::affineMapPoint` / `affineMapWeight`.

**Unweighted FEM rules** (standalone `Quadrature` subclasses; ctor is `(const ElementType& ref)`):

| Class | Native interval | Degree | Nodes1D call | Constraint |
|-------|-----------------|--------|--------------|------------|
| `GaussLegendreQuadrature` | `[-1,1]` | \(2N-1\) | `computeGaussLegendre` | `N >= 1` |
| `GaussLobattoQuadrature` | `[-1,1]` | \(2N-3\) | `computeGaussLobatto` | `N >= 2` |
| `MidpointQuadrature` | `[0,1]` | 1 | none (uniform midpoints) | `N >= 1` |

Weighted Gauss–Jacobi / Gauss–Chebyshev FEM classes are **commented out** in
`GaussLegendreQuadrature.h` (history: that file was `GaussJacobiQuadrature.h`). Those rules
integrate \(w(x)\,f(x)\) (Chebyshev weights sum to \(\pi\), not 2) and would silently corrupt
unweighted stiffness/load. The 1D math remains in `ippl::nodes1d`.

**Track B selectable rule (working tree, Task 7):** `SelectableQuadrature`
(`FEM/Quadrature/SelectableQuadrature.h`) owns `QuadratureNodeFamily` + parsers and switches
at runtime among GL / GLL / midpoint for a fixed `NumNodes1D`. Base ctor starts as GL;
`setFamily` overwrites `degree_m`/`a_m`/`b_m`. GLL with `NumNodes1D < 2` throws at runtime
(`if constexpr`) so N=1 GL/midpoint still compile. Driven by ParameterList `quadrature_nodes`
on `FEMPoissonSolver_wFEMContainer` (default `gauss_legendre`). This class reimplements the
three 1D rules rather than composing the standalone types — keep while the solver is
runtime-selectable.

**1D Lagrange interpolation nodes (working tree, Task 7)** (`FEM/InterpolationNodeFamilies.h`):
`LagrangeNodeFamily` {Equispaced, GLL} + ParameterList parsers. `LagrangeDOFLocations::setFromFamily`
fills 1D nodes on `[0,1]` (equispaced `i/(N-1)`, or GLL via Nodes1D + affine map + endpoint snap)
and rebuilds the tensor product. `LagrangeSpace_wfc` ParameterList key `interpolation_nodes`
(default `gll`). Mass-shortcut helper is **commented out** (Poisson is stiffness-only).

**1D root finding** lives in freestanding `src/Nodes1D/` (`ippl::nodes1d`): Gauss–Jacobi /
Legendre / Chebyshev / GLL on `[-1,1]` + affine map. Host-only setup; View overloads
deep-copy. `RootFinderMethod`: `GolubWelsch` (default, **tridiagonal** O(n²)),
`DenseGolubWelsch` (full-matrix O(n³) reference), `Newton` (staged asymptotic/Chebyshev +
Brent; throws on failure). GLL GW uses Jacobi(1,1) interiors (`n−2`). Oracles: edit
`NS`/`AFFINE_NS` in `unit_tests/Nodes1D/create_Nodes1DOracleData.py` and regenerate
`Nodes1DOracleData.h`.

**Tests:** `unit_tests/Nodes1D/`; `unit_tests/FEM/Quadrature/{Quadrature,GaussLegendreQuadrature,GaussLobattoQuadrature}.cpp`; `unit_tests/FEM/RefShapeFunctionData.cpp`.
Do **not** add `NO_MPI` to IPPL-linked tests (CSCS MPI loader). NUFFT `Correction.h` uses the
Kokkos View `computeGaussLegendre` overload.

### 7.4 `LagrangeSpace<T, Dim, Order, ElementType, QuadratureType, FieldLHS, FieldRHS>` — `src/FEM/LagrangeSpace.h:33`

The workhorse FEM space (vertex-centered, classic path; P1 fully supported). DOFs per element = `(Order+1)^Dim`.

```cpp
LagrangeSpace(UniformCartesian<T,Dim>& mesh, ElementType& refEl,
              const QuadratureType& quad, Layout_t& layout);
void initialize(UniformCartesian<T,Dim>&, Layout_t&);

// shape functions on the reference element:
KOKKOS_FUNCTION T       evaluateRefElementShapeFunction(localDOF, const point_t& xi) const;
KOKKOS_FUNCTION point_t evaluateRefElementShapeFunctionGradient(localDOF, const point_t& xi) const;

// matrix-free assembly (A*x) given a bilinear-form functor F:
template <typename F> FieldLHS evaluateAx(FieldLHS& field, F& evalFunction);
template <typename F> FieldLHS evaluateAx_lift(FieldLHS& field, F& evalFunction); // Dirichlet lifting
void evaluateLoadVector(FieldRHS& field) const;                                   // RHS b

// diagnostics:
template <typename F> T computeErrorL2(const FieldLHS& u_h, const F& u_sol) const;
T computeAvg(const FieldLHS& u_h) const;
DeviceStruct getDeviceMirror() const;   // for particle-interpolation kernels
```

### 7.5 The assembly contract: `EvalFunctor` → `evaluateAx`

The bilinear form is a small functor passed into `evaluateAx`. For Poisson (`PoissonSolvers/EvalFunctor.h:15`):

```cpp
template <typename Tlhs, unsigned Dim, unsigned numElemDOFs>
struct EvalFunctor {
    const Vector<Tlhs,Dim> DPhiInvT;    // inverse-transpose Jacobian (constant on uniform mesh)
    const Tlhs absDetDPhi;              // |det J|
    KOKKOS_FUNCTION auto operator()(const size_t& i, const size_t& j,
        const RefShapeFunctionData<Tlhs, Vector<Tlhs,Dim>, numElemDOFs>& qd) const {
        return dot((DPhiInvT * qd.deriv_q[j]), (DPhiInvT * qd.deriv_q[i])).apply() * absDetDPhi;
    }   // == integrand of  ∫ ∇φ_i·∇φ_j
};
```

`evaluateAx` (`LagrangeSpace.hpp:371+`) precomputes one element stiffness matrix
`A_K[i][j] = Σ_k w[k]*F(i,j,qd)` (identical for all elements on a uniform mesh), then loops
elements applying `y_I += A_K[i][j]*x_J` with BC handling + `accumulateHalo()`. The functor
receives a `RefShapeFunctionData<TVal,TDeriv,numElemDOFs>` (`FEM/RefShapeFunctionData.h:28`;
holds `.val_q[i]` and `.deriv_q[i]`) — **not** a bare gradient vector and **not** the old
`QuadratureData` / `FEMQuadratureData` names. **To implement a different PDE, write a new
functor with `operator()(size_t i, size_t j, const RefShapeFunctionData<...>& qd)`** — no
change to assembly machinery.

### 7.6 `FEMPoissonSolver<FieldLHS, FieldRHS=FieldLHS, Order=1, QuadNumNodes=5>` — `src/PoissonSolvers/FEMPoissonSolver.h:21`

Solves `-laplace(lhs) = rhs` via matrix-free PCG. Auto-selects element by `Dim` (Edge/Quad/Hex).

```cpp
FEMPoissonSolver(lhs_type& lhs, rhs_type& rhs);
void setRhs(rhs_type& rhs) override;
void solve() override;                  // builds load vector, lifts Dirichlet, runs PCG
int  getIterationCount();   Tlhs getResidue() const;
template <typename F> Tlhs getL2Error(const F& analytic);   // analytic = struct w/ KOKKOS operator()
LagrangeType& getSpace();
```
Default params: `max_iterations=1000`, `tolerance=1e-13`. `PreconditionedFEMPoissonSolver` adds Jacobi/Chebyshev/Richardson/Gauss-Seidel/SSOR preconditioners (params `preconditioner_type`, `newton_level`, `chebyshev_degree`, ...).

**Minimal FEM Poisson usage** (condensed from `test/solver/fem/TestNonhomDirichlet_2d.cpp`):

```cpp
Mesh_t mesh(domain, cellSpacing, origin);
ippl::FieldLayout<Dim> layout(MPI_COMM_WORLD, domain, isParallel);
Field_t lhs(mesh, layout, 1), rhs(mesh, layout, 1);
BConds_t bc; for (unsigned i=0;i<2*Dim;++i) bc[i]=std::make_shared<ippl::ConstantFace<Field_t>>(i,1.56);
lhs.setFieldBC(bc); rhs.setFieldBC(bc); bc.apply(lhs); bc.apply(rhs);
// ... fill rhs.getView() with f(x) via parallel_for ...
ippl::FEMPoissonSolver<Field_t,Field_t> solver(lhs, rhs);
ippl::ParameterList p; p.add("tolerance",1e-13); p.add("max_iterations",2000); solver.mergeParameters(p);
solver.solve();
double err = solver.getL2Error(AnalyticSol<double>{});
```

### 7.7 Fork-specific FEM

There are **two independent fork tracks**. Don't confuse them:

- **Track A — standalone `FEMVector` + `NedelecSpace` (working, first order).** Self-contained:
  `NedelecSpace` builds directly on `FEMVector<T>` and does its own DOF indexing, halo, BC, and assembly.
  It does **not** touch `DOFHandler`/`FiniteElementSpaceTraits`/`FEMContainer`. This is the curl-conforming
  analogue of the working first-order classic `LagrangeSpace` path. Unit tests: `unit_tests/FEM/FEMVector.cpp`,
  `unit_tests/FEM/NedelecSpace.cpp`.
- **Track B — `DOFHandler` + `FEMContainer` + `LagrangeSpace_wFEMContainer` (the generalized DOF machinery, currently under debugging).**
  This is the entity-split / higher-order generalization and is the part that **mainly needs debugging right now**.

| Component | File | What it adds | Track / status |
|-----------|------|--------------|----------------|
| `FEMVector<T>` | `FEM/FEMVector.h` | flat 1D MPI-aware DOF vector (`fillHalo`/`accumulateHalo`, `innerProduct`, `norm`, expression `operator=`); used when DOFs aren't vertex-only | **A — working** |
| `NedelecSpace<T,Dim,Order,Element,Quad,FieldType=FEMVector<T>>` | `FEM/NedelecSpace.h` | lowest-order **curl-conforming edge elements** (2D:4, 3D:12 DOFs/elem) over `FEMVector`; own `getFEMVectorDOFIndices`, `evaluateAx`, `evaluateLoadVector[Functor]`, `reconstructToPoints`, `isDOFOnBoundary`/`getBoundarySide` | **A — working (1st order)** |
| `GridPathSegmenter` | `FEM/GridPathSegmenter.h` | split a particle trajectory into `Dim+1` per-cell sub-segments | A — working |
| `ProjectCurrent.hpp` | `FEM/ProjectCurrent.hpp` | `assemble_current_nedelec(...)` — deposit current onto Nédélec edge DOFs | A — working |
| `FEMInterpolate.hpp` | `FEM/FEMInterpolate.hpp` | P1 particle deposit/sample/gradient (`locate_element_nd_and_xi`, `assemble_rhs_from_particles`, `interpolate_to_diracs`) | working (classic Lagrange) |
| `DOFHandler<T,SpaceTraits>` | `FEM/DOFHandler.h` | local-DOF → (entity, offset, local-DOF) mapping | **B — debugging** (`fillNedelecDOFMappingTable` is a TODO stub belonging to a *future* DOFHandler-based Nédélec path, unrelated to Track A) |
| `FEMContainer<T,Dim,EntityTypes,DOFNums>` | `FEM/FEMContainer.h` | entity-split multi-field storage (vertices/edges/faces/volume), each a `Field<DOFArray<T,N>,...>` over a `SubFieldLayout` | **B — debugging** |
| `LagrangeSpace_wfc` | `FEM/LagrangeSpace_wFEMContainer.h` | Lagrange space backed by `FEMContainer`+`DOFHandler` for **P≥2** / entity-split DOFs | **B — debugging** (periodic BC TODO; some DOF methods marked UNUSED) |
| `FiniteElementSpaceTraits` | `FEM/FiniteElementSpaceTraits.h` | compile-time entity/DOF metadata per space | B — `LagrangeSpaceTag` defined; `NedelecSpaceTag` has no specialization (only relevant to the *future* Track-B Nédélec, not to the working Track-A `NedelecSpace`) |
| `Entity.h`, `DOFArray.h`, `DOFLocations.h`, `FEMHelperStructs.h` | `FEM/` | entity tags (`Vertex`/`EdgeX`/`FaceXY`/...), per-cell DOF arrays, ref-element DOF coords, tuple metaprogramming | B — support |

> **Agent caution:** the part actively being debugged is **Track B** (`DOFHandler`, `FEMContainer`,
> `LagrangeSpace_wFEMContainer` and their traits/DOF-location support). Treat Track A (`FEMVector`,
> `NedelecSpace`) as working at first order, on par with the classic first-order `LagrangeSpace`.
> Always confirm any path against its matching `unit_tests/FEM/` test (e.g. `FEMContainer.cpp`,
> `FEMContainerHalo.cpp`, `LagrangeSpace_wFEMContainer.cpp`, `DOFHandler.cpp` for Track B;
> `NedelecSpace.cpp`, `FEMVector.cpp` for Track A). Both classic `LagrangeSpace` (in `IpplCore.h`) and
> `LagrangeSpace_wfc` coexist.

---

### 7.8 `FEMPoissonSolver_wFEMContainer` — Track B Poisson solver — `src/PoissonSolvers/FEMPoissonSolver_wFEMContainer.h`

Track B analogue of `FEMPoissonSolver` / `PreconditionedFEMPoissonSolver`; uses `LagrangeSpace_wfc` +
`FEMContainer` instead of classic `LagrangeSpace` + `Field`. Drives plain `ippl::CG<>` or
`ippl::PCG<FEMContainer>` + `Preconditioner.h`.

```cpp
FEMPoissonSolver_wFEMContainer(lhs_type& lhs, rhs_type& rhs);
void solve() override;   // evaluateLoadVector → evaluateAx_lift (Dirichlet) → CG or PCG
int  getIterationCount() const;   Tlhs getResidue() const;
template <typename F> Tlhs getL2Error(const F& analytic);
LagrangeType& getSpace();
```

Default params: `max_iterations=1000`, `tolerance=1e-13`, `preconditioned=true`,
`preconditioner_type="jacobi"`, plus GS/SSOR/Richardson/Chebyshev/Newton knobs.
Track B solver uses `SelectableQuadrature` (working tree). ParameterList keys
`interpolation_nodes="gll"|"equispaced"` and
`quadrature_nodes="gauss_legendre"|"gll"|"midpoint"` are applied in `solve()` via
`applyNodeFamilyParameters()`. Convergence CLI `--interpolation_nodes` /
`--quadrature_nodes` is wired on
`test/solver/fem_wContainer/TestFEMPoissonSolver*_zeroDirichlet_convergence.cpp`
(default interp `gll`; `.dat` named `convergence_FEMPoissonSolver{1,2,3}D_{gll|equispaced}.dat`).
Mass-shortcut (`usesGllMassShortcut`) is commented out — Poisson has no mass term.

Track B also accepts `poisson_stiffness_mode`: `standard` (default),
`rowsum_diagonal`, or `constant_preserving`. `EvalFunctor` explicitly opts into
the shared `LagrangeSpace_wfc::assembleElementMatrix` policy; generic mass forms
remain ordinary integrals. Corrected modes reconstruct each complete physical
element diagonal before BC restriction. The constant-preserving full action
uses `K_ij*(x_j-x_i)`; Dirichlet columns contribute `K_ij*(0-x_i)` and must not
be skipped. Lifting and L/U keep ordinary off-diagonal products; D/inverse-D use
the corrected assembled diagonal. Corrected solver modes currently reject
periodic boundaries and multigrid. `getStiffnessMode()` lets diagnostics use the
same evaluator policy. See the manual 1D driver and `FEMConstantPreservation`.
The manual 1D driver's optional `--solver-timings 1` writes `solver_timings.csv`
on one MPI rank: existing `cg` timer samples include preconditioner setup and
applications; a separate timer measures the full `solve()` call. Both exclude
source sampling and error evaluation. Performance studies use the serial backend.

Supported types: `jacobi`, `newton`, `chebyshev`, `richardson`, `richardson_alt`, `gauss-seidel`,
`ssor`, `multigrid`. Newton/Chebyshev estimate `alpha`/`beta` via `powermethod` / `adapted_powermethod`.
`multigrid`: **P1** uses geometric h-MG (`fem_multigrid_preconditioner`). **P2/P3** use
**p-multigrid** (`fem_pmultigrid_preconditioner` + `FEMOrderTransfer`): same-mesh order coarsening
to P1, then the P1 h-MG bottom solver. Design/plan: workspace
`ProjectManagement/TASK5_Preconditioners_PLAN.md` (§5.2–5.3 + Appendix A).

**Minimal Track B usage** (condensed from `test/solver/fem_wContainer/TestFEMPoissonSolver2D_zeroDirichlet_convergence.cpp`):

```cpp
using DOFHandler_t = ippl::DOFHandler<T, ippl::FiniteElementSpaceTraits<ippl::LagrangeSpaceTag, Dim, Order>>;
using Field_t = typename DOFHandler_t::FEMContainer_t;
Field_t lhs(mesh, layout, nghost), rhs(mesh, layout, nghost);
std::array<ippl::FieldBC, 2*Dim> bc; bc.fill(ippl::ZERO_FACE);
lhs.setFieldBC(bc); rhs.setFieldBC(bc);
// ... fill rhs via assignSourceToField<T,Dim,Order,Src>(..., family) ...
ippl::FEMPoissonSolver_wFEMContainer<Field_t, Field_t, Order, QuadNodes> solver(lhs, rhs);
ippl::ParameterList p;
p.add("tolerance", 1e-13);
p.add("max_iterations", 20000);
p.add("preconditioned", true);
p.add("preconditioner_type", "jacobi");
solver.mergeParameters(p);
solver.solve();
```

## 8. Solvers (Poisson / Maxwell / linear)

Require `-DIPPL_ENABLE_SOLVERS=ON` (already on in `build_serial`).

### Poisson — `src/PoissonSolvers/`
Base `Poisson<FieldLHS,FieldRHS>` (`Poisson.h`): `-laplace(lhs)=rhs`; `setLhs/setRhs`, `solve()=0`, `OutputType` bitmask `SOL|GRAD|SOL_AND_GRAD`, `getHessian()`.
- `FFTPeriodicPoissonSolver` — periodic, R2C FFT.
- `FFTOpenPoissonSolver` — open BC; `algorithm` ∈ `HOCKNEY|VICO|BIHARMONIC|DCT_VICO`.
- `FFTTruncatedGreenPeriodicPoissonSolver` — truncated Green's function (`alpha`, `force_constant`).
- `PoissonCG` — matrix-free CG/PCG on the FD Laplacian.
- `FEMPoissonSolver` / `PreconditionedFEMPoissonSolver` — see [§7.6](#76-fempoissonsolverfieldlhs-fieldrhsfieldlhs-order1-quadnumnodes5--srcpoissonsolversfempoissonsolverh21).
- `FEMPoissonSolver_wFEMContainer` — Track B solver using `LagrangeSpace_wfc`+`FEMContainer`; see [§7.8](#78-fempoissonsolver_wfemcontainer--track-b-poisson-solver--srcpoissonsolversfempoissonsolver_wfemcontainerh). Plain CG or PCG + `Preconditioner.h` (default `jacobi`; multigrid = P1 h-MG + P2/P3 p-MG).

### Maxwell — `src/MaxwellSolvers/`
`Maxwell<EMField,SourceField>` base. `FDTDSolverBase` → `StandardFDTDSolver` / `NonStandardFDTDSolver`, `fdtd_bc ∈ {periodic, absorbing}`. `solve()` per step; deposit current with `assemble_current_yee` first.

### Linear — `src/LinearSolvers/`
`CG<...>` and `PCG<...>` in `PCG.h` (matrix-free; `setOperator(std::function<lhs(lhs)>)`, `operator()(lhs,rhs,params)`, `getIterationCount()`, `getResidue()`). Preconditioners in `Preconditioner.h`: `jacobi, newton, chebyshev, richardson, gauss-seidel, ssor`.

---

## 9. Build & run

**C++20, CMake ≥ 3.24. Deps:** MPI (required), Kokkos (default 4.7.01, fetched if absent), heFFTe (2.4.x, only if FFT on), GoogleTest (unit tests).

### Existing build (use this)

`IPPLfem/build_serial` is the historically configured serial Release
(`IPPL_PLATFORMS=serial`, FFT/SOLVERS/ALPINE/TESTS/UNIT_TESTS ON) against the older
`IPPLfem/ippl-frk` tree (now under `old/IPPLfem`). **`ipplFEM/build_serial` exists**
(configured 2026-08-20 for Task 7.7: serial Release, SOLVERS+TESTS, FFT/unit tests off).
Current active build (September 2026): **`ipplFEM/build`**, serial Release,
SOLVERS+TESTS+UNIT_TESTS enabled. Always **`-j 8`** per the user's updated memory
limit; never bare `-j`.

```bash
cd ipplFEM/build
cmake --build . -j 8
# FEM solver test executables: test/solver/fem/ and test/solver/fem_wContainer/
```
`compile_commands.json` is present in `build_serial` (use for clangd / static analysis).

### Key CMake options (`CMakeLists.txt:35-53`)
`IPPL_PLATFORMS` (`SERIAL|OPENMP|CUDA|HIP|SYCL`, semicolon list), `IPPL_ENABLE_FFT`, `IPPL_ENABLE_SOLVERS`, `IPPL_ENABLE_ALPINE`, `IPPL_ENABLE_TESTS`, `IPPL_ENABLE_UNIT_TESTS`, `IPPL_ENABLE_EXAMPLES`, `BUILD_SHARED_LIBS`.
> There is **no `IPPL_ENABLE_FEM` flag** — `src/FEM/` always compiles; FEM *solvers* need `IPPL_ENABLE_SOLVERS=ON`.
> `unit_tests/` require **both** `IPPL_ENABLE_UNIT_TESTS=ON` **and** `IPPL_ENABLE_FFT=ON`.

### Fresh configure examples
```bash
# OpenMP + alpine + FFTW
cmake -B build_openmp -S ipplFEM/ippl-frk -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=20 \
  -DIPPL_PLATFORMS=openmp -DIPPL_ENABLE_FFT=ON -DIPPL_ENABLE_SOLVERS=ON \
  -DIPPL_ENABLE_ALPINE=True -DIPPL_ENABLE_TESTS=ON -DHeffte_ENABLE_FFTW=True
# CUDA (Ampere example)
cmake -B build_cuda -S ipplFEM/ippl-frk -DCMAKE_BUILD_TYPE=Release -DIPPL_PLATFORMS=cuda \
  -DKokkos_ARCH_AMPERE80=ON -DCMAKE_CUDA_ARCHITECTURES=80 -DIPPL_ENABLE_FFT=ON \
  -DIPPL_ENABLE_SOLVERS=ON -DIPPL_ENABLE_ALPINE=True -DIPPL_USE_ALTERNATIVE_VARIANT=ON
```
Presets also exist in `CMakeUserPresets.json` (`default`, `release-testing`, `debug-testing`, `alps-gh200`, ...). See `INSTALLATION.md` for HPC specifics. The helper `IPPLfem/cmake.sh` documents the local serial/cuda config.

---

## 10. Mini-app skeleton (Manager pattern)

For full PIC apps, IPPL provides a manager scaffold. **Note the split:** generic framework in `src/Manager/`, *concrete reusable containers* in `alpine/`.

- `BaseManager` (`src/Manager/BaseManager.h`): `run(nt)` loop = `pre_step → advance → post_step`. `advance()` is pure virtual.
- `PicManager<T,Dim,pc,fc,orb>`: wires particle container, field container, solver, load balancer; `par2grid()` / `grid2par()` pure virtual.
- `alpine/FieldContainer.hpp`, `alpine/ParticleContainer.hpp`, `alpine/FieldSolver.hpp`, `alpine/AlpineManager.h` — reference implementations.
- Concrete apps: `alpine/LandauDamping.cpp`, `PenningTrap.cpp`, `BumponTailInstability.cpp` (built in `build_serial/alpine/`).

`main()` pattern (from `alpine/LandauDamping.cpp`): `initialize → parse argv → construct *Manager → manager.pre_run() → manager.run(nt) → IpplTimings::print() → finalize`. Example run:
```bash
./LandauDamping 128 128 128 10000 10 FFT 0.01 LeapFrog --overallocate 2.0 --info 10
#               <----nr---->  Np    Nt solver lbt step_method
```
Solver tokens accepted by `alpine/FieldSolver.hpp`: `FFT, CG, PCG, TG, OPEN, FEM, FEM_PRECON`.
For a manager-free PIC reference, read `test/particle/PICnd.cpp`.

---

## 11. Gotchas & agent footguns

1. **`field(i,j,k)` is read-only.** Write via `field.getView()(...)` or expression assignment.
2. **View indices are local+ghost, not global.** Convert with `getOwned()[d].first()` and the `nghost` offset.
3. **`Field::Centering_t` is hardcoded to `Cell`** in `Field.h:24` even though `Centering` is a template parameter. Vertex-centered behavior is emulated via mesh/nodes conventions, not this alias.
4. **Device vs host math:** never call `std::pow` inside a `KOKKOS_*` function — use `Kokkos::pow`. This compiles on CPU but breaks/regresses on GPU.
5. **`.hpp` is where the logic is.** Editing only the `.h` won't change behavior for template methods.
6. **Collective calls** (`create`, `globalCreate`, `destroy`, `update`, halo exchange, reductions) must be hit by **all ranks** — never inside a rank-conditional branch.
7. **Do not reinvent the wheel:** Before implementing your own solver loops, preconditioners, or interpolators, search the codebase (e.g., `src/LinearSolvers/Preconditioner.h`, `ippl::PCG`) to see if the functionality already exists. Track A often provides a blueprint for Track B.
8. **BCs aren't auto-applied** except by the differential-operator wrappers (`grad`/`laplace`/...) and inside the FEM solve loop. Call `bc.apply(field)` / `field.fillHalo()` yourself otherwise.
9. **Know the two FEM fork tracks** — see [§7.7](#77-fork-specific-fem). Track A (`FEMVector` + standalone `NedelecSpace`) works at first order, like the classic first-order `LagrangeSpace`. Track B (`DOFHandler` + `FEMContainer` + `LagrangeSpace_wFEMContainer`) is the generalized DOF machinery currently being debugged — validate it against `unit_tests/FEM/{DOFHandler,FEMContainer,FEMContainerHalo,LagrangeSpace_firstOrder,LagrangeSpace_higherOrder}.cpp`. Don't assume the empty `NedelecSpaceTag` traits / `fillNedelecDOFMappingTable` stub affect the working standalone `NedelecSpace`; they belong to a future Track-B Nédélec path.
10. **`unit_tests` need FFT enabled** even for non-FFT tests (CMake gate).
11. **Branch is `FEMContainer`** (not `master`); live tree for FEMContainer work is **`ipplFEM/ippl-frk`**. Remotes include the user's fork `origin` and upstreams `s-mayani`/`sona`.
12. **Unweighted FEM quadrature only.** Do not instantiate weighted Gauss–Jacobi / Chebyshev as FEM assembly rules (classes commented out). Use `GaussLegendreQuadrature` (Track A) or `SelectableQuadrature` (Track B). Weighted integrals belong in `ippl::nodes1d`.
13. **`RefShapeFunctionData`**, not `QuadratureData` / `FEMQuadratureData`. Functors take `.val_q` / `.deriv_q`.
14. **Do not register IPPL-linked tests with `NO_MPI`.** CSCS’s ctest loader needs `mpiexec` to find `libmpi_*.so`. Tests already call `ippl::initialize`/`finalize`.
15. **Task 7 GLL files** (`InterpolationNodeFamilies.h`, `SelectableQuadrature.h`, `LagrangeGLLNodes.cpp`) may be uncommitted in the working tree. Do not `git add -u` unless the user wants Task 7 staged. Mass shortcut is commented out. `QuadratureNodeFamily` lives next to `SelectableQuadrature`; 1D Lagrange fill is in `LagrangeDOFLocations::setFromFamily`.

---

## 12. File index

| Area | Primary headers |
|------|-----------------|
| Entry | `src/Ippl.h`, `src/IpplCore.h` |
| Types | `src/Types/{Vector,ViewTypes,IpplTypes,Tuple}.h` |
| Index/Region | `src/Index/{Index,NDIndex}.h`, `src/Region/{PRegion,NDRegion,RegionLayout}.h` |
| Mesh/Layout | `src/Meshes/{UniformCartesian,Mesh,Centering}.h`, `src/FieldLayout/{FieldLayout,SubFieldLayout}.h` |
| Fields | `src/Field/{BareField,Field,BConds,BcTypes,FieldOperations,HaloCells}.h/.hpp` |
| Expressions | `src/Expression/{IpplExpressions,IpplOperations}.h` |
| Particles | `src/Particle/{ParticleBase,ParticleAttrib,ParticleSpatialLayout,ParticleLayout,ParticleBC}.h` |
| Interpolation | `src/Interpolation/{CIC,CurrentDeposition}.{h,hpp}` |
| FEM core | `src/FEM/{FiniteElementSpace,LagrangeSpace,NedelecSpace,FiniteElementSpaceTraits}.h` |
| FEM elements/quad | `src/FEM/Elements/*.h`, `src/FEM/Quadrature/{Quadrature,GaussLegendreQuadrature,GaussLobattoQuadrature,MidpointQuadrature,SelectableQuadrature}.h` |
| FEM shape-fn bundle | `src/FEM/RefShapeFunctionData.h` (`.val_q` / `.deriv_q` at one quad node) |
| FEM interpolation families | `src/FEM/InterpolationNodeFamilies.h` (`LagrangeNodeFamily` + ParameterList parsers) |
| FEM Lagrange DOF coords | `src/FEM/DOFLocations.h` (`setFromFamily` fills 1D nodes on `[0,1]`) |
| 1D nodes | `src/Nodes1D/{RootFinderMethod,Nodes1D,GaussJacobi1D,GaussLegendre1D,GaussLobatto1D,GaussChebyshev1D,AffineMap}.h` |
| FEM fork | `src/FEM/{FEMVector,FEMContainer,LagrangeSpace_wFEMContainer,DOFHandler,DOFArray,DOFLocations,Entity,FEMHelperStructs,GridPathSegmenter,ProjectCurrent,FEMInterpolate}.{h,hpp}` |
| Poisson | `src/PoissonSolvers/{Poisson,FFTPeriodicPoissonSolver,FFTOpenPoissonSolver,PoissonCG,FEMPoissonSolver,PreconditionedFEMPoissonSolver,FEMPoissonSolver_wFEMContainer,EvalFunctor}.h` |
| Maxwell | `src/MaxwellSolvers/{Maxwell,FDTDSolverBase,StandardFDTDSolver}.h` |
| Linear | `src/LinearSolvers/{PCG,Preconditioner,SolverAlgorithm}.h` |
| FFT/MPI | `src/FFT/FFT.h`, `src/Communicate/{Communicator,Archive,Buffers,Window,Environment}.h` |
| Decomp | `src/Decomposition/OrthogonalRecursiveBisection.h`, `src/Partition/Partitioner.h` |
| Manager | `src/Manager/{BaseManager,PicManager,FieldSolverBase,datatypes}.h`, `alpine/{FieldContainer,ParticleContainer,FieldSolver,AlpineManager}.hpp` |
| Utility | `src/Utility/{Inform,IpplTimings,ParameterList,ParallelDispatch,PAssert,TypeUtils}.h` |
| Random | `src/Random/{Distribution,InverseTransformSampling,NormalDistribution,Randn,Randu}.h` |
| Docs | `README.md`, `INSTALLATION.md`, `WORKFLOW.md`, `UNIT_TESTS.md` |
| Canonical examples | `alpine/LandauDamping.cpp`, `test/particle/PICnd.cpp`, `test/solver/fem/TestNonhomDirichlet_2d.cpp` |

---
*Generated for the `ippl-frk` fork at `ipplFEM/ippl-frk`, branch `FEMContainer` (Nodes1D merged
`7d88383f`). Keep this file in sync with this tree; cite file:line so future agents can verify
against source.*
