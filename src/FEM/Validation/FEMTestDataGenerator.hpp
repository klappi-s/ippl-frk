#ifndef IPPL_FEM_VALIDATION_TESTDATAGENERATOR_HPP
#define IPPL_FEM_VALIDATION_TESTDATAGENERATOR_HPP

#include <cmath>
#include <stdexcept>

#include "FEM/Validation/FEMExplicitTools.h"
#include "PoissonSolvers/EvalFunctor.h"
#include "PoissonSolvers/FEMPoissonSolver_wFEMContainer.h"

namespace ippl::femdata {

template <unsigned Dim, unsigned Order, unsigned QuadNodes>
struct FemTestDataGenerator {
    static constexpr unsigned quad_nodes = QuadNodes;

    using T            = double;
    using ElementType  = std::conditional_t<
        Dim == 1, ippl::EdgeElement<T>,
        std::conditional_t<Dim == 2, ippl::QuadrilateralElement<T>, ippl::HexahedralElement<T>>>;
    using QuadratureType = ippl::GaussLegendreQuadrature<T, QuadNodes, ElementType>;
    using DOFHandler_t   = ippl::DOFHandler<T, ippl::FiniteElementSpaceTraits<ippl::LagrangeSpaceTag, Dim, Order>>;
    using FieldType      = typename DOFHandler_t::FEMContainer_t;
    using LagrangeType   = ippl::LagrangeSpace_wfc<T, Dim, Order, ElementType, QuadratureType, FieldType, FieldType>;
    using point_t        = ippl::Vector<T, Dim>;
    static constexpr unsigned nloc = LagrangeType::numElementDOFs;

    static std::size_t schema_num_global_dofs(const std::array<unsigned, Dim>& nel) {
        std::size_t n = 1;
        for (unsigned d = 0; d < Dim; ++d) {
            n *= static_cast<std::size_t>(nel[d] * Order + 1);
        }
        return n;
    }

    static void generate(const std::array<unsigned, Dim>& nel, const std::array<double, Dim>& origin,
                         const std::array<double, Dim>& corner, const std::string& outdir,
                         const std::function<double(const point_t&)>& source, double boundary_value,
                         SolveMode solve_mode = SolveMode::Explicit) {
        std::filesystem::create_directories(outdir);

        std::array<unsigned, Dim> nodesPerDim{};
        ippl::Vector<unsigned, Dim> nodesVec{};
        ippl::Vector<T, Dim> cellSpacing{};
        ippl::Vector<T, Dim> originVec{};
        for (unsigned d = 0; d < Dim; ++d) {
            nodesPerDim[d] = nel[d] + 1;
            nodesVec[d]    = nodesPerDim[d];
            cellSpacing[d] = (corner[d] - origin[d]) / static_cast<T>(nel[d]);
            originVec[d]   = origin[d];
        }

        ippl::NDIndex<Dim> domain(nodesVec);
        using Mesh_t = ippl::UniformCartesian<T, Dim>;
        Mesh_t mesh(domain, cellSpacing, originVec);

        std::array<bool, Dim> isParallel{};
        isParallel.fill(true);
        ippl::FieldLayout<Dim> layout(MPI_COMM_WORLD, domain, isParallel);

        ElementType ref_element;
        const QuadratureType quadrature(ref_element);
        LagrangeType lagrange_space(mesh, ref_element, quadrature, layout);

        const auto weights = quadrature.getWeightsForRefElement();
        const auto qpts    = quadrature.getIntegrationNodesForRefElement();

        const std::size_t ndofs = lagrange_space.numGlobalDOFs();

        std::vector<double> f_dof(ndofs, 0.0);
        for (std::size_t g = 0; g < ndofs; ++g) {
            f_dof[g] = source(detail::dof_coord<Dim>(g, nel, Order, origin, corner));
        }

        std::vector<std::vector<double>> A_ref(nloc, std::vector<double>(nloc, 0.0));
        std::vector<double> l_ref(nloc, 0.0);
        const ippl::Vector<T, Dim> DPhiInvT_ref(1.0);
        const T absDet_ref = 1.0;
        EvalFunctor<T, Dim, nloc> eval_ref(DPhiInvT_ref, absDet_ref);

        std::vector<ippl::Vector<point_t, nloc>> grad_q(QuadratureType::numElementNodes);
        for (unsigned k = 0; k < QuadratureType::numElementNodes; ++k) {
            for (unsigned i = 0; i < nloc; ++i) {
                grad_q[k][i] = lagrange_space.evaluateRefElementShapeFunctionGradient(i, qpts[k]);
            }
        }
        for (unsigned i = 0; i < nloc; ++i) {
            for (unsigned j = 0; j < nloc; ++j) {
                for (unsigned k = 0; k < QuadratureType::numElementNodes; ++k) {
                    A_ref[i][j] += weights[k] * eval_ref(i, j, grad_q[k]);
                }
            }
            for (unsigned k = 0; k < QuadratureType::numElementNodes; ++k) {
                const double phi_i = lagrange_space.evaluateRefElementShapeFunction(i, qpts[k]);
                l_ref[i] += weights[k] * phi_i * absDet_ref;
            }
        }

        std::vector<LocalCellBlock> local_cells;
        std::vector<std::vector<double>> A_global(ndofs, std::vector<double>(ndofs, 0.0));
        std::vector<double> l_global(ndofs, 0.0);

        const auto elemIndices = lagrange_space.getDOFHandler().getElementIndices();
        auto elem_host         = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), elemIndices);
        local_cells.reserve(elem_host.extent(0));

