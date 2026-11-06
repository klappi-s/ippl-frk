// Compare cached element quadrature diagnostics with the previous particle gather
// and integration on periodic stretched meshes, using the configured execution space.
#include "../../../demos/electrostaticFEM/FEMDiagnosticMoments.h"
#include "FEM/Quadrature/SelectableQuadrature.h"
#include "gtest/gtest.h"

namespace {
    template <unsigned P, unsigned Q = P + 1>
    void checkMoments() {
        constexpr unsigned Dim = 3;
        using Handler = ippl::LagrangeDOFHandler<double, Dim, P>;
        using Field = typename Handler::FEMContainer_t;
        using Point = ippl::Vector<double, Dim>;
        using Element = ippl::HexahedralElement<double>;
        using Quad = ippl::SelectableQuadrature<double, Q, Element>;
        using Space = ippl::LagrangeSpace_wfc<double, Dim, P, Element, Quad, Field, Field>;
        ippl::NDIndex<Dim> domain;
        for (unsigned d = 0; d < Dim; ++d) domain[d] = ippl::Index(6 - d);
        std::array<bool, Dim> parallel;
        parallel.fill(true);
        ippl::FieldLayout<Dim> layout(MPI_COMM_WORLD, domain, parallel, true);
        Point spacing;
        for (unsigned d = 0; d < Dim; ++d) spacing[d] = (1 + 0.25 * d) / (5 - d);
        ippl::UniformCartesian<double, Dim> mesh(domain, spacing, Point(0));
        Element element;
        Quad quad(element);
        Space space(mesh, element, quad, layout);
        Field potential(mesh, layout);
        std::array<ippl::FieldBC, 2 * Dim> bc;
        bc.fill(ippl::PERIODIC_FACE);
        potential.setFieldBC(bc);
        Kokkos::View<double*> coefficients("diagnostic coefficients", space.numGlobalDOFs());
        auto hostCoefficients = Kokkos::create_mirror_view(coefficients);
        for (size_t i = 0; i < hostCoefficients.extent(0); ++i)
            hostCoefficients(i) = std::sin(0.7 * i);
        Kokkos::deep_copy(coefficients, hostCoefficients);
        const auto points = quad.getIntegrationNodesForRefElement();
        const auto weights = quad.getWeightsForRefElement();
        const auto handler = space.getDOFHandler();
        const auto elements = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), handler.getElementIndices());
        const size_t count = elements.extent(0) * points.dim;
        Kokkos::View<Point*> positions("legacy quadrature positions", count), electric("legacy quadrature fields", count);
        auto hostPositions = Kokkos::create_mirror_view(positions);
        for (size_t e = 0; e < elements.extent(0); ++e) {
            const auto cell = handler.getElementNDIndex(elements(e));
            for (unsigned q = 0; q < points.dim; ++q)
                for (unsigned d = 0; d < Dim; ++d)
                    hostPositions(e * points.dim + q)[d] = (cell[d] + points[q][d]) * spacing[d];
        }
        Kokkos::deep_copy(positions, hostPositions);
        double determinant = 1;
        for (unsigned d = 0; d < Dim; ++d) determinant *= spacing[d];
        const double wave = 2 * Kokkos::numbers::pi_v<double>;
        for (auto family : {ippl::LagrangeNodeFamily::GLL, ippl::LagrangeNodeFamily::Equispaced}) {
            space.setInterpolationNodes(family);
            electrostatic_fem::FEMDiagnosticMoments<Dim> diagnostics;
            diagnostics.initialize(space, quad);
            for (bool constant : {false, true}) {
                SCOPED_TRACE(constant ? "constant" : "nonconstant");
                if (constant) potential = 1;
                else space.fillFromGlobalCoefficients(potential, coefficients);
                ippl::gatherFEMElectricField(potential, space, positions, electric, count);
                const auto hostField = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), electric);
                double local[3] = {}, expected[3] = {};
                for (size_t i = 0; i < count; ++i) {
                    const double weight = determinant * weights[i % points.dim];
                    for (unsigned d = 0; d < Dim; ++d)
                        local[0] += 0.5 * weight * hostField(i)[d] * hostField(i)[d];
                    local[1] += weight * hostField(i)[0] * std::cos(wave * hostPositions(i)[0]);
                    local[2] += weight * hostField(i)[0] * std::sin(wave * hostPositions(i)[0]);
                }
                ippl::Comm->allreduce<double>(local, expected, 3, std::plus<double>());
                const auto actual = diagnostics.evaluate(potential, space, wave);
                EXPECT_NEAR(actual.energy, expected[0], 2e-11 * std::max(1.0, expected[0]));
                EXPECT_NEAR(actual.real, expected[1], 2e-11 * std::max(1.0, std::abs(expected[1])));
                EXPECT_NEAR(actual.imaginary, expected[2], 2e-11 * std::max(1.0, std::abs(expected[2])));
                if (constant) {
                    EXPECT_NEAR(actual.energy, 0, 1e-20);
                    EXPECT_NEAR(actual.real, 0, 2e-11);
                    EXPECT_NEAR(actual.imaginary, 0, 2e-11);
                }
            }
            Quad endpointQuad(element, ippl::QuadratureNodeFamily::GaussLobatto);
            EXPECT_THROW(diagnostics.initialize(space, endpointQuad), IpplException);
        }
    }
}

// P1 interior GL diagnostics agree with the legacy gather on one or multiple ranks.
TEST(FEMDiagnosticMoments, P1) { checkMoments<1>(); }
// P2 checks both interpolation families, stretched cells and constant potentials.
TEST(FEMDiagnosticMoments, P2) { checkMoments<2>(); }
// P3 also checks unequal local element counts under the registered MPI2 run.
TEST(FEMDiagnosticMoments, P3) { checkMoments<3>(); }

// The demo also supports nine GL nodes per direction; retain that integration rule.
TEST(FEMDiagnosticMoments, P3NinePointRule) { checkMoments<3, 9>(); }

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
