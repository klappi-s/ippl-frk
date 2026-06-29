#ifndef IPPL_FEM_VALIDATION_MATRIXFREECOMPARISON_HPP
#define IPPL_FEM_VALIDATION_MATRIXFREECOMPARISON_HPP

#include <cmath>

namespace ippl::femvalidate {

template <unsigned Dim, unsigned Order, unsigned QuadNodes>
double relativeErrorEvaluateAxVsExplicit(const std::function<double(std::size_t)>& pattern,
                                         double rtol_scale) {
    using Ctx = CoreGridContext<Dim, Order, QuadNodes>;
    using FieldType = typename Ctx::FieldType;

    Ctx ctx;
    const auto eval = makePoissonEvalFunctor(ctx.space, ctx.ref_element);
    const auto cell_A =
        assembleCellStiffnessMatrices(ctx.space, ctx.ref_element, ctx.quadrature);
    const auto A_global = detail::assembleGlobalStiffness(ctx.space, cell_A);

    std::array<unsigned, Dim> nel{};
    for (unsigned d = 0; d < Dim; ++d) {
        nel[d] = Ctx::nel_core;
    }

    const std::size_t ndofs = ctx.space.numGlobalDOFs();
    std::vector<double> v_g(ndofs);
    for (std::size_t g = 0; g < ndofs; ++g) {
        v_g[g] = pattern(g);
    }
    const std::vector<double> y_ex_g = detail::applyZeroFaceGlobal<Dim, Order>(A_global, v_g, nel);

    FieldType v(ctx.mesh, ctx.layout, 1);
    std::array<ippl::FieldBC, 2 * Dim> bc{};
    bc.fill(ippl::ZERO_FACE);
    v.setFieldBC(bc);
    scatterGlobalPattern(v, ctx.space, pattern);
    v.fillHalo();

    FieldType y_mf = ctx.space.evaluateAx(v, eval);
    y_mf.fillHalo();

    FieldType y_ex(ctx.mesh, ctx.layout, 1);
    y_ex.setFieldBC(bc);
    scatterGlobalPattern(y_ex, ctx.space, [&](std::size_t g) { return y_ex_g[g]; });
    y_ex.fillHalo();

    return detail::relativeFieldNormError(ctx.space, y_mf, y_ex) / rtol_scale;
}

template <unsigned Dim, unsigned Order, unsigned QuadNodes>
std::vector<double> assembleExplicitGlobalLoad(
    const std::function<double(const ippl::Vector<double, Dim>&)>& source) {
    using Ctx = CoreGridContext<Dim, Order, QuadNodes>;

    Ctx ctx;
    using T = double;
    static constexpr unsigned nloc = Ctx::LagrangeType::numElementDOFs;
    using QuadratureType = typename Ctx::QuadratureType;

    const std::size_t ndofs = ctx.space.numGlobalDOFs();
    std::array<unsigned, Dim> nel{};
    for (unsigned d = 0; d < Dim; ++d) {
        nel[d] = Ctx::nel_core;
    }
    std::array<double, Dim> origin{};
    std::array<double, Dim> corner{};
    for (unsigned d = 0; d < Dim; ++d) {
        origin[d] = 0.0;
        corner[d] = 1.0;
    }

    std::vector<double> f_dof(ndofs, 0.0);
    for (std::size_t g = 0; g < ndofs; ++g) {
        f_dof[g] = source(ippl::femdata::detail::dof_coord<Dim>(g, nel, Order, origin, corner));
    }

    const auto weights = ctx.quadrature.getWeightsForRefElement();
    const auto qpts    = ctx.quadrature.getIntegrationNodesForRefElement();

    std::vector<double> l_global(ndofs, 0.0);

    const auto owned_elements = ownedElementIndicesHost(ctx.space);
    for (const std::size_t e : owned_elements) {
        const auto elem_nd = ctx.space.getElementNDIndex(e);
        const auto verts   = ctx.space.getElementMeshVertexPoints(elem_nd);
        const T absDet =
            Kokkos::abs(ctx.ref_element.getDeterminantOfTransformationJacobian(verts));

        std::vector<double> l_e(nloc, 0.0);
        const auto g_dofs0 = ctx.space.getGlobalDOFIndices(e);
        std::vector<double> f_cell(nloc);
        for (unsigned i = 0; i < nloc; ++i) {
            f_cell[i] = f_dof[g_dofs0[i]];
        }

        for (unsigned k = 0; k < QuadratureType::numElementNodes; ++k) {
            double f_q = 0.0;
            for (unsigned j = 0; j < nloc; ++j) {
                f_q += ctx.space.evaluateRefElementShapeFunction(j, qpts[k]) * f_cell[j];
            }
            for (unsigned i = 0; i < nloc; ++i) {
                const double phi_i = ctx.space.evaluateRefElementShapeFunction(i, qpts[k]);
                l_e[i] += f_q * phi_i * weights[k] * absDet;
            }
        }

        for (unsigned i = 0; i < nloc; ++i) {
            l_global[g_dofs0[i]] += l_e[i];
        }
    }

    if (ippl::Comm->size() > 1) {
        MPI_Allreduce(MPI_IN_PLACE, l_global.data(), static_cast<int>(l_global.size()), MPI_DOUBLE,
                      MPI_SUM, MPI_COMM_WORLD);
    }
    return l_global;
}

template <unsigned Dim, unsigned Order, unsigned QuadNodes>
double relativeErrorEvaluateLoadVectorVsExplicit(
    const std::function<double(const ippl::Vector<double, Dim>&)>& source,
    bool interior_only) {
    using Ctx = CoreGridContext<Dim, Order, QuadNodes>;
    using FieldType = typename Ctx::FieldType;

    Ctx ctx;
    const std::vector<double> l_ex = assembleExplicitGlobalLoad<Dim, Order, QuadNodes>(source);

    std::array<unsigned, Dim> nel{};
    for (unsigned d = 0; d < Dim; ++d) {
        nel[d] = Ctx::nel_core;
    }
    std::array<double, Dim> origin{};
    std::array<double, Dim> corner{};
    for (unsigned d = 0; d < Dim; ++d) {
        origin[d] = 0.0;
        corner[d] = 1.0;
    }

    FieldType rhs(ctx.mesh, ctx.layout, 1);
    std::array<ippl::FieldBC, 2 * Dim> bc{};
    bc.fill(ippl::ZERO_FACE);
    rhs.setFieldBC(bc);

    scatterGlobalPattern(rhs, ctx.space, [&](std::size_t g) {
        return source(ippl::femdata::detail::dof_coord<Dim>(g, nel, Order, origin, corner));
    });
    rhs.fillHalo();

    ctx.space.evaluateLoadVector(rhs);
    rhs.fillHalo();

    FieldType l_ex_field(ctx.mesh, ctx.layout, 1);
    l_ex_field.setFieldBC(bc);
    scatterGlobalPattern(l_ex_field, ctx.space, [&](std::size_t g) {
        if (interior_only && ippl::femdata::detail::is_boundary_dof<Dim>(g, Order, nel)) {
            return 0.0;
        }
        return l_ex[g];
    });
    l_ex_field.fillHalo();

    return detail::relativeFieldNormError(ctx.space, rhs, l_ex_field);
}

template <unsigned Dim, unsigned Order, unsigned QuadNodes>
std::vector<double> solveExplicitCorePoisson(
    const std::function<double(const ippl::Vector<double, Dim>&)>& source, double boundary_value) {
    using Ctx = CoreGridContext<Dim, Order, QuadNodes>;

    Ctx ctx;
    using T = double;
    static constexpr unsigned nloc = Ctx::LagrangeType::numElementDOFs;
    using QuadratureType = typename Ctx::QuadratureType;

    const std::size_t ndofs = ctx.space.numGlobalDOFs();
    std::array<unsigned, Dim> nel{};
    for (unsigned d = 0; d < Dim; ++d) {
        nel[d] = Ctx::nel_core;
    }

    const std::vector<double> l_global = assembleExplicitGlobalLoad<Dim, Order, QuadNodes>(source);

    const auto weights = ctx.quadrature.getWeightsForRefElement();
    const auto qpts    = ctx.quadrature.getIntegrationNodesForRefElement();

    std::vector<ippl::Vector<ippl::Vector<T, Dim>, nloc>> grad_q(QuadratureType::numElementNodes);
    for (unsigned k = 0; k < QuadratureType::numElementNodes; ++k) {
        for (unsigned i = 0; i < nloc; ++i) {
            grad_q[k][i] = ctx.space.evaluateRefElementShapeFunctionGradient(i, qpts[k]);
        }
    }

    std::vector<std::vector<double>> A_global(ndofs, std::vector<double>(ndofs, 0.0));

    const auto owned_elements = ownedElementIndicesHost(ctx.space);
    for (const std::size_t e : owned_elements) {
        const auto elem_nd = ctx.space.getElementNDIndex(e);
        const auto verts   = ctx.space.getElementMeshVertexPoints(elem_nd);
        const auto DPhiInvT =
            ctx.ref_element.getInverseTransposeTransformationJacobian(verts);
        const T absDet =
            Kokkos::abs(ctx.ref_element.getDeterminantOfTransformationJacobian(verts));
        EvalFunctor<T, Dim, nloc> eval_phys(DPhiInvT, absDet);

        std::vector<std::vector<double>> A_e(nloc, std::vector<double>(nloc, 0.0));
        const auto g_dofs0 = ctx.space.getGlobalDOFIndices(e);

        for (unsigned k = 0; k < QuadratureType::numElementNodes; ++k) {
            for (unsigned i = 0; i < nloc; ++i) {
                for (unsigned j = 0; j < nloc; ++j) {
                    A_e[i][j] += weights[k] * eval_phys(i, j, grad_q[k]);
                }
            }
        }

        for (unsigned i = 0; i < nloc; ++i) {
            const std::size_t gi = g_dofs0[i];
            for (unsigned j = 0; j < nloc; ++j) {
                A_global[gi][g_dofs0[j]] += A_e[i][j];
            }
        }
    }

    if (ippl::Comm->size() > 1) {
        std::vector<double> flat_A(ndofs * ndofs, 0.0);
        for (std::size_t i = 0; i < ndofs; ++i) {
            for (std::size_t j = 0; j < ndofs; ++j) {
                flat_A[i * ndofs + j] = A_global[i][j];
            }
        }
        MPI_Allreduce(MPI_IN_PLACE, flat_A.data(), static_cast<int>(flat_A.size()), MPI_DOUBLE, MPI_SUM,
                      MPI_COMM_WORLD);
        for (std::size_t i = 0; i < ndofs; ++i) {
            for (std::size_t j = 0; j < ndofs; ++j) {
                A_global[i][j] = flat_A[i * ndofs + j];
            }
        }
    }

    std::vector<std::size_t> constrained;
    for (std::size_t g = 0; g < ndofs; ++g) {
        if (ippl::femdata::detail::is_boundary_dof<Dim>(g, Order, nel)) {
            constrained.push_back(g);
        }
    }

    return ippl::femdata::detail::solve_dirichlet(A_global, l_global, constrained, boundary_value);
}

template <unsigned Dim, unsigned Order, unsigned QuadNodes>
double relativeExplicitSolutionResidual(
    const std::function<double(const ippl::Vector<double, Dim>&)>& source,
    bool interior_only) {
    using Ctx = CoreGridContext<Dim, Order, QuadNodes>;

    Ctx ctx;
    const auto cell_A =
        assembleCellStiffnessMatrices(ctx.space, ctx.ref_element, ctx.quadrature);
    const auto A_global = detail::assembleGlobalStiffness(ctx.space, cell_A);
    const std::vector<double> l_global = assembleExplicitGlobalLoad<Dim, Order, QuadNodes>(source);
    const std::vector<double> u_global =
        solveExplicitCorePoisson<Dim, Order, QuadNodes>(source, 0.0);

    std::array<unsigned, Dim> nel{};
    for (unsigned d = 0; d < Dim; ++d) {
        nel[d] = Ctx::nel_core;
    }

    const std::vector<double> Ku =
        detail::applyZeroFaceGlobal<Dim, Order>(A_global, u_global, nel);

    double err2 = 0.0;
    double ref2 = 0.0;
    for (std::size_t g = 0; g < l_global.size(); ++g) {
        if (interior_only && ippl::femdata::detail::is_boundary_dof<Dim>(g, Order, nel)) {
            continue;
        }
        const double d = Ku[g] - l_global[g];
        err2 += d * d;
        ref2 += l_global[g] * l_global[g];
    }
    return std::sqrt(err2) / std::max(std::sqrt(ref2), 1e-30);
}

template <unsigned Dim, unsigned Order, unsigned QuadNodes>
double relativeGlobalVectorScatterGatherError(const std::vector<double>& u_global) {
    using Ctx = CoreGridContext<Dim, Order, QuadNodes>;
    using FieldType = typename Ctx::FieldType;

    Ctx ctx;
    FieldType u(ctx.mesh, ctx.layout, 1);
    std::array<ippl::FieldBC, 2 * Dim> bc{};
    bc.fill(ippl::ZERO_FACE);
    u.setFieldBC(bc);

    scatterGlobalPattern(u, ctx.space, [&](std::size_t g) { return u_global[g]; });
    u.fillHalo();

    FieldType u_ref(ctx.mesh, ctx.layout, 1);
    u_ref.setFieldBC(bc);
    scatterGlobalPattern(u_ref, ctx.space, [&](std::size_t g) { return u_global[g]; });
    u_ref.fillHalo();

    return detail::relativeFieldNormError(ctx.space, u, u_ref);
}

template <unsigned Dim, unsigned Order, unsigned QuadNodes>
double relativeMatrixFreeResidualAtExplicitSolution(
    const std::function<double(const ippl::Vector<double, Dim>&)>& source) {
    using Ctx = CoreGridContext<Dim, Order, QuadNodes>;
    using FieldType = typename Ctx::FieldType;

    Ctx ctx;
    const std::vector<double> u_global =
        solveExplicitCorePoisson<Dim, Order, QuadNodes>(source, 0.0);

    std::array<unsigned, Dim> nel{};
    for (unsigned d = 0; d < Dim; ++d) {
        nel[d] = Ctx::nel_core;
    }

    FieldType u(ctx.mesh, ctx.layout, 1);
    FieldType rhs(ctx.mesh, ctx.layout, 1);
    std::array<ippl::FieldBC, 2 * Dim> bc{};
    bc.fill(ippl::ZERO_FACE);
    u.setFieldBC(bc);
    rhs.setFieldBC(bc);

    scatterGlobalPattern(u, ctx.space, [&](std::size_t g) { return u_global[g]; });
    u.fillHalo();

    std::array<double, Dim> origin{};
    std::array<double, Dim> corner{};
    for (unsigned d = 0; d < Dim; ++d) {
        origin[d] = 0.0;
        corner[d] = 1.0;
    }
    scatterGlobalPattern(rhs, ctx.space, [&](std::size_t g) {
        return source(ippl::femdata::detail::dof_coord<Dim>(g, nel, Order, origin, corner));
    });
    rhs.fillHalo();

    ctx.space.evaluateLoadVector(rhs);
    rhs.fillHalo();

    const auto eval = makePoissonEvalFunctor(ctx.space, ctx.ref_element);
    FieldType Au = ctx.space.evaluateAx(u, eval);
    Au.fillHalo();

    return detail::relativeFieldNormError(ctx.space, Au, rhs);
}

template <typename LagrangeType, typename ElementType, typename FieldType>
double relativeSolutionOperatorResidual(const LagrangeType& space, const ElementType& ref_element,
                                        const FieldType& u_field, const FieldType& load_field) {
    static constexpr unsigned Dim   = LagrangeType::dim;
    static constexpr unsigned Order = LagrangeType::order;

    const auto eval = makePoissonEvalFunctor(space, ref_element);
    FieldType Au    = space.evaluateAx(const_cast<FieldType&>(u_field), eval);
    Au.fillHalo();

    return detail::relativeFieldNormError(space, Au, load_field);
}

template <typename LagrangeType, typename FieldType>
double globalSolutionError(const LagrangeType& space, const FieldType& u_field,
                           const std::vector<double>& u_global) {
    FieldType u_ref(u_field.get_mesh(), u_field.getLayout(), u_field.getNghost());
    u_ref.setFieldBC(u_field.getFieldBCTypes());
    scatterGlobalPattern(u_ref, space, [&](std::size_t g) { return u_global[g]; });
    u_ref.fillHalo();
    return detail::relativeFieldNormError(space, u_field, u_ref);
}
}  // namespace ippl::femvalidate

#endif
