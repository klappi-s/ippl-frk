# FEM oracle data: contents and consumers

Numerical arrays are supplied by the pinned `ipplTestData` package. This IPPL
folder keeps the schema, loader, dependency lock, and test support. Normal builds
load the package without Python. See [the build guide](../README_oracles.md) and
[the external package](https://github.com/klappi-s/ipplTestData).

This catalog describes the published `v1.0.0` dataset pinned by
[ipplTestData.lock.json](ipplTestData.lock.json). Case order and test consumers
were checked against that release on 16 September 2026.

There are **36 configurations: 12 per dimension**, covering P1–P5 with equispaced
and GLL interpolation plus stretched P3 meshes. GL quadrature uses `max(5,p+1)`
points per axis. No cases or matrix entries were removed in the storage migration.

**Package files and stored order**

Paths below are relative to the extracted package root printed by CMake. For each
N, three files describe the same records:

| File | Contents and use |
|---|---|
| `data/fem-Nd.bin` | Exact numerical arrays, shared only when byte-identical. Read once per dimension/process by all three suites. |
| `data/fem-Nd.json` | Readable record fields and every array's bank, offset, count, and shape; used for inspection. |
| `include/FEMOracleBindings_ND.h` | Small `DataN` descriptors exposing `legacyCases`, `referenceCases`, and `operatorCases`; compiled by `FEMOracleTestData.cpp`. |

Within each descriptor header and JSON, records occur as **legacy, reference,
operator**. The binary stores deduplicated arrays in order of first occurrence,
first the f64 bank and then the u32 bank; case locations are given by the JSON.
The following tables apply separately to **each of those three files** for its
dimension. Both `referenceCases[i]` and `operatorCases[i]` have the listed case ID.
Reference fields feed `FEMReferenceOracleND`; operator fields feed
`FEMOperatorOracleND` and `FEMPoissonOracleND`.

| Dimension | Unit mesh cells | Stretched mesh cells | Stretched origin → corner |
|---|---|---|---|
| 1D | 4 | 4 | `(-0.25)` → `(1.75)` |
| 2D | `(3,2)` | `(3,2)` | `(-0.25,0.5)` → `(1.75,3.5)` |
| 3D | `(2,2,2)` | `(3,2,2)` | `(-0.25,0.5,-0.75)` → `(1.75,3.5,0.75)` |

**`fem-1d.bin`, `fem-1d.json`, `FEMOracleBindings_1D.h`**

`legacyCases` comes first and retains the two original complete P3 examples,
including full quadrature basis tables and per-cell matrices. Python checks these;
current C++ numerical suites consume the reference/operator records below.

| Legacy index | Case ID |
|---|---|
| 0 | `p3_1d_equispaced_gl5_4cells` |
| 1 | `p3_1d_gll_gl5_4cells` |

Then `referenceCases`, followed by `operatorCases`, each in this order:

| Index | Case ID |
|---|---|
| 0 | `p1_1d_equispaced_gl5_unit` |
| 1 | `p1_1d_gll_gl5_unit` |
| 2 | `p2_1d_equispaced_gl5_unit` |
| 3 | `p2_1d_gll_gl5_unit` |
| 4 | `p3_1d_equispaced_gl5_unit` |
| 5 | `p3_1d_gll_gl5_unit` |
| 6 | `p4_1d_equispaced_gl5_unit` |
| 7 | `p4_1d_gll_gl5_unit` |
| 8 | `p5_1d_equispaced_gl6_unit` |
| 9 | `p5_1d_gll_gl6_unit` |
| 10 | `p3_1d_equispaced_gl5_stretched` |
| 11 | `p3_1d_gll_gl5_stretched` |

**`fem-2d.bin`, `fem-2d.json`, `FEMOracleBindings_2D.h`**

`legacyCases` is empty. Reference and operator arrays each use this order:

| Index | Case ID |
|---|---|
| 0 | `p1_2d_equispaced_gl5_unit` |
| 1 | `p1_2d_gll_gl5_unit` |
| 2 | `p2_2d_equispaced_gl5_unit` |
| 3 | `p2_2d_gll_gl5_unit` |
| 4 | `p3_2d_equispaced_gl5_unit` |
| 5 | `p3_2d_gll_gl5_unit` |
| 6 | `p4_2d_equispaced_gl5_unit` |
| 7 | `p4_2d_gll_gl5_unit` |
| 8 | `p5_2d_equispaced_gl6_unit` |
| 9 | `p5_2d_gll_gl6_unit` |
| 10 | `p3_2d_equispaced_gl5_stretched` |
| 11 | `p3_2d_gll_gl5_stretched` |

**`fem-3d.bin`, `fem-3d.json`, `FEMOracleBindings_3D.h`**

`legacyCases` is empty. Reference and operator arrays each use this order:

| Index | Case ID |
|---|---|
| 0 | `p1_3d_equispaced_gl5_unit` |
| 1 | `p1_3d_gll_gl5_unit` |
| 2 | `p2_3d_equispaced_gl5_unit` |
| 3 | `p2_3d_gll_gl5_unit` |
| 4 | `p3_3d_equispaced_gl5_unit` |
| 5 | `p3_3d_gll_gl5_unit` |
| 6 | `p4_3d_equispaced_gl5_unit` |
| 7 | `p4_3d_gll_gl5_unit` |
| 8 | `p5_3d_equispaced_gl6_unit` |
| 9 | `p5_3d_gll_gl6_unit` |
| 10 | `p3_3d_equispaced_gl5_stretched` |
| 11 | `p3_3d_gll_gl5_stretched` |

Within each complete P3 example and every operator case, `boundaries` is ordered:

| Index | Boundary case | Data |
|---|---|---|
| 0 | `zero` | Zero Dirichlet; constrained/free indices, lift, RHS, free-block actions, and full solution. |
| 1 | `constant_0_75` | The same quantities with `g=0.75` on every boundary face. |

For each operator case, [FEMPoissonOracle.cpp](../FEMPoissonOracle.cpp) registers
these eight solve variants in this order (each name starts with the case ID):

1. `<case>_CG_zero`
2. `<case>_CG_constant_0_75`
3. `<case>_CG_ZeroSource`
4. `<case>_CG_ZeroSourceConstantBoundary`
5. `<case>_Jacobi_zero`
6. `<case>_Jacobi_constant_0_75`
7. `<case>_Jacobi_ZeroSource`
8. `<case>_Jacobi_ZeroSourceConstantBoundary`

`ZeroSource` and `ZeroSourceConstantBoundary` are constructed by the C++ test
and have no separate stored dataset. Their exact solutions are respectively
zero and the prescribed constant. CTest repeats operator/solve registrations
with `standard`, `rowsum_diagonal`, and `constant_preserving` stiffness modes,
using the same numerical package and tolerances in every mode.
These are per-case variant orders; typed registration, MPI launches, and CTest
sharding determine execution order separately.

**Package `include/FEMOracleRegistry.h`**

Generated compile-time tags, with fields `<dimension, order, quadrature points>`.
There are no numerical case arrays here. All suites use these tags to instantiate
their tests; [CMake](../CMakeLists.txt) also derives operator compilation units from them.
The file declares `d1::CaseTypes`, then `d2::CaseTypes`, then `d3::CaseTypes`.
Each tuple contains the following tags in index order:

| Tuple index | `d1::CaseTypes` | `d2::CaseTypes` | `d3::CaseTypes` |
|---|---|---|---|
| 0 | `CaseTag<1, 1, 5>` | `CaseTag<2, 1, 5>` | `CaseTag<3, 1, 5>` |
| 1 | `CaseTag<1, 2, 5>` | `CaseTag<2, 2, 5>` | `CaseTag<3, 2, 5>` |
| 2 | `CaseTag<1, 3, 5>` | `CaseTag<2, 3, 5>` | `CaseTag<3, 3, 5>` |
| 3 | `CaseTag<1, 4, 5>` | `CaseTag<2, 4, 5>` | `CaseTag<3, 4, 5>` |
| 4 | `CaseTag<1, 5, 6>` | `CaseTag<2, 5, 6>` | `CaseTag<3, 5, 6>` |

Both node families share each tag; the concrete family and geometry come from
the ordered reference/operator case arrays above.

**[FEMOracleData.h](FEMOracleData.h)**

Hand-maintained schema, array shapes, and version. **No test cases or numerical
expectations.** Used by all generated headers and their consumers. Structs appear
in this order: `CaseTag`, `Reference`, `Mesh`, `Elements`, `Source`, `Load`,
`SparseMatrix`, `Operator`, `Boundary`, `Diagnostics`, `Case`, `ReferenceCase`,
`OperatorCase`.

**[FEMOracleTestData.h](FEMOracleTestData.h)**

**No stored test cases.** Declares `operatorCases()`, `referenceCases()`, then
`legacyCases()`. [FEMOracleTestData.cpp](../FEMOracleTestData.cpp) owns the runtime
storage and returns immutable spans. All three suites use these accessors.

**[FEMOracleTestSupport.h](FEMOracleTestSupport.h)**

**No stored test cases.** Provides fixtures, scatter/gather, MPI ownership checks,
comparison helpers, and application of the external CSR matrices. Used by
[FEMOperatorOracle.cpp](../FEMOperatorOracle.cpp),
[FEMPoissonOracle.cpp](../FEMPoissonOracle.cpp), and
[FEMOperatorOracleMain.cpp](../FEMOperatorOracleMain.cpp).

**[FEMOracleRuntime.h](FEMOracleRuntime.h)**

**No oracle cases.** Loads format-1 array banks, converts portable indices, and
checks format, lengths, and span bounds. `FEMOracleRuntimeTest.cpp` tests, in order:
`ExactBitsIndicesAndBounds`, `MissingTruncatedAndTrailingDataAreRejected`, and
`WrongMagicVersionDimensionAndCountsAreRejected`.

**[FEMOracleDependency.cmake](FEMOracleDependency.cmake) and
[ipplTestData.lock.json](ipplTestData.lock.json)**

**No test cases.** Resolve the exact package and verify archive, manifest, payload,
and schema hashes before compilation. The lock identifies the reviewed release.

**Package `manifest.json` and generator `generator/create_FEMOracleData.py`**

The manifest records provenance and hashes plus the following complete ordered
registry. The generator owns that registry and all numerical checks. Filtering
by dimension gives each file's order above.

| Registry index | Case ID |
|---|---|
| 0 | `p1_1d_equispaced_gl5_unit` |
| 1 | `p1_1d_gll_gl5_unit` |
| 2 | `p2_1d_equispaced_gl5_unit` |
| 3 | `p2_1d_gll_gl5_unit` |
| 4 | `p3_1d_equispaced_gl5_unit` |
| 5 | `p3_1d_gll_gl5_unit` |
| 6 | `p4_1d_equispaced_gl5_unit` |
| 7 | `p4_1d_gll_gl5_unit` |
| 8 | `p5_1d_equispaced_gl6_unit` |
| 9 | `p5_1d_gll_gl6_unit` |
| 10 | `p1_2d_equispaced_gl5_unit` |
| 11 | `p1_2d_gll_gl5_unit` |
| 12 | `p2_2d_equispaced_gl5_unit` |
| 13 | `p2_2d_gll_gl5_unit` |
| 14 | `p3_2d_equispaced_gl5_unit` |
| 15 | `p3_2d_gll_gl5_unit` |
| 16 | `p4_2d_equispaced_gl5_unit` |
| 17 | `p4_2d_gll_gl5_unit` |
| 18 | `p5_2d_equispaced_gl6_unit` |
| 19 | `p5_2d_gll_gl6_unit` |
| 20 | `p1_3d_equispaced_gl5_unit` |
| 21 | `p1_3d_gll_gl5_unit` |
| 22 | `p2_3d_equispaced_gl5_unit` |
| 23 | `p2_3d_gll_gl5_unit` |
| 24 | `p3_3d_equispaced_gl5_unit` |
| 25 | `p3_3d_gll_gl5_unit` |
| 26 | `p4_3d_equispaced_gl5_unit` |
| 27 | `p4_3d_gll_gl5_unit` |
| 28 | `p5_3d_equispaced_gl6_unit` |
| 29 | `p5_3d_gll_gl6_unit` |
| 30 | `p3_1d_equispaced_gl5_stretched` |
| 31 | `p3_1d_gll_gl5_stretched` |
| 32 | `p3_2d_equispaced_gl5_stretched` |
| 33 | `p3_2d_gll_gl5_stretched` |
| 34 | `p3_3d_equispaced_gl5_stretched` |
| 35 | `p3_3d_gll_gl5_stretched` |

**External `generator/runtime_format.py` and `oracle-environment.yml`**

**No additional cases.** The writer serializes checked records, shares exact
arrays, emits descriptors/JSON, and packages deterministic archives. The YAML
pins the generation stack. Both live in `ipplTestData`; neither runs during
ordinary C++ builds. Package `schema/FEMOracleData.h` is an exact copy of IPPL's
schema, with no test cases.

**Reference data → reference tests**

For `const auto& r = fem_oracle::referenceCases()[i]`, these members are in
the dimension’s runtime package. The test names below belong to
[FEMReferenceOracle.cpp](../FEMReferenceOracle.cpp).

| Data members | What they describe | Test using them |
|---|---|---|
| `r.reference.nodes`, `r.mesh.coordinates`, `cellOrigins`, `jacobians`, `determinants` | Local/physical DOF positions and affine cell geometry. | `CoordinatesAndGeometry` |
| `r.reference.entityAxisMasks`, `entityOffsets`, `entityLocalIndices`; `r.mesh.cellDofs`; `r.boundaryAxes` | Entity membership, local/global numbering, connectivity, and physical boundary flags. | `DofMappingsAndBoundaryFlags`, `DeviceBasisAndMappings` |
| `r.reference.probePoints`, `basisValues`, `basisGradients`, `nodes` | Basis/derivative values at asymmetric probes and interpolation nodes. | `HostBasisAndGradients`, `DeviceBasisAndMappings`, `PolynomialReproductionThroughOrder` |
| `r.reference.quadPoints`, `quadWeights`, `quadExactnessDegree`, `stiffness`, `mass`; `r.physicalStiffness`, `r.physicalMass` | Reference quadrature and dense reference/physical element matrices. | `QuadratureAndElementMatrices`; physical matrices also supply the operator suite's single-cell column expectations. |

An operator case's `c.geometry` refers to one of these reference cases. Uniform
cells share the physical element matrices. The reference cases omit the large
`quadBasisValues` and `quadBasisGradients` tables; those spans are empty.

**Operator/solution data → field and solver tests**

For `const auto& c = fem_oracle::operatorCases()[i]`, the following quantities
come from the dimension’s runtime package. The operator test names belong to
[FEMOperatorOracle.cpp](../FEMOperatorOracle.cpp).

| Data members | Meaning | Where used |
|---|---|---|
| `c.geometry.mesh.coordinates`, `c.op.probe` | External coordinates and an asymmetric coefficient vector. | `AdapterAgainstProductionFillAndExternalCoordinates` checks the field adapter and production fill. |
| `c.geometry.physicalStiffness`, `physicalMass`; `c.singleCellSource`, `singleCellLoad` | Full element matrices and one cell's nodal source/load. | `SingleCellStiffnessAndMassColumnsNoFace` checks every stiffness, mass, and impulse-load column. |
| `c.op.stiffness`, `mass`, `stiffnessAction`, `massAction`; `c.impulseDofs` | Raw global CSR matrices, their actions on the probe, and representative entity impulses. | `RawWholeFieldActionsAndEntityImpulses`; stiffness also supplies the independent solver residual. |
| `c.source.nodalValues`, `c.load.preBc`, `c.constantLoad` | Source samples, assembled load before constraints, and the load for source one. | `NodalLoadRetainsBoundarySourceSamples`; nodal source and raw load also drive complete solves. |
| `c.sampledConvergenceSource`, `sampledConvergenceScale` | Expected samples of the convergence driver's polynomial source and its float roundoff allowance. | `SourceSamplingUsesPhysicalNodesAndSelectedFamily` |
| `c.boundaries[k].constrainedDofs`, `freeDofs`, `prescribedValues`, `operatorAction`, `lift`, `liftedRhs` | Boundary/free DOFs, prescribed values, constrained action, `A_FC*g`, and `b_F-A_FC*g`. | `ConstrainedActionsBoundaryLiftAndRhs`; boundary indices/values also check solver output. |
| `c.op.diagonal`; `c.boundaries[k].lowerAction`, `upperAction`, `offDiagonalAction` | Global diagonal and strict triangular/off-diagonal actions on free DOFs. | `DiagonalInverseAndGlobalTriangularBlocks` |
| `c.geometry.reference.nodes`, `c.op.stiffness`, boundary indices | Affine energy probes and independent constrained constant-vector action; other expectations are structural identities. | `ConstantActionBoundaryColumnsAndSplitConsistency` |
| `c.diagnostics.*` | Coefficient norms/inner product, a second vector, physical L² norm, and absolute/relative FE errors. | `CoefficientNormsExcludeGhostsAndPhysicalErrorIsAbsolute` |
| **`c.boundaries[k].solution`** | **Full discrete solution, including prescribed boundary coefficients.** | [FEMPoissonOracle.cpp](../FEMPoissonOracle.cpp): CG and Jacobi-PCG coefficient comparisons, together with residual checks against `c.op.stiffness` and immutable `c.load.preBc`. |

`boundaries[0]` is zero Dirichlet; `boundaries[1]` is constant `0.75`. Complete
solve tests also construct zero-source cases with zero and constant solutions from each fixture;
there is no separate stored zero-source dataset. The same data serve all MPI
rank counts. Reference/operator tests use double and float; complete solves use double.

Additional exported fields support inspection and Python checks, including
`source.interpolatedQuadValues`, `load.elementValues`, and `load.analyticalSource`.
The C++ load expectation is **`load.preBc`**, assembled from the interpolated nodal
source. `load.analyticalSource` integrates the analytical function directly and
can differ. Diagonal tests derive their expectations from `op.diagonal` and the
test input; the exported `op.diagonalAction` and `inverseDiagonalAction` describe
the original probe specifically.

**Reading or extending the data**

Use case descriptors instead of long generated array names. For example, from a
test source in the parent folder:

```cpp
#include "Oracle/FEMOracleTestData.h"

const auto& c = fem_oracle::operatorCases()[0];
const auto nodes = c.geometry.mesh.coordinates;
const auto& A = c.op.stiffness;                  // raw global CSR matrix
const auto b = c.load.preBc;                    // raw global load
const auto& bc = c.boundaries[1];               // constant Dirichlet case
const auto u = bc.solution;                     // full expected coefficients
```

Indices are zero-based. Global DOFs/cells use x-fastest ordering; local DOFs use
the documented vertex/edge/face/interior order. Dense matrices are row-major.
Boundary actions/lifts are indexed by `freeDofs`; solution vectors use all global
DOFs. [FEMOracleData.h](FEMOracleData.h) gives the complete field shapes.

To add cases, edit `CASE_REGISTRY` in the external generator, regenerate and
check the complete package, publish a new version, and update IPPL's lock. Basix
supplies basis data, DOLFINx assembles matrices/loads, and SciPy solves constrained
systems with a DOLFINx/PETSc cross-check. IPPL supplies no expected numerical
values. See the [regeneration guide](../README_oracles.md#recreate-the-data).
