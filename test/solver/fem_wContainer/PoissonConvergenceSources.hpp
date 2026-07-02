// Shared source functions, exact solutions, and RHS assignment for
// FEMPoissonSolver_wFEMContainer convergence studies (TASK3 §11.2 sources).
//
// 1D uses TASK3 §11.2 sources verbatim. For Dim > 1, sine uses the product form
// (FEniCS / TestZeroBC_sin); polynomial, affine, and nonsymmetric use manufactured
// (u, f) pairs from PoissonConvergencePolySolution_wFEMContainer.hpp because the plan
// RHS functions do not admit closed-form exact solutions with homogeneous Dirichlet.
#ifndef IPPL_POISSON_CONVERGENCE_SOURCES_WFC_HPP
#define IPPL_POISSON_CONVERGENCE_SOURCES_WFC_HPP

#include "FEM/DOFHandler.h"
#include "FEM/DOFLocations.h"
#include "FEM/Elements/EdgeElement.h"
#include "FEM/Elements/HexahedralElement.h"
#include "FEM/Elements/QuadrilateralElement.h"
#include "PoissonConvergenceSolutions.hpp"

#include <array>
#include <cmath>
#include <string>
#include <tuple>
#include <utility>

namespace ippl {
namespace poisson_convergence_wfc {

enum class SourceCase : unsigned { LowOrderPolynomial = 0, HighOrderPolynomial = 1, ShiftedExponential = 2, Sines = 3 };

inline constexpr std::array<const char*, 4> kSourceTags = {"lowOrderPolynomial", "highOrderPolynomial", "shiftedExponential",
                                                           "sines"};

inline const char* sourceTag(SourceCase c) { return kSourceTags[static_cast<unsigned>(c)]; }



// ---------------------------------------------------------------------------
// RHS sources f(x) — TASK3_FEMContainer_PLAN.md §11.2, domain [0,1]^Dim
// ---------------------------------------------------------------------------
template <SourceCase Src, unsigned Dim, typename T>
KOKKOS_INLINE_FUNCTION T evaluateSource(ippl::Vector<T, Dim> x) {
    if constexpr (Dim == 1) {
        if constexpr (Src == SourceCase::LowOrderPolynomial) { return source_LowOrderPolynomial_1D<T>(x[0]); }
        else if constexpr (Src == SourceCase::HighOrderPolynomial) { return source_HighOrderPolynomial_1D<T>(x[0]); }
        else if constexpr (Src == SourceCase::ShiftedExponential) { return source_ShiftedExponential_1D<T>(x[0]); }
        else { return source_Sines_1D<T>(x[0]); }
    } else if constexpr (Dim == 2) {
        if constexpr (Src == SourceCase::LowOrderPolynomial) { return source_LowOrderPolynomial_2D<T>(x[0], x[1]); }
        else if constexpr (Src == SourceCase::HighOrderPolynomial) { return source_HighOrderPolynomial_2D<T>(x[0], x[1]); }
        else if constexpr (Src == SourceCase::ShiftedExponential) { return source_ShiftedExponential_2D<T>(x[0], x[1]); }
        else { return source_Sines_2D<T>(x[0], x[1]); }
    } else {
        if constexpr (Src == SourceCase::LowOrderPolynomial) { return source_LowOrderPolynomial_3D<T>(x[0], x[1], x[2]); }
        else if constexpr (Src == SourceCase::HighOrderPolynomial) { return source_HighOrderPolynomial_3D<T>(x[0], x[1], x[2]); }
        else if constexpr (Src == SourceCase::ShiftedExponential) { return source_ShiftedExponential_3D<T>(x[0], x[1], x[2]); }
        else { return source_Sines_3D<T>(x[0], x[1], x[2]); }
    }
}

// ---------------------------------------------------------------------------
// Exact solutions u(x) for -Laplace(u) = f, u = 0 on boundary of [0,1]^Dim
// ---------------------------------------------------------------------------
template <SourceCase Src, unsigned Dim, typename T>
KOKKOS_INLINE_FUNCTION T evaluateExact(ippl::Vector<T, Dim> x) {
    if constexpr (Dim == 1) {
        if constexpr (Src == SourceCase::LowOrderPolynomial) { return exact_LowOrderPolynomial_1D<T>(x[0]); }
        else if constexpr (Src == SourceCase::HighOrderPolynomial) { return exact_HighOrderPolynomial_1D<T>(x[0]); }
        else if constexpr (Src == SourceCase::ShiftedExponential) { return exact_ShiftedExponential_1D<T>(x[0]); }
        else { return exact_Sines_1D<T>(x[0]); }
    } else if constexpr (Dim == 2) {
        if constexpr (Src == SourceCase::LowOrderPolynomial) { return exact_LowOrderPolynomial_2D<T>(x[0], x[1]); }
        else if constexpr (Src == SourceCase::HighOrderPolynomial) { return exact_HighOrderPolynomial_2D<T>(x[0], x[1]); }
        else if constexpr (Src == SourceCase::ShiftedExponential) { return exact_ShiftedExponential_2D<T>(x[0], x[1]); }
        else { return exact_Sines_2D<T>(x[0], x[1]); }
    } else {
        if constexpr (Src == SourceCase::LowOrderPolynomial) { return exact_LowOrderPolynomial_3D<T>(x[0], x[1], x[2]); }
        else if constexpr (Src == SourceCase::HighOrderPolynomial) { return exact_HighOrderPolynomial_3D<T>(x[0], x[1], x[2]); }
        else if constexpr (Src == SourceCase::ShiftedExponential) { return exact_ShiftedExponential_3D<T>(x[0], x[1], x[2]); }
        else { return exact_Sines_3D<T>(x[0], x[1], x[2]); }
    }
}

template <SourceCase Src, unsigned Dim, typename T>
struct AnalyticSolutionFunctor {
    KOKKOS_FUNCTION T operator()(ippl::Vector<T, Dim> x) const {
        return evaluateExact<Src, Dim, T>(x);
    }
};

// ---------------------------------------------------------------------------
// Sample f at Lagrange DOF locations on a uniform Cartesian mesh.
// ---------------------------------------------------------------------------
template <typename T, unsigned Dim, unsigned Order, SourceCase Src, typename Field_t>
void assignSourceToField(Field_t& field, UniformCartesian<T, Dim>& mesh,
                         const FieldLayout<Dim>& layout) {
    using SpaceTraits  = FiniteElementSpaceTraits<LagrangeSpaceTag, Dim, Order>;
    using DOFHandler_t = DOFHandler<T, SpaceTraits>;
    using ElementType =
        std::conditional_t<Dim == 1, EdgeElement<T>,
                           std::conditional_t<Dim == 2, QuadrilateralElement<T>,
                                              HexahedralElement<T>>>;
    using indices_t = Vector<size_t, Dim>;
    using point_t   = Vector<T, Dim>;
    using vertex_points_t = typename ElementType::vertex_points_t;

    field = T(0);
    DOFHandler_t dofHandler(mesh, layout);
    ElementType refElement;
    LagrangeDOFLocations<T, Dim, Order> dofLocs;
    const auto elemIndices = dofHandler.getElementIndices();
    const int nghost       = field.getNghost();

    auto createMirrorViews = [&]<typename EntityType>() {
        auto view      = field.template getView<EntityType>();
        auto view_host = Kokkos::create_mirror_view(view);
        Kokkos::deep_copy(view_host, view);
        return view_host;
    };

    constexpr size_t numTypes = DOFHandler_t::numEntityTypes;
    auto mirror_views_tuple   = [&]<size_t... Is>(std::index_sequence<Is...>) {
        return std::make_tuple(
            createMirrorViews.template
            operator()<std::tuple_element_t<Is, typename SpaceTraits::EntityTypes>>()...);
    }(std::make_index_sequence<numTypes>{});

    const size_t numElements = elemIndices.extent(0);
    for (size_t elemIdx = 0; elemIdx < numElements; ++elemIdx) {
        const size_t elementIndex            = elemIndices(elemIdx);
        const indices_t localElementNDIndex  = dofHandler.getLocalElementNDIndex(elementIndex, nghost);
        const indices_t globalElementNDIndex = dofHandler.getElementNDIndex(elementIndex);

        vertex_points_t elementVertexPoints;
        constexpr size_t numVertices = ElementType::NumVertices;
        for (size_t v = 0; v < numVertices; ++v) {
            const auto vertexMapping = dofHandler.getElementDOFMapping(v);
            for (size_t d = 0; d < Dim; ++d) {
                const size_t vertexGlobalIndex =
                    globalElementNDIndex[d] + vertexMapping.entityLocalIndex[d];
                elementVertexPoints[v][d] =
                    vertexGlobalIndex * mesh.getMeshSpacing(d) + mesh.getOrigin()[d];
            }
        }

        for (size_t i = 0; i < SpaceTraits::dofsPerElement; ++i) {
            const auto dofMap = dofHandler.getElementDOFMapping(i);
            const point_t phys =
                refElement.localToGlobal(elementVertexPoints, dofLocs.locations[i]);
            const T val = evaluateSource<Src, Dim, T>(phys);

            auto writeEntity = [&]<size_t EntityIdx>() {
                using EntityType = std::tuple_element_t<EntityIdx, typename SpaceTraits::EntityTypes>;
                constexpr size_t dofStart =
                    DOFHandler_t::template getEntityDOFStart<EntityType>();
                constexpr size_t dofEnd = DOFHandler_t::template getEntityDOFEnd<EntityType>();
                if (i < dofStart || i >= dofEnd) {
                    return;
                }
                auto& view_host = std::get<EntityIdx>(mirror_views_tuple);
                apply(view_host, localElementNDIndex + dofMap.entityLocalIndex)[dofMap.entityLocalDOF] =
                    val;
            };

            [&]<size_t... Is>(std::index_sequence<Is...>) {
                (writeEntity.template operator()<Is>(), ...);
            }(std::make_index_sequence<numTypes>{});
        }
    }

    [&]<size_t... Is>(std::index_sequence<Is...>) {
        (
            [&] {
                using EntityType =
                    std::tuple_element_t<Is, typename SpaceTraits::EntityTypes>;
                Kokkos::deep_copy(field.template getView<EntityType>(),
                                  std::get<Is>(mirror_views_tuple));
            }(),
            ...);
    }(std::make_index_sequence<numTypes>{});

    field.fillHalo();
}

struct ConvergenceRow {
    const char* source   = "";
    unsigned order       = 0;
    unsigned quad_nodes  = 0;
    unsigned num_nodes   = 0;
    double h             = 0.0;
    double rel_l2        = 0.0;
    double cg_residue    = 0.0;
    int cg_iterations    = 0;
};

}  // namespace poisson_convergence_wfc
}  // namespace ippl

#endif
