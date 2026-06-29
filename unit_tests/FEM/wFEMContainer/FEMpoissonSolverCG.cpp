// Matrix-free CG Poisson solve on core grid vs explicit dense reference (M5c).

#include "Ippl.h"

#include "FEM/Validation/FEMMatrixFreeComparison.h"
#include "PoissonSolvers/FEMPoissonSolver_wFEMContainer.h"
#include "gtest/gtest.h"

namespace {

template <unsigned Dim, unsigned Order>
void testCgVsExplicit() {
    using Ctx = ippl::femvalidate::CoreGridContext<Dim, Order, 9>;
    using FieldType = typename Ctx::FieldType;

    Ctx ctx;

    auto source = [](const ippl::Vector<double, Dim>& x) -> double {
        if constexpr (Dim == 1) {
            return 2.0 * x[0];
        } else if constexpr (Dim == 2) {
            return x[0] + x[1];
        } else {
            return x[0] + x[1] + x[2];
        }
    };

    const auto u_explicit =
        ippl::femvalidate::solveExplicitCorePoisson<Dim, Order, 9>(source, 0.0);

    FieldType lhs(ctx.mesh, ctx.layout, 1);
    FieldType rhs(ctx.mesh, ctx.layout, 1);

    std::array<ippl::FieldBC, 2 * Dim> bc{};
    bc.fill(ippl::ZERO_FACE);
    lhs.setFieldBC(bc);
    rhs.setFieldBC(bc);

    ippl::femvalidate::scatterGlobalPattern(
        rhs, ctx.space, [&](std::size_t g) {
            std::array<unsigned, Dim> nel{};
            std::array<double, Dim> origin{};
            std::array<double, Dim> corner{};
            for (unsigned d = 0; d < Dim; ++d) {
                nel[d]    = Ctx::nel_core;
                origin[d] = 0.0;
                corner[d] = 1.0;
            }
            return source(ippl::femdata::detail::dof_coord<Dim>(g, nel, Order, origin, corner));
        });

    ippl::FEMPoissonSolver_wFEMContainer<FieldType, FieldType, Order, 9> solver(lhs, rhs);

    ippl::ParameterList params;
    params.add("tolerance", 1e-12);
    params.add("max_iterations", 4000);
    solver.mergeParameters(params);
    solver.solve();

    lhs.fillHalo();

    const double cg_op_res = ippl::femvalidate::relativeSolutionOperatorResidual(
        solver.getSpace(), ctx.ref_element, lhs, rhs);
    EXPECT_LT(cg_op_res, 1e-9)
        << "CG operator residual dim=" << Dim << " rel=" << cg_op_res
        << " iters=" << solver.getIterationCount() << " residue=" << solver.getResidue();

    const double rel =
        ippl::femvalidate::globalSolutionError(ctx.space, lhs, u_explicit);
    EXPECT_LT(rel, 1e-9) << "dim=" << Dim << " order=" << Order << " rel=" << rel;
}

}  // namespace

#define CORE_GRID_POISSON_CASE(dim, order) \
    TEST(CoreGridPoisson_wFEMContainer, Dim##dim##Order##order) { testCgVsExplicit<dim, order>(); }

CORE_GRID_POISSON_CASE(1, 1)
CORE_GRID_POISSON_CASE(1, 2)
CORE_GRID_POISSON_CASE(1, 3)
CORE_GRID_POISSON_CASE(2, 1)
CORE_GRID_POISSON_CASE(2, 2)
CORE_GRID_POISSON_CASE(2, 3)
CORE_GRID_POISSON_CASE(3, 1)
CORE_GRID_POISSON_CASE(3, 2)
CORE_GRID_POISSON_CASE(3, 3)
// Stiffness + load vs explicit: MatrixFreeVsExplicit (serial). CG operator residual checked above.

int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    ippl::finalize();
    return rc;
}
