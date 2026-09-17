#include "Oracle/FEMOracleTestData.h"
#include "Oracle/FEMOracleTestSupport.h"
#include "PoissonSolvers/FEMPoissonSolver_wFEMContainer.h"

#if FEM_ORACLE_DIM == 1
namespace oracle = fem_oracle::d1;
#elif FEM_ORACLE_DIM == 2
namespace oracle = fem_oracle::d2;
#elif FEM_ORACLE_DIM == 3
namespace oracle = fem_oracle::d3;
#endif
namespace support = fem_oracle::test;

// Each configuration/algorithm/boundary is a named test, allowing CTest to split
// expensive 3D solves without duplicating binaries or maintaining a case list.
template <typename Tag>
class FEMPoissonOracle : public ::testing::Test {
    const fem_oracle::OperatorCase& case_m;
    unsigned boundary_m;
    bool jacobi_m, zeroSource_m;

public:
    FEMPoissonOracle(const fem_oracle::OperatorCase& c, unsigned boundary, bool jacobi, bool zero)
        : case_m(c)
        , boundary_m(boundary)
        , jacobi_m(jacobi)
        , zeroSource_m(zero) {}
    void TestBody() override {
        const auto& c  = case_m;
        const auto& bc = c.boundaries[boundary_m];
        using Context  = support::Context<double, Tag>;
        using Field    = typename Context::Field;
        Context ctx(c.geometry);
        const double prescribed = bc.prescribedValues[0];
        auto lhs = ctx.field(boundary_m ? ippl::CONSTANT_FACE : ippl::ZERO_FACE, prescribed);
        // Source metadata should not be required to duplicate the solution's BCs.
        auto rhs = ctx.field();
        std::vector<double> source(c.source.nodalValues.begin(), c.source.nodalValues.end());
        std::vector<double> expected(bc.solution.begin(), bc.solution.end());
        std::vector<double> load(c.load.preBc.begin(), c.load.preBc.end());
        if (zeroSource_m) {
            std::fill(source.begin(), source.end(), 0.0);
            std::fill(expected.begin(), expected.end(), prescribed);
            std::fill(load.begin(), load.end(), 0.0);
        }
        // Nonzero initial boundary coefficients intentionally differ from g.
        ctx.scatter(lhs, c.op.probe);
        ctx.scatter(rhs, source);
        ippl::FEMPoissonSolver_wFEMContainer<Field, Field, Tag::ORDER, Tag::QUAD_POINTS> solver(
            lhs, rhs);
        ippl::ParameterList parameters;
        parameters.add("preconditioned", jacobi_m);
        parameters.add("preconditioner_type", std::string("jacobi"));
        parameters.add("poisson_stiffness_mode",
                       std::string(ippl::poissonStiffnessModeName(support::selectedStiffnessMode)));
        parameters.add("interpolation_nodes", std::string(c.geometry.interpolationFamily));
        parameters.add("quadrature_nodes", std::string("gauss_legendre"));
        parameters.add("max_iterations", 600);
        parameters.add("tolerance", 1e-12);
        solver.mergeParameters(parameters);
        solver.solve();
        EXPECT_TRUE(std::isfinite(solver.getResidue()));
        EXPECT_GE(solver.getIterationCount(), 0);
        EXPECT_LT(solver.getIterationCount(), 600);
        const auto solution = ctx.gather(lhs);
        EXPECT_EQ(solution.size(), expected.size());
        double coefficientError = 0, coefficientScale = 0;
        for (size_t i = 0; i < solution.size(); ++i) {
            EXPECT_TRUE(std::isfinite(solution[i])) << i;
            coefficientError += std::pow(solution[i] - expected[i], 2);
            coefficientScale += expected[i] * expected[i];
        }
        EXPECT_LE(std::sqrt(coefficientError), 1e-11 + 1e-9 * std::sqrt(coefficientScale));
        for (auto i : bc.constrainedDofs)
            EXPECT_NEAR(solution[i], prescribed, 2e-12) << i;

        // Independent residual uses immutable external A and the pre-BC load.
        const auto externalAction = support::apply(c.op.stiffness, solution);
        auto raw                  = ctx.field();
        ctx.scatter(raw, solution);
        raw.fillHalo();
        auto eval               = ctx.stiffness();
        auto actualActionField  = ctx.space_m.evaluateAx(raw, eval);
        const auto actualAction = ctx.gather(actualActionField);
        double externalResidual = 0, productionResidual = 0, loadScale = 0;
        for (auto i : bc.freeDofs) {
            externalResidual += std::pow(externalAction[i] - load[i], 2);
            productionResidual += std::pow(actualAction[i] - load[i], 2);
            loadScale += load[i] * load[i];
        }
        const double residualLimit = 1e-10 + 1e-9 * std::sqrt(loadScale);
        EXPECT_LE(std::sqrt(externalResidual), residualLimit);
        EXPECT_LE(std::sqrt(productionResidual), residualLimit);
        // This norm uses IPPL's owned-DOF/MPI reduction too.
        auto error = ctx.field();
        std::vector<double> differences(solution.size());
        for (size_t i = 0; i < solution.size(); ++i)
            differences[i] = solution[i] - expected[i];
        ctx.scatter(error, differences, true);
        EXPECT_NEAR(ippl::norm(error), std::sqrt(coefficientError), 2e-12);
    }
};

