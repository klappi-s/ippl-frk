// Check FEM structure, operator and solver reuse without stale values or layouts.
// Covers repeated periodic MPI1/MPI2 solves and Dirichlet boundary-mode preservation.
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
        explicit Problem(unsigned cells = 8, bool periodic = true)
            : domain(ippl::Vector<unsigned, 1>(cells + 1))
            , layout(MPI_COMM_WORLD, domain, std::array<bool, 1>{true}, periodic)
            , mesh(domain, ippl::Vector<double, 1>(1.0 / cells), ippl::Vector<double, 1>(0))
            , x(mesh, layout)
            , b(mesh, layout)
            , solver(x, b) {
            std::array<ippl::FieldBC, 2> bc;
            bc.fill(periodic ? ippl::PERIODIC_FACE : ippl::ZERO_FACE);
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

TEST(FEMReuse, StructureCloneHasIndependentZeroValuesAndOwnedEntityLayouts) {
    for (unsigned cells : {8u, 16u}) {
        Problem p(cells);
        auto source = std::make_unique<Field>(p.x);
        p.fill(*source);
        const auto internal = IpplTimings::getTimer("findInternal");
        const auto periodic = IpplTimings::getTimer("findPeriodic");
        const auto before =
            IpplTimings::getCallCount(internal) + IpplTimings::getCallCount(periodic);
        auto clone = source->cloneStructure();
        EXPECT_EQ(IpplTimings::getCallCount(internal) + IpplTimings::getCallCount(periodic),
                  before);
        EXPECT_DOUBLE_EQ(ippl::norm(clone), 0);
        for (auto bc : clone.getFieldBCTypes())
            EXPECT_EQ(bc, ippl::NO_FACE);
        clone.setFieldBC(source->getFieldBC());
        source.reset();
        p.fill(clone);
        clone.fillHalo();  // Entity layouts cannot refer to the destroyed source.
        p.fill(p.b);
        EXPECT_DOUBLE_EQ(ippl::norm(clone - p.b), 0);
        clone = 0;
        EXPECT_GT(ippl::norm(p.b), 0);
    }
}

// Repeated applications must clear previous owned/halo contributions while retaining
// output allocations. The same check runs with one and two MPI ranks.
TEST(FEMReuse, OperatorIntoReusesStorageAndClearsAccumulationHalos) {
    Problem p;
    p.fill(p.x);
    const double h = p.mesh.getMeshSpacing()[0];
    ippl::EvalFunctor<double, 1, 4> eval(ippl::Vector<double, 1>(1 / h), h,
                                       p.solver.getStiffnessMode());
    auto& space = p.solver.getSpace();
    auto result = p.x.cloneStructure();
    const auto vertexData = result.getView<ippl::Vertex<1>>().data();
    const auto edgeData = result.getView<ippl::EdgeX<1>>().data();
    auto original = p.x.deepCopy();
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        std::apply([](const auto&... views) {
            (Kokkos::deep_copy(views,
                typename std::remove_cvref_t<decltype(views)>::value_type(17.0)), ...);
        }, result.getAllViews());
        space.evaluateAxInto(p.x, eval, result);
        auto expected = space.evaluateAx(p.x, eval);
        EXPECT_LT(ippl::norm(result - expected), 1e-12);
        EXPECT_DOUBLE_EQ(ippl::norm(p.x - original), 0);
        EXPECT_EQ(result.getView<ippl::Vertex<1>>().data(), vertexData);
        EXPECT_EQ(result.getView<ippl::EdgeX<1>>().data(), edgeData);
    }
    EXPECT_THROW(space.evaluateAxInto(p.x, eval, p.x), IpplException);
}

