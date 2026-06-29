// Explicit assembly, scatter/gather, and field host-access helpers for FEM validation.
#ifndef IPPL_FEM_VALIDATION_EXPLICITTOOLS_H
#define IPPL_FEM_VALIDATION_EXPLICITTOOLS_H

#include <functional>
#include <vector>

#include "FEM/Validation/FEMExplicitGrid.h"
#include "FEM/LagrangeSpace_wFEMContainer.h"
#include "PoissonSolvers/EvalFunctor.h"

namespace ippl::femvalidate {

template <typename LagrangeType>
std::vector<std::size_t> ownedElementIndicesHost(const LagrangeType& space);

namespace detail {

template <typename LagrangeType, typename FieldType>
std::vector<double> gatherFieldToGlobal(const LagrangeType& space, const FieldType& field);

template <unsigned Dim, unsigned Order>
std::vector<double> applyZeroFaceGlobal(const std::vector<std::vector<double>>& A,
                                        const std::vector<double>& v,
                                        const std::array<unsigned, Dim>& nel);

template <typename LagrangeType>
std::vector<std::vector<double>> assembleGlobalStiffness(
    const LagrangeType& space, const std::vector<std::vector<std::vector<double>>>& cell_A);

template <typename LagrangeType, typename FieldType>
double relativeFieldNormError(const LagrangeType& space, const FieldType& approx,
                              const FieldType& reference,
                              const std::function<bool(std::size_t)>& skip_dof = {});

}  // namespace detail

template <typename LagrangeType, typename ElementType>
EvalFunctor<typename LagrangeType::FEMContainer_t::value_type, LagrangeType::dim,
            LagrangeType::numElementDOFs>
makePoissonEvalFunctor(const LagrangeType& space, const ElementType& ref_element);

template <typename LagrangeType, typename ElementType, typename QuadratureType>
std::vector<std::vector<std::vector<double>>> assembleCellStiffnessMatrices(
    const LagrangeType& space, const ElementType& ref_element, const QuadratureType& quadrature);

template <typename LagrangeType, typename FieldType>
void scatterGlobalPattern(FieldType& field, const LagrangeType& space,
                          const std::function<double(std::size_t)>& pattern);

}  // namespace ippl::femvalidate

#include "FEM/Validation/FEMExplicitTools.hpp"

#endif
