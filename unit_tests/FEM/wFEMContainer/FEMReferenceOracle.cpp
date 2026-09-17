#include "Ippl.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "FEM/LagrangeSpace_wFEMContainer.h"
#include "FEM/Quadrature/SelectableQuadrature.h"
#include "PoissonSolvers/EvalFunctor.h"
#include "TestUtils.h"
#include "gtest/gtest.h"
#include "Oracle/FEMOracleTestData.h"

// Compile each dimension separately to bound template/header compilation memory.
#if FEM_ORACLE_DIM == 1
namespace oracle = fem_oracle::d1;
#elif FEM_ORACLE_DIM == 2
namespace oracle = fem_oracle::d2;
#elif FEM_ORACLE_DIM == 3
namespace oracle = fem_oracle::d3;
#else
#error "FEM_ORACLE_DIM must be 1, 2 or 3"
#endif

template <typename Tag>
constexpr bool matches(const fem_oracle::ReferenceCase& c) {
    return c.dimension == Tag::DIM && c.order == Tag::ORDER
           && c.reference.quadPointsPerAxis == Tag::QUAD_POINTS;
}

TEST(FEMReferenceRegistry, EveryCaseHasExactlyOneCompiledType) {
    for (const auto& c : fem_oracle::referenceCases()) {
        const auto count = [&]<typename... Tags>(std::tuple<Tags...>) {
            return (0 + ... + static_cast<int>(matches<Tags>(c)));
        }(oracle::CaseTypes{});
        EXPECT_EQ(count, 1) << c.name;
    }
}

template <typename>
class FEMReferenceOracle;

template <typename Scalar, typename Tag>
class FEMReferenceOracle<Parameters<Scalar, Tag>> : public ::testing::Test {
public:
    using T                               = Scalar;
    static constexpr unsigned DIM         = Tag::DIM;
    static constexpr unsigned ORDER       = Tag::ORDER;
    static constexpr unsigned QUAD_POINTS = Tag::QUAD_POINTS;
    using Handler                         = ippl::LagrangeDOFHandler<T, DIM, ORDER>;
    using Field                           = typename Handler::FEMContainer_t;
    using Element =
        std::tuple_element_t<DIM - 1,
                             std::tuple<ippl::EdgeElement<T>, ippl::QuadrilateralElement<T>,
                                        ippl::HexahedralElement<T>>>;
    using Quadrature = ippl::SelectableQuadrature<T, Tag::QUAD_POINTS, Element>;
    using Space      = ippl::LagrangeSpace_wfc<T, DIM, ORDER, Element, Quadrature, Field, Field>;
    using Point      = ippl::Vector<T, DIM>;
    static constexpr unsigned N = Handler::dofsPerElement;
    static constexpr bool FLOAT = std::is_same_v<T, float>;

    static Point point(fem_oracle::Reals values, std::size_t row = 0) {
        Point result;
        for (unsigned d = 0; d < DIM; ++d)
            result[d] = static_cast<T>(values[row * DIM + d]);
        return result;
    }

    struct Context {
        static ippl::NDIndex<DIM> makeDomain(const fem_oracle::ReferenceCase& c) {
            ippl::NDIndex<DIM> domain;
            for (unsigned d = 0; d < DIM; ++d)
                domain[d] = ippl::Index(c.mesh.cellsPerAxis[d] + 1);
            return domain;
        }
        static Point spacing(const fem_oracle::ReferenceCase& c) {
            Point result;
            for (unsigned d = 0; d < DIM; ++d)
                result[d] =
                    static_cast<T>((c.mesh.corner[d] - c.mesh.origin[d]) / c.mesh.cellsPerAxis[d]);
            return result;
        }
        static std::array<bool, DIM> parallelAxes() {
            std::array<bool, DIM> result;
            result.fill(true);
            return result;
        }
        explicit Context(const fem_oracle::ReferenceCase& c)
            : domain_m(makeDomain(c))
            , layout_m(MPI_COMM_WORLD, domain_m, parallelAxes(), false)
            , mesh_m(domain_m, spacing(c), point(c.mesh.origin))
            , quadrature_m(element_m)
            , space_m(mesh_m, element_m, quadrature_m, layout_m) {
            space_m.setInterpolationNodes(std::string_view(c.interpolationFamily) == "gll"
                                              ? ippl::LagrangeNodeFamily::GLL
                                              : ippl::LagrangeNodeFamily::Equispaced);
        }
        ippl::NDIndex<DIM> domain_m;
        ippl::FieldLayout<DIM> layout_m;
        ippl::UniformCartesian<T, DIM> mesh_m;
        Element element_m;
        Quadrature quadrature_m;
        Space space_m;
    };

