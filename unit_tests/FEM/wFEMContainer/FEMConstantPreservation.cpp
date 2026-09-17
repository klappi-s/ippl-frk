#include "Ippl.h"
#include "PoissonSolvers/FEMPoissonSolver_wFEMContainer.h"
#include "gtest/gtest.h"

namespace {
using Handler = ippl::LagrangeDOFHandler<double, 1, 3>;
using Field = Handler::FEMContainer_t;
using Solver = ippl::FEMPoissonSolver_wFEMContainer<Field, Field, 3, 9>;

struct Problem {
    ippl::NDIndex<1> domain;
    ippl::FieldLayout<1> layout;
    ippl::UniformCartesian<double, 1> mesh;
    Field lhs, rhs;
    Solver solver;

    Problem(unsigned vertices, const char* family)
        : domain(ippl::Vector<unsigned, 1>(vertices))
        , layout(MPI_COMM_WORLD, domain, std::array<bool, 1>{true})
        , mesh(domain, ippl::Vector<double, 1>(1.0 / (vertices - 1)), ippl::Vector<double, 1>(0))
        , lhs(mesh, layout, 1), rhs(mesh, layout, 1), solver(lhs, rhs) {
        lhs.setFieldBC(std::array<ippl::FieldBC, 2>{ippl::ZERO_FACE, ippl::ZERO_FACE});
        rhs.setFieldBC(std::array<ippl::FieldBC, 2>{ippl::NO_FACE, ippl::NO_FACE});
        ippl::ParameterList parameters;
        parameters.add("interpolation_nodes", std::string(family));
        parameters.add("preconditioned", false);
        parameters.add("tolerance", 1e-13);
        parameters.add("max_iterations", 30000);
        solver.mergeParameters(parameters);
        solver.configureNodeFamilies();
    }
};
} // namespace

TEST(FEMConstantPreservation, RawActionIsInvariantUnderConstantShift) {
    for (const auto* family : {"gll", "equispaced"}) {
        SCOPED_TRACE(family);
        Problem p(5, family);
        auto& space = p.solver.getSpace();
        p.rhs = 0;
        Kokkos::View<double*> coefficients("shift probe", space.numGlobalDOFs());
        auto host = Kokkos::create_mirror_view(coefficients);
        for (size_t i = 0; i < host.extent(0); ++i) host(i) = (i * i % 17) * 0.125;
        Kokkos::deep_copy(coefficients, host);
        space.fillFromGlobalCoefficients(p.rhs, coefficients);
        p.rhs.fillHalo();
        ippl::EvalFunctor<double, 1, Handler::dofsPerElement> eval(
            ippl::Vector<double, 1>(4), 0.25);
        auto before = space.evaluateAx(p.rhs, eval);
        p.rhs = p.rhs + 2.0;
        p.rhs.fillHalo();
        auto after = space.evaluateAx(p.rhs, eval);
        auto difference = after.deepCopy();
        difference = after - before;
        EXPECT_LE(ippl::norm(difference), 2e-13 * ippl::norm(before));
    }
}

TEST(FEMConstantPreservation, DefaultModeFineMeshQuadraticAccuracy) {
    for (const auto* family : {"gll", "equispaced"}) {
        SCOPED_TRACE(family);
        Problem p(1024, family);
        ASSERT_EQ(p.solver.getStiffnessMode(), ippl::PoissonStiffnessMode::ConstantPreserving);
        p.lhs = 0;
        p.rhs = 2; // -u'' = 2, u=x*(1-x), including source samples on the boundary.
        p.solver.solve();
        const auto exact = [](const ippl::Vector<double, 1>& x) { return x[0] * (1 - x[0]); };
        EXPECT_LT(p.solver.getL2Error(exact), 1e-12);
        EXPECT_LT(p.solver.getIterationCount(), 30000);
    }
}

TEST(FEMConstantPreservation, StandardRemainsExplicitlySelectable) {
    Problem p(5, "gll");
    ippl::ParameterList parameters;
    parameters.add("poisson_stiffness_mode", std::string("standard"));
    p.solver.mergeParameters(parameters);
    ASSERT_EQ(p.solver.getStiffnessMode(), ippl::PoissonStiffnessMode::Standard);
    p.lhs = 0;
    p.rhs = 2;
    p.solver.solve();
    const auto exact = [](const ippl::Vector<double, 1>& x) { return x[0] * (1 - x[0]); };
    EXPECT_LT(p.solver.getL2Error(exact), 1e-12);
}

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
