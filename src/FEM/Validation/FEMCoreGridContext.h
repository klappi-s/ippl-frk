// Unit [0,1]^d core grid with nel=4 for matrix-free vs explicit validation.
#ifndef IPPL_FEM_VALIDATION_COREGRIDCONTEXT_H
#define IPPL_FEM_VALIDATION_COREGRIDCONTEXT_H

#include "FEM/LagrangeSpace_wFEMContainer.h"

namespace ippl::femvalidate {

/// Mesh, layout, quadrature, and LagrangeSpace_wfc on the 4×4×4 core fenics grid.
template <unsigned Dim, unsigned Order, unsigned QuadNodes = 9>
struct CoreGridContext;

}  // namespace ippl::femvalidate

#include "FEM/Validation/FEMCoreGridContext.hpp"

#endif