    template <typename F>
    void eachCase(F&& function) {
        unsigned count = 0;
        for (const auto& c : fem_oracle::referenceCases()) {
            if (!matches<Tag>(c))
                continue;
            ++count;
            SCOPED_TRACE(c.name);
            ASSERT_EQ(c.reference.nodes.size(), N * DIM);
            Context context(c);
            function(c, context);
        }
        EXPECT_GT(count, 0u) << "compiled type has no oracle case";
    }

    static void close(T actual, double expected, bool derivative = false) {
        const double absolute = FLOAT ? (derivative ? 2e-5 : 4e-6) : 1e-12;
        const double relative = FLOAT ? (derivative ? 4e-5 : 2e-5) : 1e-10;
        EXPECT_NEAR(static_cast<double>(actual), expected,
                    absolute + relative * std::abs(expected));
    }

    static constexpr auto entityMasks() {
        return []<typename... Entities>(std::tuple<Entities...>) {
            return std::array<std::size_t, sizeof...(Entities)>{([] {
                std::size_t mask = 0;
                for (unsigned d = 0; d < DIM; ++d)
                    if (Entities::dir[d])
                        mask |= 1u << d;
                return mask;
            }())...};
        }(typename Handler::EntityTypes{});
    }
};

using Combinations   = CreateCombinations<std::tuple<double, float>, oracle::CaseTypes>::type;
using ReferenceTypes = TestForTypes<Combinations>::type;
struct ReferenceTypeNames {
    template <typename Param>
    static std::string GetName(int) {
        using Fixture = FEMReferenceOracle<Param>;
        return std::string(Fixture::FLOAT ? "FloatP" : "DoubleP") + std::to_string(Fixture::ORDER)
               + "Q" + std::to_string(Fixture::QUAD_POINTS);
    }
};
TYPED_TEST_SUITE(FEMReferenceOracle, ReferenceTypes, ReferenceTypeNames);

TYPED_TEST(FEMReferenceOracle, CoordinatesAndGeometry) {
    this->eachCase([&](const auto& c, auto& ctx) {
        using Fixture    = TestFixture;
        constexpr auto D = Fixture::DIM;
        auto& space      = ctx.space_m;
        ASSERT_EQ(space.numGlobalDOFs(), c.mesh.coordinates.size() / D);
        ASSERT_EQ(space.numElements(), c.mesh.cellOrigins.size() / D);
        for (unsigned i = 0; i < Fixture::N; ++i) {
            SCOPED_TRACE("local DOF " + std::to_string(i));
            const auto location = space.getRefElementDOFLocation(i);
            for (unsigned d = 0; d < D; ++d)
                Fixture::close(location[d], c.reference.nodes[i * D + d]);
        }
        for (std::size_t cell = 0; cell < space.numElements(); ++cell) {
            SCOPED_TRACE("cell " + std::to_string(cell));
            const auto index = space.getElementNDIndex(cell);
            EXPECT_EQ(space.getElementIndex(index), cell);
            const auto vertices = space.getElementMeshVertexPoints(index);
            const auto jacobian = ctx.element_m.getTransformationJacobian(vertices);
            const auto inverse  = ctx.element_m.getInverseTransposeTransformationJacobian(vertices);
            Fixture::close(ctx.element_m.getDeterminantOfTransformationJacobian(vertices),
                           c.mesh.determinants[cell]);
            for (unsigned d = 0; d < D; ++d) {
                Fixture::close(jacobian[d], c.mesh.jacobians[cell * D * D + d * D + d]);
                Fixture::close(inverse[d], 1 / c.mesh.jacobians[cell * D * D + d * D + d]);
                Fixture::close(vertices[0][d], c.mesh.cellOrigins[cell * D + d]);
            }
            for (unsigned i = 0; i < Fixture::N; ++i) {
                const auto local     = Fixture::point(c.reference.nodes, i);
                const auto physical  = ctx.element_m.localToGlobal(vertices, local);
                const auto global    = c.mesh.cellDofs[cell * Fixture::N + i];
                const auto recovered = ctx.element_m.globalToLocal(
                    vertices, Fixture::point(c.mesh.coordinates, global));
                for (unsigned d = 0; d < D; ++d) {
                    Fixture::close(physical[d], c.mesh.coordinates[global * D + d]);
                    Fixture::close(recovered[d], c.reference.nodes[i * D + d]);
                }
            }
        }
    });
}

