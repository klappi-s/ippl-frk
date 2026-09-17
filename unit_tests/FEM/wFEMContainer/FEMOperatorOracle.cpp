#include "Oracle/FEMOracleTestData.h"
#include "Oracle/FEMOracleTestSupport.h"
#include "PoissonConvergenceSources.hpp"
#include "TestUtils.h"

#if FEM_ORACLE_DIM == 1
namespace oracle = fem_oracle::d1;
#elif FEM_ORACLE_DIM == 2
namespace oracle = fem_oracle::d2;
#elif FEM_ORACLE_DIM == 3
namespace oracle = fem_oracle::d3;
#endif

namespace support = fem_oracle::test;
// CTest partitions only the expensive column loop; every column runs exactly once.
extern unsigned columnShard, columnShards;

namespace {

    template <typename>
    class FEMOperatorOracle;
    template <typename Scalar, typename Tag>
    class FEMOperatorOracle<Parameters<Scalar, Tag>> : public ::testing::Test {
    public:
        using T                       = Scalar;
        using Context                 = support::Context<T, Tag>;
        static constexpr unsigned DIM = Tag::DIM, ORDER = Tag::ORDER, Q = Tag::QUAD_POINTS;
        static constexpr unsigned QUAD_TOTAL = [] {
            unsigned n = 1;
            for (unsigned d = 0; d < DIM; ++d)
                n *= Q;
            return n;
        }();
        template <typename F>
        void eachCase(F function, bool single = false) {
            unsigned count = 0;
            for (const auto& c : fem_oracle::operatorCases()) {
                if (!support::matches<Tag>(c.geometry))
                    continue;
                ++count;
                SCOPED_TRACE(c.geometry.name);
                Context ctx(c.geometry, single);
                function(c, ctx);
            }
            EXPECT_GT(count, 0u);
        }
        static void compare(fem_oracle::Reals actual, fem_oracle::Reals expected,
                            fem_oracle::Reals scale = {}) {
            support::compare<T, QUAD_TOTAL>(actual, expected, scale);
        }
    };
    using SlotTag       = std::tuple_element_t<FEM_ORACLE_ORDER_SLOT, oracle::CaseTypes>;
    using Combinations  = CreateCombinations<std::tuple<double, float>, std::tuple<SlotTag>>::type;
    using OperatorTypes = TestForTypes<Combinations>::type;
    struct OperatorTypeNames {
        template <typename Param>
        static std::string GetName(int) {
            using F = FEMOperatorOracle<Param>;
            return std::string(std::is_same_v<typename F::T, float> ? "FloatP" : "DoubleP")
                   + std::to_string(F::ORDER) + "Q" + std::to_string(F::Q);
        }
    };
    TYPED_TEST_SUITE(FEMOperatorOracle, OperatorTypes, OperatorTypeNames);

    TYPED_TEST(FEMOperatorOracle, AdapterAgainstProductionFillAndExternalCoordinates) {
        this->eachCase([&](const auto& c, auto& ctx) {
            auto field = ctx.field();
            ctx.fillProduction(field, c.op.probe);
            this->compare(ctx.gather(field), c.op.probe);
            // Coordinate-valued fills exercise every external index independently.
            for (unsigned d = 0; d < TestFixture::DIM; ++d) {
                std::vector<double> coordinates(c.op.probe.size());
                for (size_t i = 0; i < coordinates.size(); ++i)
                    coordinates[i] = c.geometry.mesh.coordinates[i * TestFixture::DIM + d];
                ctx.fillProduction(field, coordinates);
                this->compare(ctx.gather(field), coordinates);
            }
        });
    }

