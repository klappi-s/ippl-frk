#include "Ippl.h"

#include "PoissonSolvers/FEMPoissonSolver_wFEMContainer.h"
#include "gtest/gtest.h"

namespace {
    using Handler = ippl::LagrangeDOFHandler<double, 1, 3>;
    using Field   = Handler::FEMContainer_t;
    using Solver  = ippl::FEMPoissonSolver_wFEMContainer<Field, Field, 3, 4>;
    struct Problem {
        ippl::NDIndex<1> domain;
        ippl::FieldLayout<1> layout;
        ippl::UniformCartesian<double, 1> mesh;
        Field x, b;
        Solver solver;
        explicit Problem(unsigned cells = 8)
            : domain(ippl::Vector<unsigned, 1>(cells + 1))
            , layout(MPI_COMM_WORLD, domain, std::array<bool, 1>{true}, true)
            , mesh(domain, ippl::Vector<double, 1>(1.0 / cells), ippl::Vector<double, 1>(0))
            , x(mesh, layout)
            , b(mesh, layout)
            , solver(x, b) {
            const std::array<ippl::FieldBC, 2> bc{ippl::PERIODIC_FACE, ippl::PERIODIC_FACE};
            x.setFieldBC(bc);
            b.setFieldBC(bc);
            x = 0;
            ippl::ParameterList p;
            p.add("fem_reuse", std::string("full"));
            p.add("tolerance", 1e-12);
            solver.mergeParameters(p);
            solver.configureNodeFamilies();
        }
        void fill(Field& f, double scale = 1) {
            Kokkos::View<double*> values("cache probe", solver.getSpace().numGlobalDOFs());
            auto host = Kokkos::create_mirror_view(values);
            for (size_t i = 0; i < host.extent(0); ++i)
                host(i) = scale * std::sin(0.7 * i);
            Kokkos::deep_copy(values, host);
            solver.getSpace().fillFromGlobalCoefficients(f, values);
            f.fillHalo();
        }
        Field makeLoad() {
            fill(x);
            const double h = mesh.getMeshSpacing()[0];
            ippl::EvalFunctor<double, 1, 4> eval(ippl::Vector<double, 1>(1 / h), h,
                                                 solver.getStiffnessMode());
            return solver.getSpace().evaluateAx(x, eval);
        }
    };
}  // namespace
TEST(FEMReuse, CopiedContainerOwnsItsEntityLayouts) {
    Problem p;
    auto source = std::make_unique<Field>(p.x);
    p.fill(*source);
    auto copy = std::make_unique<Field>(*source);
    source.reset();  // Halo exchange must not consult the destroyed source's layouts.
    copy->fillHalo();
    p.fill(p.b);
    EXPECT_DOUBLE_EQ(ippl::norm(*copy - p.b), 0);
    *copy = *copy;
    copy->fillHalo();
    EXPECT_DOUBLE_EQ(ippl::norm(*copy - p.b), 0);
}
TEST(FEMReuse, ChangingLoadsMatchRebuildAndParametersInvalidate) {
    Problem p;
    auto load = p.makeLoad();
    ippl::ParameterList options;
    for (const std::string method : {"plain", "jacobi", "chebyshev", "gauss-seidel"}) {
        options = ippl::ParameterList{};
        options.add("preconditioned", method != "plain");
        options.add("preconditioner_type", method == "plain" ? std::string("jacobi") : method);
        options.add("chebyshev_degree", 8);
        p.solver.mergeParameters(options);
        std::vector<Field> reference;
        std::vector<int> iterations;
        for (const std::string mode : {"off", "operator", "full"}) {
            ippl::ParameterList reuse;
            reuse.add("fem_reuse", mode);
            p.solver.mergeParameters(reuse);
            p.solver.invalidatePreparedData();
            for (unsigned i = 0; i < 3; ++i) {
                auto rhs = load.deepCopy();
                rhs      = rhs * (1.0 + 0.2 * i);
                p.x      = 0;
                p.solver.solveLoadVector(rhs);
                const auto stats = p.solver.getSolveStatistics();
                EXPECT_EQ(stats.operatorSetups, mode == "off" || i == 0);
                EXPECT_EQ(stats.preconditionerSetups,
                          method != "plain" && (mode != "full" || i == 0));
                EXPECT_LT(ippl::norm(rhs - p.solver.applyOperator(p.x)), 3e-11 * ippl::norm(rhs));
                if (mode == "off") {
                    reference.push_back(p.x.deepCopy());
                    iterations.push_back(p.solver.getIterationCount());
                } else {
                    EXPECT_LT(ippl::norm(p.x - reference[i]), 1e-9 * ippl::norm(reference[i]));
                    EXPECT_EQ(p.solver.getIterationCount(), iterations[i]);
                }
            }
        }
    }
    auto checkRebuild = [&](bool op, bool pre) {
        p.x = 0;
        p.solver.solveLoadVector(load);
        EXPECT_EQ(p.solver.getSolveStatistics().operatorSetups, op);
        EXPECT_EQ(p.solver.getSolveStatistics().preconditionerSetups, pre);
    };
    ippl::ParameterList parameter;
    parameter.add("tolerance", 2e-12);
    p.solver.mergeParameters(parameter);
    checkRebuild(false, false);
    parameter = ippl::ParameterList{};
    parameter.add("gauss_seidel_outer_iterations", 1);
    p.solver.mergeParameters(parameter);
    checkRebuild(false, true);
    for (const auto& entry :
         std::vector<std::pair<std::string, std::string>>{{"interpolation_nodes", "equispaced"},
                                                          {"quadrature_nodes", "gll"},
                                                          {"poisson_stiffness_mode", "standard"}}) {
        parameter = ippl::ParameterList{};
        parameter.add(entry.first, entry.second);
        p.solver.mergeParameters(parameter);
        checkRebuild(true, true);
    }
    p.mesh.setMeshSpacing(ippl::Vector<double, 1>(0.25));
    checkRebuild(true, true);
    const auto changed = p.x.deepCopy();
    p.solver.invalidatePreparedData();
    checkRebuild(true, true);
    EXPECT_LT(ippl::norm(p.x - changed), 1e-10 * ippl::norm(changed));
    p.solver.setRhs(p.b);
    checkRebuild(true, true);
}
TEST(FEMReuse, SmoothPeriodicSourceConvergesAtFourthOrder) {
    for (const std::string family : {"gll", "equispaced"}) {
        double previous = 0;
        for (unsigned cells : {8u, 16u}) {
            Problem p(cells);
            ippl::ParameterList options;
            options.add("interpolation_nodes", family);
            p.solver.mergeParameters(options);
            p.solver.configureNodeFamilies();
            Kokkos::View<double*> values("sinusoidal nodal source", 3 * cells);
            auto host             = Kokkos::create_mirror_view(values);
            const double nodes[3] = {0, family == "gll" ? (1 - 1 / std::sqrt(5.0)) / 2 : 1.0 / 3,
                                     family == "gll" ? (1 + 1 / std::sqrt(5.0)) / 2 : 2.0 / 3};
            const double wave     = 2 * Kokkos::numbers::pi_v<double>;
            for (unsigned i = 0; i < 3 * cells; ++i)
                host(i) = wave * wave * std::sin(wave * (i / 3 + nodes[i % 3]) / cells);
            Kokkos::deep_copy(values, host);
            p.solver.getSpace().fillFromGlobalCoefficients(p.b, values);
            p.solver.solve();
            const auto exact = KOKKOS_LAMBDA(const ippl::Vector<double, 1>& x) {
                return Kokkos::sin(wave * x[0]);
            };
            const double error = p.solver.getL2Error(exact);
            EXPECT_LT(error, 1e-3);
            if (previous) {
                EXPECT_GT(previous / error, 14);
            }
            previous = error;
        }
    }
}
int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