TYPED_TEST(FEMReferenceOracle, DofMappingsAndBoundaryFlags) {
    this->eachCase([&](const auto& c, auto& ctx) {
        using Fixture           = TestFixture;
        constexpr auto D        = Fixture::DIM;
        constexpr auto masks    = Fixture::entityMasks();
        const auto& handler     = ctx.space_m.getDOFHandler();
        const auto localIndices = ctx.space_m.getLocalDOFIndices();
        for (unsigned i = 0; i < Fixture::N; ++i) {
            SCOPED_TRACE("local DOF " + std::to_string(i));
            EXPECT_EQ(localIndices[i], i);
            const auto mapping = handler.getElementDOFMapping(i);
            ASSERT_LT(mapping.entityTypeIndex, masks.size());
            EXPECT_EQ(masks[mapping.entityTypeIndex], c.reference.entityAxisMasks[i]);
            EXPECT_EQ(mapping.entityLocalDOF, c.reference.entityOffsets[i]);
            for (unsigned d = 0; d < D; ++d)
                EXPECT_EQ(mapping.entityLocalIndex[d], c.reference.entityLocalIndices[i * D + d]);
        }
        auto owned =
            Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, handler.getElementIndices());
        ASSERT_EQ(owned.extent(0), ctx.space_m.numElements());
        std::vector<unsigned> seen(ctx.space_m.numElements(), 0);
        for (std::size_t k = 0; k < owned.extent(0); ++k) {
            ASSERT_LT(owned(k), seen.size());
            ++seen[owned(k)];
        }
        for (auto count : seen)
            EXPECT_EQ(count, 1u);
        for (std::size_t cell = 0; cell < ctx.space_m.numElements(); ++cell) {
            SCOPED_TRACE("cell " + std::to_string(cell));
            const auto indices = ctx.space_m.getGlobalDOFIndices(cell);
            for (unsigned i = 0; i < Fixture::N; ++i) {
                SCOPED_TRACE("local DOF " + std::to_string(i));
                const auto expected = c.mesh.cellDofs[cell * Fixture::N + i];
                EXPECT_EQ(indices[i], expected);
                EXPECT_EQ(ctx.space_m.getGlobalDOFIndex(cell, i), expected);
                EXPECT_EQ(ctx.space_m.getLocalDOFIndex(cell, expected), i);
                bool boundary = false;
                for (unsigned d = 0; d < D; ++d) {
                    const bool expectedBoundary = c.boundaryAxes[expected * D + d];
                    EXPECT_EQ(handler.isDOFOnBoundary(cell, i, d), expectedBoundary);
                    boundary |= expectedBoundary;
                }
                EXPECT_EQ(handler.isDOFOnBoundary(cell, i), boundary);
            }
        }
    });
}