    TYPED_TEST(FEMOperatorOracle, SingleCellStiffnessAndMassColumnsNoFace) {
        if (ippl::Comm->size() != 1)
            GTEST_SKIP() << "Single-cell columns run on one rank.";
        this->eachCase(
            [&](const auto& c, auto& ctx) {
                constexpr auto N = TestFixture::Context::N;
                // Canonical entity order -> x-fastest order on the one-cell lattice.
                std::vector<size_t> permutation(N);
                const auto& nodes = c.geometry.reference.nodes;
                for (size_t i = 0; i < N; ++i) {
                    size_t global = 0, stride = 1;
                    for (unsigned d = 0; d < TestFixture::DIM; ++d) {
                        std::vector<double> axis;
                        for (size_t j = 0; j < N; ++j)
                            axis.push_back(nodes[j * TestFixture::DIM + d]);
                        std::sort(axis.begin(), axis.end());
                        axis.erase(std::unique(axis.begin(), axis.end()), axis.end());
                        global += std::distance(axis.begin(),
                                                std::lower_bound(axis.begin(), axis.end(),
                                                                 nodes[i * TestFixture::DIM + d]))
                                  * stride;
                        stride *= TestFixture::ORDER + 1;
                    }
                    permutation[i] = global;
                }
                auto a     = ctx.stiffness();
                auto m     = ctx.mass();
                auto field = ctx.field();
                for (size_t column = columnShard; column < N; column += columnShards) {
                    SCOPED_TRACE("element column " + std::to_string(column));
                    std::vector<double> unit(N), expectedA(N), expectedM(N), scaleA(N), scaleM(N);
                    unit[permutation[column]] = 1;
                    for (size_t row = 0; row < N; ++row) {
                        expectedA[permutation[row]] =
                            c.geometry.physicalStiffness[row * N + column];
                        expectedM[permutation[row]] = c.geometry.physicalMass[row * N + column];
                        scaleA[permutation[row]] =
                            std::sqrt(c.geometry.physicalStiffness[row * N + row]
                                      * c.geometry.physicalStiffness[column * N + column]);
                        scaleM[permutation[row]] =
                            std::sqrt(c.geometry.physicalMass[row * N + row]
                                      * c.geometry.physicalMass[column * N + column]);
                    }
                    ctx.scatter(field, unit);
                    auto ax = ctx.space_m.evaluateAx(field, a);
                    auto mx = ctx.space_m.evaluateAx(field, m);
                    this->compare(ctx.gather(ax), expectedA, scaleA);
                    this->compare(ctx.gather(mx), expectedM, scaleM);
                    // A nodal impulse load must produce that same external mass column.
                    ctx.space_m.evaluateLoadVector(field);
                    this->compare(ctx.gather(field), expectedM, scaleM);
                }
                std::vector<double> source(N), load(N);
                for (size_t i = 0; i < N; ++i) {
                    source[permutation[i]] = c.singleCellSource[i];
                    load[permutation[i]]   = c.singleCellLoad[i];
                }
                ctx.scatter(field, source);
                ctx.space_m.evaluateLoadVector(field);
                this->compare(ctx.gather(field), load);
            },
            true);
    }

    TYPED_TEST(FEMOperatorOracle, RawWholeFieldActionsAndEntityImpulses) {
        this->eachCase([&](const auto& c, auto& ctx) {
            auto field = ctx.field();
            auto a     = ctx.stiffness();
            auto m     = ctx.mass();
            auto check = [&](fem_oracle::Reals input, fem_oracle::Reals expectedA,
                             fem_oracle::Reals expectedM) {
                ctx.scatter(field, input);
                field.fillHalo();
                auto ax = ctx.space_m.evaluateAx(field, a);
                auto mx = ctx.space_m.evaluateAx(field, m);
                this->compare(ctx.gather(ax), expectedA,
                              support::apply(c.op.stiffness, input, true));
                this->compare(ctx.gather(mx), expectedM, support::apply(c.op.mass, input, true));
                this->compare(ctx.gather(field), input);
            };
            check(c.op.probe, c.op.stiffnessAction, c.op.massAction);
            for (auto index : c.impulseDofs) {
                SCOPED_TRACE("entity impulse " + std::to_string(index));
                std::vector<double> impulse(c.op.probe.size());
                impulse[index] = 1;
                check(impulse, support::apply(c.op.stiffness, impulse),
                      support::apply(c.op.mass, impulse));
            }
        });
    }

    TYPED_TEST(FEMOperatorOracle, NodalLoadRetainsBoundarySourceSamples) {
        this->eachCase([&](const auto& c, auto& ctx) {
            for (auto type : {ippl::NO_FACE, ippl::ZERO_FACE, ippl::CONSTANT_FACE}) {
                SCOPED_TRACE("BC type " + std::to_string(type));
                auto field = ctx.field(type, typename TestFixture::T(.75));
                ctx.scatter(field, c.source.nodalValues);
                field.fillHalo();
                this->compare(ctx.gather(field), c.source.nodalValues);
                ctx.space_m.evaluateLoadVector(field);
                auto expected = std::vector<double>(c.load.preBc.begin(), c.load.preBc.end());
                if (type != ippl::NO_FACE)
                    for (auto i : c.boundaries[0].constrainedDofs)
                        expected[i] = 0;
                this->compare(ctx.gather(field), expected,
                              support::apply(c.op.mass, c.source.nodalValues, true));
            }
            auto constant = ctx.field();
            std::vector<double> ones(c.op.probe.size(), 1);
            ctx.scatter(constant, ones);
            constant.fillHalo();
            ctx.space_m.evaluateLoadVector(constant);
            this->compare(ctx.gather(constant), c.constantLoad);
        });
    }