// On nonperiodic MPI1/MPI2 layouts, an explicit ZERO_FACE operator must match
// the legacy action on a homogeneous-BC copy without changing prescribed input
// values or BC offsets. Both stiffness modes must leave owned boundary rows zero.
TEST(FEMReuse, HomogeneousOperatorModePreservesPrescribedInputBoundaries) {
    Problem p(8, false);
    const std::array<ippl::FieldBC, 2> types{ippl::CONSTANT_FACE, ippl::CONSTANT_FACE};
    const std::array<double, 2> offsets{2.75, 2.75}, slopes{0, 0};
    p.x.setFieldBC(types, offsets, slopes);
    p.x = 2.75;
    p.x.fillHalo();
    const auto original = p.x.deepCopy();
    auto homogeneous = p.x.deepCopy();
    homogeneous.setFieldBC(std::array<ippl::FieldBC, 2>{ippl::ZERO_FACE, ippl::ZERO_FACE});
    homogeneous.fillHalo();
    auto result = p.x.cloneStructure();
    auto& space = p.solver.getSpace();
    const double h = p.mesh.getMeshSpacing()[0];
    for (const auto mode : {ippl::PoissonStiffnessMode::Standard,
                            ippl::PoissonStiffnessMode::ConstantPreserving}) {
        ippl::EvalFunctor<double, 1, 4> eval(ippl::Vector<double, 1>(1 / h), h, mode);
        const auto expected = space.evaluateAx(homogeneous, eval);
        space.evaluateAxInto(p.x, eval, result, ippl::ZERO_FACE);
        EXPECT_NEAR(ippl::norm(result - expected), 0, 1e-12);
        EXPECT_DOUBLE_EQ(ippl::norm(p.x - original), 0);
        EXPECT_EQ(p.x.getFieldBCTypes(), types);
        using ScalarField = ippl::Field<double, 1, ippl::UniformCartesian<double, 1>, Cell>;
        const auto bc = p.x.getFieldBC();
        for (unsigned face = 0; face < 2; ++face) {
            auto* constant = dynamic_cast<ippl::ExtrapolateFace<ScalarField>*>(bc[face].get());
            ASSERT_NE(constant, nullptr);
            EXPECT_DOUBLE_EQ(constant->getOffset(), offsets[face]);
            EXPECT_DOUBLE_EQ(constant->getSlope(), slopes[face]);
        }
        const auto values = Kokkos::create_mirror_view_and_copy(
            Kokkos::HostSpace(), result.getView<ippl::Vertex<1>>());
        const auto owned = result.getLayout<ippl::Vertex<1>>().getLocalNDIndex();
        for (int i = 0; i < owned[0].length(); ++i) {
            const int global = owned[0].first() + i;
            if (global == 0 || global == 8)
                EXPECT_DOUBLE_EQ(values(i + result.getNghost())[0], 0);
        }
    }
}

// Cached layouts must not be rediscovered by operator and load temporaries.
TEST(FEMReuse, OperatorResultsDoNotRediscoverNeighbors) {
    Problem p;
    p.fill(p.x);
    const double h = p.mesh.getMeshSpacing()[0];
    ippl::EvalFunctor<double, 1, 4> eval(ippl::Vector<double, 1>(1 / h), h,
                                         p.solver.getStiffnessMode());
    auto& space         = p.solver.getSpace();
    const auto internal = IpplTimings::getTimer("findInternal");
    const auto periodic = IpplTimings::getTimer("findPeriodic");
    const auto before   = IpplTimings::getCallCount(internal) + IpplTimings::getCallCount(periodic);
    for (unsigned i = 0; i < 3; ++i) {
        auto full    = space.evaluateAx(p.x, eval);
        auto lower   = space.evaluateAx_lower(p.x, eval);
        auto upper   = space.evaluateAx_upper(p.x, eval);
        auto both    = space.evaluateAx_upperlower(p.x, eval);
        auto inverse = space.evaluateAx_inversediag(p.x, eval);
        p.b          = p.x;
        space.evaluateLoadVector(p.b);
        EXPECT_TRUE(std::isfinite(ippl::norm(full)));
        EXPECT_TRUE(std::isfinite(ippl::norm(inverse)));
        EXPECT_TRUE(std::isfinite(ippl::norm(p.b)));
        EXPECT_LT(ippl::norm(lower + upper - both), 1e-11);
    }
    EXPECT_EQ(IpplTimings::getCallCount(internal) + IpplTimings::getCallCount(periodic), before);
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
            const auto exact = [wave](const ippl::Vector<double, 1>& x) {
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