TYPED_TEST(FEMReferenceOracle, HostBasisAndGradients) {
    this->eachCase([&](const auto& c, auto& ctx) {
        using Fixture = TestFixture;
        for (std::size_t probe = 0; probe < c.reference.probePoints.size() / Fixture::DIM;
             ++probe) {
            SCOPED_TRACE("probe " + std::to_string(probe));
            auto point = Fixture::point(c.reference.probePoints, probe);
            for (unsigned i = 0; i < Fixture::N; ++i) {
                SCOPED_TRACE("local DOF " + std::to_string(i));
                Fixture::close(ctx.space_m.evaluateRefElementShapeFunction(i, point),
                               c.reference.basisValues[probe * Fixture::N + i]);
                auto gradient = ctx.space_m.evaluateRefElementShapeFunctionGradient(i, point);
                for (unsigned d = 0; d < Fixture::DIM; ++d)
                    Fixture::close(
                        gradient[d],
                        c.reference.basisGradients[(probe * Fixture::N + i) * Fixture::DIM + d],
                        true);
            }
        }
        for (unsigned node = 0; node < Fixture::N; ++node)
            for (unsigned i = 0; i < Fixture::N; ++i)
                Fixture::close(ctx.space_m.evaluateRefElementShapeFunction(
                                   i, Fixture::point(c.reference.nodes, node)),
                               i == node ? 1. : 0.);
    });
}

TYPED_TEST(FEMReferenceOracle, DeviceBasisAndMappings) {
    this->eachCase([&](const auto& c, auto& ctx) {
        using Fixture        = TestFixture;
        using T              = typename Fixture::T;
        constexpr unsigned N = Fixture::N, D = Fixture::DIM;
        auto device      = ctx.space_m.getDeviceMirror();
        auto handler     = ctx.space_m.getDOFHandler();
        const auto count = c.reference.probePoints.size() / D;
        Kokkos::View<typename Fixture::Point*> points("oracle probes", count);
        auto pointsHost = Kokkos::create_mirror_view(points);
        for (std::size_t i = 0; i < count; ++i)
            pointsHost(i) = Fixture::point(c.reference.probePoints, i);
        Kokkos::deep_copy(points, pointsHost);
        Kokkos::View<T**> values("device basis", count, N);
        Kokkos::View<std::size_t**> indices("device global mapping", ctx.space_m.numElements(), N);
        Kokkos::View<typename Fixture::Handler::DOFMapping*> mappings("device entity mapping", N);
        ippl::parallel_for(
            "oracle device basis", Kokkos::RangePolicy<>(0, count * N),
            KOKKOS_LAMBDA(const auto& index) {
                const auto k         = index[0];
                values(k / N, k % N) = device.evaluateRefElementShapeFunction(k % N, points(k / N));
            });
        ippl::parallel_for(
            "oracle device mapping", Kokkos::RangePolicy<>(0, indices.extent(0)),
            KOKKOS_LAMBDA(const auto& index) {
                const auto cell   = index[0];
                const auto global = device.getGlobalDOFIndices(handler.getElementNDIndex(cell));
                for (unsigned i = 0; i < N; ++i) {
                    indices(cell, i) = global[i];
                    if (cell == 0)
                        mappings(i) = handler.getElementDOFMapping(i);
                }
            });
        auto hostValues      = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, values);
        auto hostIndices     = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, indices);
        auto hostMappings    = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, mappings);
        constexpr auto masks = Fixture::entityMasks();
        for (std::size_t probe = 0; probe < count; ++probe)
            for (unsigned i = 0; i < N; ++i)
                Fixture::close(hostValues(probe, i), c.reference.basisValues[probe * N + i]);
        for (unsigned i = 0; i < N; ++i) {
            ASSERT_LT(hostMappings(i).entityTypeIndex, masks.size());
            EXPECT_EQ(masks[hostMappings(i).entityTypeIndex], c.reference.entityAxisMasks[i]);
            EXPECT_EQ(hostMappings(i).entityLocalDOF, c.reference.entityOffsets[i]);
            for (unsigned d = 0; d < D; ++d)
                EXPECT_EQ(hostMappings(i).entityLocalIndex[d],
                          c.reference.entityLocalIndices[i * D + d]);
        }
        for (std::size_t cell = 0; cell < indices.extent(0); ++cell)
            for (unsigned i = 0; i < N; ++i)
                EXPECT_EQ(hostIndices(cell, i), c.mesh.cellDofs[cell * N + i]);
    });
}