    TYPED_TEST(FEMOperatorOracle, SourceSamplingUsesPhysicalNodesAndSelectedFamily) {
        this->eachCase([&](const auto& c, auto& ctx) {
            using namespace ippl::poisson_convergence_wfc;
            auto field = ctx.field(ippl::ZERO_FACE);
            assignSourceToField<typename TestFixture::T, TestFixture::DIM, TestFixture::ORDER,
                                SourceCase::HighOrderPolynomial>(
                field, ctx.mesh_m, ctx.layout_m, ctx.space_m.getInterpolationNodes());
            const auto sampled = ctx.gather(field);
            for (size_t i = 0; i < sampled.size(); ++i) {
                SCOPED_TRACE("source sample " + std::to_string(i));
                if constexpr (std::is_same_v<typename TestFixture::T, float>) {
                    EXPECT_NEAR(
                        sampled[i], c.sampledConvergenceSource[i],
                        1e-6
                            + std::numeric_limits<float>::epsilon() * c.sampledConvergenceScale[i]);
                } else {
                    EXPECT_NEAR(sampled[i], c.sampledConvergenceSource[i],
                                1e-12 + 1e-10 * std::abs(c.sampledConvergenceSource[i]));
                }
            }
            // The sampler's metadata must not eliminate source boundary samples.
            ctx.space_m.evaluateLoadVector(field);
            auto expected = support::apply(c.op.mass, c.sampledConvergenceSource);
            for (auto i : c.boundaries[0].constrainedDofs)
                expected[i] = 0;
            auto scale = support::apply(c.op.mass, c.sampledConvergenceSource, true);
            if constexpr (std::is_same_v<typename TestFixture::T, float>) {
                const auto sampling = support::apply(c.op.mass, c.sampledConvergenceScale, true);
                for (size_t i = 0; i < scale.size(); ++i)
                    scale[i] += sampling[i] / (4 * TestFixture::QUAD_TOTAL);
            }
            this->compare(ctx.gather(field), expected, scale);
        });
    }

    TYPED_TEST(FEMOperatorOracle, ConstrainedActionsBoundaryLiftAndRhs) {
        this->eachCase([&](const auto& c, auto& ctx) {
            auto a = ctx.stiffness();
            for (const auto& bc : c.boundaries) {
                SCOPED_TRACE(bc.name);
                const bool zero = std::string_view(bc.name) == "zero";
                auto field      = ctx.field(zero ? ippl::ZERO_FACE : ippl::CONSTANT_FACE,
                                            typename TestFixture::T(.75));
                auto input      = std::vector<double>(c.op.probe.begin(), c.op.probe.end());
                if (!zero)
                    for (size_t i = 0; i < bc.constrainedDofs.size(); ++i)
                        input[bc.constrainedDofs[i]] = bc.prescribedValues[i];
                ctx.scatter(field, input);
                field.fillHalo();
                auto action = ctx.space_m.evaluateAx(field, a);
                auto actual = ctx.gather(action);
                std::vector<double> expected(input.size());
                for (size_t i = 0; i < bc.freeDofs.size(); ++i)
                    expected[bc.freeDofs[i]] = bc.operatorAction[i];
                if (!zero)
                    for (size_t i = 0; i < bc.constrainedDofs.size(); ++i)
                        expected[bc.constrainedDofs[i]] = bc.prescribedValues[i];
                this->compare(actual, expected, support::apply(c.op.stiffness, input, true));
                // Only boundary coefficients contribute to the lifting operator.
                for (size_t i = 0; i < bc.constrainedDofs.size(); ++i)
                    input[bc.constrainedDofs[i]] = bc.prescribedValues[i];
                ctx.scatter(field, input);
                field.fillHalo();
                auto lift = ctx.space_m.evaluateAx_lift(field, a);
                actual    = ctx.gather(lift);
                std::fill(expected.begin(), expected.end(), 0);
                for (size_t i = 0; i < bc.freeDofs.size(); ++i)
                    expected[bc.freeDofs[i]] = bc.lift[i];
                this->compare(actual, expected, support::apply(c.op.stiffness, input, true));
                auto load = ctx.field(ippl::ZERO_FACE);
                ctx.scatter(load, c.source.nodalValues);
                load.fillHalo();
                ctx.space_m.evaluateLoadVector(load);
                load   = load - lift;
                actual = ctx.gather(load);
                std::fill(expected.begin(), expected.end(), 0);
                for (size_t i = 0; i < bc.freeDofs.size(); ++i)
                    expected[bc.freeDofs[i]] = bc.liftedRhs[i];
                this->compare(actual, expected);
            }
        });
    }

