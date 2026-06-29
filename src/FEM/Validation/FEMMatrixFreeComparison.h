// Matrix-free vs explicit comparison and residual helpers for FEM validation.
#ifndef IPPL_FEM_VALIDATION_MATRIXFREECOMPARISON_H
#define IPPL_FEM_VALIDATION_MATRIXFREECOMPARISON_H

#include <functional>
#include <vector>

#include "FEM/Validation/FEMCoreGridContext.h"
#include "FEM/Validation/FEMExplicitTools.h"

namespace ippl::femvalidate {

template <unsigned Dim, unsigned Order, unsigned QuadNodes = 9>
double relativeErrorEvaluateAxVsExplicit(const std::function<double(std::size_t)>& pattern,
                                         double rtol_scale = 1.0);

template <unsigned Dim, unsigned Order, unsigned QuadNodes = 9>
std::vector<double> assembleExplicitGlobalLoad(
    const std::function<double(const ippl::Vector<double, Dim>&)>& source);

template <unsigned Dim, unsigned Order, unsigned QuadNodes = 9>
double relativeErrorEvaluateLoadVectorVsExplicit(
    const std::function<double(const ippl::Vector<double, Dim>&)>& source,
    bool interior_only = true);

template <unsigned Dim, unsigned Order, unsigned QuadNodes = 9>
std::vector<double> solveExplicitCorePoisson(
    const std::function<double(const ippl::Vector<double, Dim>&)>& source, double boundary_value);

template <unsigned Dim, unsigned Order, unsigned QuadNodes = 9>
double relativeExplicitSolutionResidual(
    const std::function<double(const ippl::Vector<double, Dim>&)>& source,
    bool interior_only = true);

template <unsigned Dim, unsigned Order, unsigned QuadNodes = 9>
double relativeGlobalVectorScatterGatherError(const std::vector<double>& u_global);

template <unsigned Dim, unsigned Order, unsigned QuadNodes = 9>
double relativeMatrixFreeResidualAtExplicitSolution(
    const std::function<double(const ippl::Vector<double, Dim>&)>& source);

template <typename LagrangeType, typename ElementType, typename FieldType>
double relativeSolutionOperatorResidual(const LagrangeType& space, const ElementType& ref_element,
                                        const FieldType& u_field, const FieldType& load_field);

template <typename LagrangeType, typename FieldType>
double globalSolutionError(const LagrangeType& space, const FieldType& u_field,
                           const std::vector<double>& u_global);

}  // namespace ippl::femvalidate

#include "FEM/Validation/FEMMatrixFreeComparison.hpp"

#endif