TYPED_TEST(FEMReferenceOracle, PolynomialReproductionThroughOrder) {
    this->eachCase([&](const auto& c, auto& ctx) {
        using Fixture        = TestFixture;
        using T              = typename Fixture::T;
        constexpr unsigned D = Fixture::DIM, N = Fixture::N;
        for (std::size_t probe = 0; probe < c.reference.probePoints.size() / D; ++probe) {
            SCOPED_TRACE("probe " + std::to_string(probe));
            const auto point = Fixture::point(c.reference.probePoints, probe);
            std::vector<T> values(N);
            std::vector<typename Fixture::Point> gradients(N);
            for (unsigned i = 0; i < N; ++i) {
                values[i]    = ctx.space_m.evaluateRefElementShapeFunction(i, point);
                gradients[i] = ctx.space_m.evaluateRefElementShapeFunctionGradient(i, point);
            }
            // All tensor-product monomials with each coordinate degree <= p.
            for (unsigned monomial = 0; monomial < N; ++monomial) {
                SCOPED_TRACE("monomial " + std::to_string(monomial));
                std::array<unsigned, D> powers;
                auto index      = monomial;
                double expected = 1;
                for (unsigned d = 0; d < D; ++d) {
                    powers[d] = index % (Fixture::ORDER + 1);
                    index /= Fixture::ORDER + 1;
                    expected *= std::pow(c.reference.probePoints[probe * D + d], powers[d]);
                }
                T value = 0;
                typename Fixture::Point gradient(T(0));
                for (unsigned i = 0; i < N; ++i) {
                    double coefficient = 1;
                    for (unsigned d = 0; d < D; ++d)
                        coefficient *= std::pow(c.reference.nodes[i * D + d], powers[d]);
                    value += static_cast<T>(coefficient) * values[i];
                    for (unsigned d = 0; d < D; ++d)
                        gradient[d] += static_cast<T>(coefficient) * gradients[i][d];
                }
                Fixture::close(value, expected);
                for (unsigned d = 0; d < D; ++d)
                    Fixture::close(gradient[d],
                                   expected * powers[d] / c.reference.probePoints[probe * D + d],
                                   true);
            }
        }
    });
}

