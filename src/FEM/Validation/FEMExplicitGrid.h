// Grid DOF indexing and dense Dirichlet solve helpers for FEM validation/export.
#ifndef IPPL_FEM_VALIDATION_EXPLICITGRID_H
#define IPPL_FEM_VALIDATION_EXPLICITGRID_H

#include <array>
#include <cstddef>
#include <vector>

#include "Types/Vector.h"

namespace ippl::femdata {
namespace detail {

/// Decode lexicographic global DOF index into per-axis mesh node indices.
template <unsigned Dim>
void decode_ippl_dof(std::size_t g, const std::array<unsigned, Dim>& nx,
                     std::array<unsigned, Dim>& ix);

/// Physical coordinates of global DOF \p g on a tensor-product Lagrange grid.
template <unsigned Dim>
ippl::Vector<double, Dim> dof_coord(std::size_t g, const std::array<unsigned, Dim>& nel,
                                    unsigned order, const std::array<double, Dim>& origin,
                                    const std::array<double, Dim>& corner);

/// True when \p g lies on the domain boundary (homogeneous Dirichlet support).
template <unsigned Dim>
bool is_boundary_dof(std::size_t g, unsigned order, const std::array<unsigned, Dim>& nel);

/// Dense Dirichlet solve with FEniCS-style row lifting (constrained rows → identity).
std::vector<double> solve_dirichlet(std::vector<std::vector<double>> A, std::vector<double> b,
                                    const std::vector<std::size_t>& constrained, double bc_val);

}  // namespace detail
}  // namespace ippl::femdata

#include "FEM/Validation/FEMExplicitGrid.hpp"

#endif