    TYPED_TEST(FEMOperatorOracle, DiagonalInverseAndGlobalTriangularBlocks) {
        this->eachCase([&](const auto& c, auto& ctx) {
            auto a = ctx.stiffness();
            for (auto type : {ippl::NO_FACE, ippl::ZERO_FACE, ippl::CONSTANT_FACE}) {
                SCOPED_TRACE("BC type " + std::to_string(type));
                auto field = ctx.field(type, typename TestFixture::T(.75));
                auto input = std::vector<double>(c.op.probe.begin(), c.op.probe.end());
                if (type == ippl::CONSTANT_FACE)
                    for (auto i : c.boundaries[0].constrainedDofs)
                        input[i] = .75;
                ctx.scatter(field, input);
                field.fillHalo();
                std::vector<double> d(input.size()), inv(input.size());
                for (size_t i = 0; i < input.size(); ++i) {
                    d[i]   = c.op.diagonal[i] * input[i];
                    inv[i] = input[i] / c.op.diagonal[i];
                }
                if (type != ippl::NO_FACE)
                    for (auto i : c.boundaries[0].constrainedDofs)
                        d[i] = inv[i] = type == ippl::ZERO_FACE ? 0 : input[i];
                auto diagonal = ctx.space_m.evaluateAx_diag(field, a);
                auto inverse  = ctx.space_m.evaluateAx_inversediag(field, a);
                this->compare(ctx.gather(diagonal), d);
                this->compare(ctx.gather(inverse), inv);
            }
            for (auto type : {ippl::ZERO_FACE, ippl::CONSTANT_FACE}) {
                auto field = ctx.field(type, typename TestFixture::T(.75));
                auto input = std::vector<double>(c.op.probe.begin(), c.op.probe.end());
                for (auto i : c.boundaries[0].constrainedDofs)
                    input[i] = type == ippl::ZERO_FACE ? 0 : .75;
                ctx.scatter(field, input);
                field.fillHalo();
                auto lower = ctx.space_m.evaluateAx_lower(field, a);
                auto upper = ctx.space_m.evaluateAx_upper(field, a);
                auto off   = ctx.space_m.evaluateAx_upperlower(field, a);
                auto check = [&](auto& result, fem_oracle::Reals freeAction, const char* stage) {
                    SCOPED_TRACE(stage);
                    std::vector<double> expected(input.size());
                    for (size_t i = 0; i < freeAction.size(); ++i)
                        expected[c.boundaries[0].freeDofs[i]] = freeAction[i];
                    if (type == ippl::CONSTANT_FACE)
                        for (auto i : c.boundaries[0].constrainedDofs)
                            expected[i] = input[i];
                    this->compare(ctx.gather(result), expected,
                                  support::apply(c.op.stiffness, input, true));
                };
                check(lower, c.boundaries[0].lowerAction, "strict lower action");
                check(upper, c.boundaries[0].upperAction, "strict upper action");
                check(off, c.boundaries[0].offDiagonalAction, "off-diagonal action");
            }
        });
    }

