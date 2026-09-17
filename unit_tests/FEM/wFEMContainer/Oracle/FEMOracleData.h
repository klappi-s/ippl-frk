// Hand-maintained header contract; numerical values live in FEMOracleData_{1,2,3}D.h.
// See README_oracles.md. This header needs only the C++20 standard library.
#pragma once

#include <cstddef>
#include <span>

namespace fem_oracle {

inline constexpr unsigned SCHEMA_VERSION = 3;
using Reals = std::span<const double>;
using Indices = std::span<const std::size_t>;

template <unsigned Dim, unsigned Order, unsigned QuadPoints>
struct CaseTag {
    static constexpr unsigned DIM = Dim;
    static constexpr unsigned ORDER = Order;
    static constexpr unsigned QUAD_POINTS = QuadPoints;
};

// All indices are zero-based. Arrays are row-major, last axis fastest.
struct Reference {
    Reals nodes;                  // [local DOF, dimension]: vertices then entity interiors
    Indices entityDimensions;     // [local DOF]
    Indices entityIndices;        // [local DOF]: entity number within its dimension
    Indices entityOffsets;        // [local DOF]: position within the entity
    Indices entityAxisMasks;      // [local DOF]: bit d marks an interior/free entity axis
    Indices entityLocalIndices;   // [local DOF, dimension]: entity origin relative to cell
    Reals probePoints;            // [probe, dimension]: external basis sample locations
    Reals basisValues;            // [probe, local DOF]
    Reals basisGradients;         // [probe, local DOF, dimension], reference derivatives
    unsigned quadPointsPerAxis;
    unsigned quadExactnessDegree;
    Reals quadPoints;             // [quadrature point, dimension], reference [0,1]^d
    Reals quadWeights;            // [quadrature point], on reference [0,1]^d
    Reals quadBasisValues;        // [quadrature point, local DOF]; optional for reference-only cases
    Reals quadBasisGradients;     // [quadrature point, local DOF, dimension]; likewise optional
    Reals stiffness;              // [local row, local column], raw reference matrix
    Reals mass;                   // [local row, local column], raw reference matrix
    Reals constantLoad;           // [local DOF], integral of basis function
};

struct Mesh {
    Indices cellsPerAxis;
    Reals origin;
    Reals corner;
    Reals coordinates;            // [global DOF, dimension], x-fastest coordinate order
    Indices cellDofs;             // [cell, local DOF], cells in x-fastest order
    Reals cellOrigins;            // [cell, dimension]
    Reals jacobians;              // [cell, dimension, dimension]
    Reals determinants;           // [cell], positive for the supported affine boxes
};

struct Elements {
    Reals stiffness;              // [cell, local row, local column], raw physical matrices
    Reals mass;                   // [cell, local row, local column]
};

struct Source {
    const char* expression;
    Reals nodalValues;            // [global DOF], includes physical boundary samples
    Reals interpolatedQuadValues; // [cell, quadrature point]
};

struct Load {
    Reals preBc;                  // [global DOF], integral(phi_i * interpolated source)
    Reals elementValues;          // [cell, local DOF]
    Reals analyticalSource;       // [global DOF], diagnostic: integral(phi_i * analytic f)
};

struct SparseMatrix {
    std::size_t rows;
    Indices rowOffsets;           // [rows+1], CSR; sorted columns, no numerical threshold
    Indices columns;
    Reals values;
};

struct Operator {
    SparseMatrix stiffness;      // raw matrix, no boundary modification
    SparseMatrix mass;
    Reals probe;                 // [global DOF], asymmetric coefficient vector
    Reals stiffnessAction;       // A * probe
    Reals massAction;            // M * probe
    Reals diagonal;              // diag(A)
    Reals diagonalAction;        // diag(A) * probe
    Reals inverseDiagonalAction; // probe / diag(A)
};

struct Boundary {
    const char* name;
    Indices constrainedDofs;     // C, sorted global indices
    Reals prescribedValues;      // g, indexed by C
    Indices freeDofs;            // F, sorted global indices
    Reals lift;                  // A_FC * g, indexed by F
    Reals liftedRhs;             // b_F - lift, indexed by F
    Reals operatorAction;        // A_FF * probe_F, indexed by F
    Reals lowerAction;           // strict lower(A_FF) * probe_F
    Reals upperAction;           // strict upper(A_FF) * probe_F
    Reals offDiagonalAction;     // (A_FF - diag(A_FF)) * probe_F
    Reals solution;              // [global DOF], includes prescribed boundary values
};

struct Diagnostics {
    Reals otherVector;           // [global DOF], second operand of coefficientInnerProduct
    const char* exactExpression;
    Reals exactNodalValues;      // [global DOF], exactExpression sampled at the FE nodes
    double coefficientL1;       // norms/products of Operator::probe, each global DOF once
    double coefficientL2;
    double coefficientLInf;
    double coefficientInnerProduct;
    double physicalL2;          // FE function represented by Operator::probe
    double absoluteL2Error;     // FE probe function minus exactExpression
    double exactL2;
    double relativeL2Error;     // absoluteL2Error / exactL2
};

struct Case {
    const char* name;
    unsigned dimension;
    unsigned order;
    const char* interpolationFamily;
    const char* quadratureFamily;
    Reference reference;
    Mesh mesh;
    Elements elements;
    Source source;
    Load load;
    Operator op;
    std::span<const Boundary> boundaries;
    Diagnostics diagnostics;
};

// Reference-stage cases deliberately omit global assembly/solver data.
struct ReferenceCase {
    const char* name;
    unsigned dimension;
    unsigned order;
    const char* interpolationFamily;
    Reference reference;
    Mesh mesh;
    Indices boundaryAxes;         // [global DOF, dimension], coordinate-based boundary flags
    Reals physicalStiffness;      // [local row, local column], identical for every uniform cell
    Reals physicalMass;           // [local row, local column]
};

// Operator-stage data shares geometry with REFERENCE_CASES; no new full solves.
struct OperatorCase {
    const ReferenceCase& geometry;
    Source source;
    Load load;
    Operator op;
    std::span<const Boundary> boundaries; // Includes independently solved global coefficients
    Diagnostics diagnostics;
    Reals constantLoad;         // global load for f=1
    Reals singleCellSource;     // first physical cell, canonical local DOF order
    Reals singleCellLoad;       // physical mass * singleCellSource
    Reals sampledConvergenceSource; // existing HighOrderPolynomial sampler
    Reals sampledConvergenceScale; // eps * scale bounds source/coordinate roundoff; see generator
    Indices impulseDofs;        // one interior representative per entity axis mask
};

} // namespace fem_oracle