TYPED_TEST(FEMReferenceOracle, QuadratureAndElementMatrices) {
    this->eachCase([&](const auto& c, auto& ctx) {
        using Fixture        = TestFixture;
        using T              = typename Fixture::T;
        using Point          = typename Fixture::Point;
        constexpr unsigned N = Fixture::N, D = Fixture::DIM;
        constexpr unsigned Q = Fixture::Quadrature::numElementNodes;
        auto points          = ctx.quadrature_m.getIntegrationNodesForRefElement();
        auto weights         = ctx.quadrature_m.getWeightsForRefElement();
        ASSERT_EQ(c.reference.quadWeights.size(), Q);
        EXPECT_EQ(ctx.quadrature_m.getDegree(), c.reference.quadExactnessDegree);
        EXPECT_EQ(ctx.quadrature_m.getOrder(), c.reference.quadExactnessDegree + 1);
        for (unsigned d = 0; d < D; ++d) {
            for (unsigned degree = 0; degree <= c.reference.quadExactnessDegree; ++degree) {
                T integral = 0;
                for (unsigned q = 0; q < Q; ++q)
                    integral += weights[q] * std::pow(points[q][d], degree);
                Fixture::close(integral, 1. / (degree + 1));
            }
        }
        Kokkos::View<ippl::Vector<T, N>*> basis("element basis", Q);
        Kokkos::View<ippl::Vector<Point, N>*> gradients("element gradients", Q);
        auto hostBasis     = Kokkos::create_mirror_view(basis);
        auto hostGradients = Kokkos::create_mirror_view(gradients);
        for (unsigned q = 0; q < Q; ++q) {
            Fixture::close(weights[q], c.reference.quadWeights[q]);
            for (unsigned d = 0; d < D; ++d)
                Fixture::close(points[q][d], c.reference.quadPoints[q * D + d]);
            for (unsigned i = 0; i < N; ++i) {
                hostBasis(q)[i] = ctx.space_m.evaluateRefElementShapeFunction(i, points[q]);
                hostGradients(q)[i] =
                    ctx.space_m.evaluateRefElementShapeFunctionGradient(i, points[q]);
            }
        }
        Kokkos::deep_copy(basis, hostBasis);
        Kokkos::deep_copy(gradients, hostGradients);
        const auto vertices =
            ctx.space_m.getElementMeshVertexPoints(ctx.space_m.getElementNDIndex(0));
        const auto inverse     = ctx.element_m.getInverseTransposeTransformationJacobian(vertices);
        const auto determinant = ctx.element_m.getDeterminantOfTransformationJacobian(vertices);
        const ippl::EvalFunctor<T, D, N> reference(Point(T(1)), T(1)),
            physical(inverse, determinant);
        Kokkos::View<T***> matrices("actual element matrices", 4, N, N);
        // Integrate production basis/gradient and EvalFunctor primitives. Expected
        // matrices are the immutable DOLFINx data; no field adapter is involved.
        ippl::parallel_for(
            "oracle element integrals", Kokkos::RangePolicy<>(0, N * N),
            KOKKOS_LAMBDA(const auto& index) {
                const unsigned i = index[0] / N, j = index[0] % N;
                T stiffnessRef = 0, massRef = 0, stiffnessPhysical = 0;
                for (unsigned q = 0; q < Q; ++q) {
                    const ippl::RefShapeFunctionData<T, Point, N> shape{basis(q), gradients(q)};
                    stiffnessRef += weights[q] * reference(i, j, shape);
                    massRef += weights[q] * shape.val_q[i] * shape.val_q[j];
                    stiffnessPhysical += weights[q] * physical(i, j, shape);
                }
                matrices(0, i, j) = stiffnessRef;
                matrices(1, i, j) = massRef;
                matrices(2, i, j) = stiffnessPhysical;
                matrices(3, i, j) = massRef * determinant;
            });
        const auto actual = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, matrices);
        const std::array<fem_oracle::Reals, 4> expected{c.reference.stiffness, c.reference.mass,
                                                        c.physicalStiffness, c.physicalMass};
        for (unsigned matrix = 0; matrix < 4; ++matrix) {
            SCOPED_TRACE("matrix " + std::to_string(matrix));
            ASSERT_EQ(expected[matrix].size(), N * N);
            for (unsigned i = 0; i < N; ++i)
                for (unsigned j = 0; j < N; ++j) {
                    const double scale =
                        std::sqrt(expected[matrix][i * N + i] * expected[matrix][j * N + j]);
                    // Float accumulation bound scales with quadrature count and the
                    // energy of the two basis functions, including near-zero entries.
                    const double tolerance =
                        Fixture::FLOAT
                            ? 1e-6 + 4 * std::numeric_limits<float>::epsilon() * Q * scale
                            : 2e-12 + 1e-10 * scale;
                    EXPECT_NEAR(actual(matrix, i, j), expected[matrix][i * N + j], tolerance)
                        << "row=" << i << " column=" << j;
                }
        }
    });
}

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    int result = 1;
    if (ippl::Comm->size() == 1)
        result = RUN_ALL_TESTS();
    else
        std::cerr << "FEM reference tests require one MPI rank.\n";
    ippl::finalize();
    return result;
}