    TYPED_TEST(FEMOperatorOracle, ConstantActionBoundaryColumnsAndSplitConsistency) {
        using T = typename TestFixture::T;
        this->eachCase([&](const auto& c, auto& ctx) {
            auto a = ctx.stiffness();
            auto standard = ctx.stiffness(ippl::PoissonStiffnessMode::Standard);
            const auto matrix = ctx.space_m.assembleElementMatrix(a);
            const auto original = ctx.space_m.assembleElementMatrix(standard);
            constexpr auto N = TestFixture::Context::N;
            for (size_t i = 0; i < N; ++i) {
                long double sum = 0, magnitude = 0;
                for (size_t j = 0; j < N; ++j) {
                    if (i != j) {
                        EXPECT_EQ(matrix[i][j], original[i][j]);
                    }
                    sum += matrix[i][j];
                    magnitude += std::abs(static_cast<long double>(matrix[i][j]));
                    EXPECT_NEAR(matrix[i][j], matrix[j][i],
                        8 * std::numeric_limits<T>::epsilon() * TestFixture::QUAD_TOTAL
                          * std::sqrt(std::abs(double(matrix[i][i]) * matrix[j][j])));
                }
                if (a.enforceZeroRowSum()) {
                    EXPECT_LE(std::abs(sum), N * std::numeric_limits<T>::epsilon() * magnitude);
                }
            }
            // Coordinate coefficients represent nonconstant affine functions.
            for (unsigned d = 0; d < TestFixture::DIM; ++d) {
                long double energy = 0;
                for (size_t i = 0; i < N; ++i)
                    for (size_t j = 0; j < N; ++j)
                        energy += c.geometry.reference.nodes[i * TestFixture::DIM + d]
                                  * static_cast<long double>(matrix[i][j])
                                  * c.geometry.reference.nodes[j * TestFixture::DIM + d];
                EXPECT_GT(energy, 0);
            }

            auto raw = ctx.field();
            std::vector<double> constant(c.op.probe.size(), 1.25), zero(constant.size());
            ctx.scatter(raw, constant);
            raw.fillHalo();
            auto rawAction = ctx.space_m.evaluateAx(raw, a);
            if (a.useCoefficientDifferences()) {
                EXPECT_EQ(ippl::norm(rawAction), T(0));
            } else {
                this->compare(ctx.gather(rawAction), zero,
                              support::apply(c.op.stiffness, constant, true));
            }
            auto mass = ctx.mass();
            auto massAction = ctx.space_m.evaluateAx(raw, mass);
            EXPECT_GT(ippl::norm(massAction), T(0));

            auto constrained = ctx.field(ippl::ZERO_FACE);
            ctx.scatter(constrained, constant); // Deliberately nonzero physical boundary entries.
            constrained.fillHalo();
            auto full = ctx.space_m.evaluateAx(constrained, a);
            auto diagonal = ctx.space_m.evaluateAx_diag(constrained, a);
            auto off = ctx.space_m.evaluateAx_upperlower(constrained, a);
            auto split = diagonal.deepCopy();
            split = diagonal + off;
            for (auto i : c.boundaries[0].constrainedDofs) constant[i] = 0;
            auto expected = support::apply(c.op.stiffness, constant);
            for (auto i : c.boundaries[0].constrainedDofs) expected[i] = 0;
            const auto scale = support::apply(c.op.stiffness, constant, true);
            this->compare(ctx.gather(full), expected, scale);
            this->compare(ctx.gather(split), ctx.gather(full), scale);
        });
    }

    TYPED_TEST(FEMOperatorOracle, CoefficientNormsExcludeGhostsAndPhysicalErrorIsAbsolute) {
        this->eachCase([&](const auto& c, auto& ctx) {
            using T = typename TestFixture::T;
            for (int ghosts : {1, 2}) {
                SCOPED_TRACE("ghost layers " + std::to_string(ghosts));
                auto field = ctx.field(ippl::NO_FACE, T(0), ghosts),
                     other = ctx.field(ippl::NO_FACE, T(0), ghosts);
                ctx.scatter(field, c.op.probe, true);
                ctx.scatter(other, c.diagnostics.otherVector, true);
                std::array<double, 6> actual{ippl::norm(field, 1),
                                             ippl::norm(field, 2),
                                             innerProduct(field, other),
                                             std::max(std::abs(field.min()), std::abs(field.max())),
                                             field.min(),
                                             field.max()};
                std::array<double, 6> expected{
                    c.diagnostics.coefficientL1,
                    c.diagnostics.coefficientL2,
                    c.diagnostics.coefficientInnerProduct,
                    c.diagnostics.coefficientLInf,
                    *std::min_element(c.op.probe.begin(), c.op.probe.end()),
                    *std::max_element(c.op.probe.begin(), c.op.probe.end())};
                this->compare(actual, expected);
                // Both reduction neutral values matter: all-negative maxima and all-positive
                // minima.
                for (double value : {-2., 2.}) {
                    std::vector<double> constant(c.op.probe.size(), value);
                    ctx.scatter(field, constant, true);
                    EXPECT_EQ(field.min(), value);
                    EXPECT_EQ(field.max(), value);
                }
            }
            auto field = ctx.field();
            ctx.scatter(field, c.op.probe);
            field.fillHalo();
            std::array<double, 2> actual{
                ctx.space_m.computeErrorL2(field, support::Zero<T, TestFixture::DIM>{}),
                ctx.space_m.computeErrorL2(field, support::Exact<T, TestFixture::DIM>{})};
            std::array<double, 2> expected{c.diagnostics.physicalL2, c.diagnostics.absoluteL2Error};
            this->compare(actual, expected);
            EXPECT_GT(actual[1], 1e-3);
            // Separate relative quantity; computeErrorL2 itself returns the absolute integral norm.
            std::array<double, 1> relative{actual[1] / c.diagnostics.exactL2},
                expectedRelative{c.diagnostics.relativeL2Error};
            this->compare(relative, expectedRelative);
        });
    }

}  // namespace
