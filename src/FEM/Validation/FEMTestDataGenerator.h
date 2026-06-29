// Generate FEM Poisson reference data (SCHEMA.md) via LagrangeSpace_wfc + explicit or CG solve.
#ifndef IPPL_FEM_VALIDATION_TESTDATAGENERATOR_H
#define IPPL_FEM_VALIDATION_TESTDATAGENERATOR_H

#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "FEM/Validation/FEMDataExport.h"
#include "FEM/Validation/FEMExplicitGrid.h"
#include "FEM/LagrangeSpace_wFEMContainer.h"

namespace ippl::femdata {

/// Solution path for SCHEMA `solution_vector_u.txt`.
enum class SolveMode {
    Explicit,  ///< Dense assembled K + Gaussian elimination (default for fenics regression)
    CG         ///< Track B matrix-free CG via FEMPoissonSolver_wFEMContainer
};

template <unsigned Dim, unsigned Order, unsigned QuadNodes>
struct FemTestDataGenerator;

}  // namespace ippl::femdata

#include "FEM/Validation/FEMTestDataGenerator.hpp"

#endif