TEST(FEMPoissonRegistry, EveryCaseHasSolutionsAndOneCompiledType) {
    for (const auto& c : fem_oracle::operatorCases()) {
        const auto count = []<typename... Tags>(std::tuple<Tags...>, const auto& ref) {
            return (0 + ... + int(support::matches<Tags>(ref)));
        }(oracle::CaseTypes{}, c.geometry);
        EXPECT_EQ(count, 1) << c.geometry.name;
        for (const auto& bc : c.boundaries)
            EXPECT_EQ(bc.solution.size(), c.source.nodalValues.size()) << c.geometry.name;
    }
}

TEST(FEMPoissonRegistry, UnsupportedModesFailBeforeLoadAssembly) {
    using Tag = std::tuple_element_t<0, oracle::CaseTypes>;
    using Context = support::Context<double, Tag>;
    using Field = typename Context::Field;
    const auto cases = fem_oracle::operatorCases();
    const auto found = std::find_if(cases.begin(), cases.end(), [](const auto& c) {
        return support::matches<Tag>(c.geometry);
    });
    ASSERT_NE(found, cases.end());
    Context ctx(found->geometry);
    auto lhs = ctx.field(ippl::ZERO_FACE);
    auto rhs = ctx.field();
    ctx.scatter(rhs, found->source.nodalValues);
    ippl::FEMPoissonSolver_wFEMContainer<Field, Field, Tag::ORDER, Tag::QUAD_POINTS> solver(lhs, rhs);
    ippl::ParameterList parameters;
    ASSERT_EQ(solver.getStiffnessMode(), ippl::PoissonStiffnessMode::ConstantPreserving);
    parameters.add("preconditioner_type", std::string("multigrid"));
    solver.mergeParameters(parameters);
    EXPECT_THROW(solver.solve(), IpplException);
    parameters.update("preconditioner_type", std::string("jacobi"));
    solver.mergeParameters(parameters);
    std::array<ippl::FieldBC, 2 * Tag::DIM> periodic;
    periodic.fill(ippl::PERIODIC_FACE);
    lhs.setFieldBC(periodic);
    EXPECT_THROW(solver.solve(), IpplException);
    parameters.add("poisson_stiffness_mode", std::string("invalid"));
    solver.mergeParameters(parameters);
    EXPECT_THROW(solver.solve(), std::invalid_argument);
    parameters.update("poisson_stiffness_mode", std::string("rowsum_diagonal"));
    solver.mergeParameters(parameters);
    EXPECT_THROW(solver.solve(), std::invalid_argument);
    const auto after = ctx.gather(rhs);
    for (size_t i = 0; i < after.size(); ++i) EXPECT_EQ(after[i], found->source.nodalValues[i]);
}

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    support::configureStiffnessMode();
    [&]<typename... Tags>(std::tuple<Tags...>) {
        (
            [&] {
                for (const auto& c : fem_oracle::operatorCases()) {
                    if (!support::matches<Tags>(c.geometry))
                        continue;
                    for (bool jacobi : {false, true}) {
                        for (unsigned stage = 0; stage < 4; ++stage) {
                            const auto name =
                                std::string(c.geometry.name) + (jacobi ? "_Jacobi_" : "_CG_")
                                + (stage == 3 ? "ZeroSourceConstantBoundary" :
                                   stage == 2 ? "ZeroSource" : c.boundaries[stage].name);
                            ::testing::RegisterTest(
                                "FEMPoissonOracle", name.c_str(), nullptr, nullptr, __FILE__,
                                __LINE__, [&c, jacobi, stage]() -> ::testing::Test* {
                                    return new FEMPoissonOracle<Tags>(c, stage % 2, jacobi,
                                                                      stage >= 2);
                                });
                        }
                    }
                }
            }(),
            ...);
    }(oracle::CaseTypes{});
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