        for (std::size_t ei = 0; ei < elem_host.extent(0); ++ei) {
            const std::size_t e = elem_host(ei);
            const auto elem_nd = lagrange_space.getElementNDIndex(e);
            const auto verts   = lagrange_space.getElementMeshVertexPoints(elem_nd);
            const auto DPhiInvT =
                ref_element.getInverseTransposeTransformationJacobian(verts);
            const T absDet =
                std::abs(ref_element.getDeterminantOfTransformationJacobian(verts));
            EvalFunctor<T, Dim, nloc> eval_phys(DPhiInvT, absDet);

            std::vector<std::vector<double>> A_e(nloc, std::vector<double>(nloc, 0.0));
            std::vector<double> l_e(nloc, 0.0);

            const auto g_dofs0 = lagrange_space.getGlobalDOFIndices(e);
            std::vector<std::size_t> g_dofs_1(nloc);
            std::vector<double> f_cell(nloc);
            for (unsigned i = 0; i < nloc; ++i) {
                g_dofs_1[i] = g_dofs0[i] + 1;
                if (g_dofs0[i] >= ndofs) {
                    throw std::runtime_error(
                        "FEMTestDataGenerator: global DOF index out of SCHEMA range");
                }
                f_cell[i] = f_dof[g_dofs0[i]];
            }

            for (unsigned k = 0; k < QuadratureType::numElementNodes; ++k) {
                double f_q = 0.0;
                for (unsigned j = 0; j < nloc; ++j) {
                    f_q += lagrange_space.evaluateRefElementShapeFunction(j, qpts[k]) * f_cell[j];
                }
                for (unsigned i = 0; i < nloc; ++i) {
                    const double phi_i = lagrange_space.evaluateRefElementShapeFunction(i, qpts[k]);
                    l_e[i] += f_q * phi_i * weights[k] * absDet;
                    for (unsigned j = 0; j < nloc; ++j) {
                        A_e[i][j] += weights[k] * eval_phys(i, j, grad_q[k]);
                    }
                }
            }

            for (unsigned i = 0; i < nloc; ++i) {
                const std::size_t gi = g_dofs0[i];
                l_global[gi] += l_e[i];
                for (unsigned j = 0; j < nloc; ++j) {
                    A_global[gi][g_dofs0[j]] += A_e[i][j];
                }
            }

            LocalCellBlock block;
            block.global_dofs_1based = g_dofs_1;
            block.matrix               = A_e;
            block.vector               = l_e;
            local_cells.push_back(std::move(block));
        }

        std::vector<std::size_t> constrained;
        std::vector<std::pair<std::size_t, double>> constrained_pairs;
        for (std::size_t g = 0; g < ndofs; ++g) {
            if (detail::is_boundary_dof<Dim>(g, Order, nel)) {
                constrained.push_back(g);
                constrained_pairs.emplace_back(g + 1, boundary_value);
            }
        }

        std::vector<double> u;
        if (solve_mode == SolveMode::CG) {
            FieldType lhs(mesh, layout, 1);
            FieldType rhs(mesh, layout, 1);
            std::array<ippl::FieldBC, 2 * Dim> bc{};
            bc.fill(ippl::ZERO_FACE);
            lhs.setFieldBC(bc);
            rhs.setFieldBC(bc);

            femvalidate::scatterGlobalPattern(rhs, lagrange_space, [&](std::size_t g) {
                return source(detail::dof_coord<Dim>(g, nel, Order, origin, corner));
            });
            rhs.fillHalo();

            FEMPoissonSolver_wFEMContainer<FieldType, FieldType, Order, QuadNodes> solver(lhs, rhs);
            solver.solve();
            u = femvalidate::detail::gatherFieldToGlobal(lagrange_space, lhs);
        } else {
            u = detail::solve_dirichlet(A_global, l_global, constrained, boundary_value);
        }

        write_dense_matrix(outdir + "/reference_stiffness_matrix.txt", A_ref,
                           "Reference-element stiffness matrix A (before BCs)");
        write_vector(outdir + "/reference_load_vector.txt", l_ref,
                     "Reference-element load vector for f=1 (before BCs)");
        write_local_stiffness_matrices(
            outdir + "/local_stiffness_matrices.txt", local_cells, Dim, Order,
            "Per-element stiffness matrices A_e on physical cells (IPPL cell and DOF ordering)");
        write_local_load_vectors(outdir + "/local_load_vectors.txt", local_cells, Dim, Order,
                                 "Per-element load vectors l_e on physical cells (IPPL cell and DOF ordering)");
        write_vector(outdir + "/global_load_vector.txt", l_global,
                     "Global load vector l before BCs (IPPL DOF ordering)");
        write_vector(outdir + "/solution_vector_u.txt", u,
                     solve_mode == SolveMode::CG
                         ? "Global solution u from Track B CG (IPPL DOF ordering)"
                         : "Global solution u after Dirichlet BCs and linear solve (IPPL DOF ordering)");
        write_constrained_dofs(
            outdir + "/constrained_dofs.txt", constrained_pairs,
            "Dirichlet-constrained global DOFs and prescribed values (IPPL 1-based ordering)");
    }
};

}  // namespace ippl::femdata

#endif
