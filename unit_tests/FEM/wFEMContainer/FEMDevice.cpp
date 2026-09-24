#include "Ippl.h"
#include "FEM/LagrangeSpace_wFEMContainer.h"
#include "FEM/Quadrature/SelectableQuadrature.h"
#include "gtest/gtest.h"

namespace {
using Handler = ippl::LagrangeDOFHandler<double, 1, 3>;
using Field = Handler::FEMContainer_t;
using Element = ippl::EdgeElement<double>;
using Quadrature = ippl::SelectableQuadrature<double, 4, Element>;
using Space = ippl::LagrangeSpace_wfc<double, 1, 3, Element, Quadrature, Field, Field>;
using Point = ippl::Vector<double, 1>;
}

// Compare reused reference tables with fresh objects after independently changing
// interpolation and the externally owned quadrature. Oracle suites validate the
// numerical values; this test specifically protects the device cache lifecycle.
void checkReferenceCaches() {
    ippl::NDIndex<1> domain(ippl::Vector<unsigned, 1>(5));
    ippl::FieldLayout<1> layout(MPI_COMM_WORLD, domain, std::array<bool, 1>{true}, false);
    ippl::UniformCartesian<double, 1> mesh(domain, Point(0.25), Point(0));
    Element element;
    Quadrature quadrature(element);
    Space space(mesh, element, quadrature, layout);
    Field input(mesh, layout);
    Kokkos::View<double*> coefficients("cache lifecycle coefficients", space.numGlobalDOFs());
    auto host = Kokkos::create_mirror_view(coefficients);
    for (size_t i = 0; i < host.extent(0); ++i) host(i) = std::sin(0.4 * i) + 0.1 * i;
    Kokkos::deep_copy(coefficients, host);
    space.fillFromGlobalCoefficients(input, coefficients);
    input.fillHalo();

    for (const auto family : {ippl::LagrangeNodeFamily::GLL, ippl::LagrangeNodeFamily::Equispaced,
                              ippl::LagrangeNodeFamily::GLL}) {
        space.setInterpolationNodes(family);
        auto previousLoad = input.deepCopy();
        for (const auto rule : {ippl::QuadratureNodeFamily::GaussLegendre, ippl::QuadratureNodeFamily::GaussLobatto,
                                ippl::QuadratureNodeFamily::GaussLegendre}) {
            quadrature.setFamily(rule);
            Quadrature freshQuadrature(element);
            freshQuadrature.setFamily(rule);
            Space fresh(mesh, element, freshQuadrature, layout);
            fresh.setInterpolationNodes(family);
            auto actual = input.deepCopy(), expected = input.deepCopy();
            space.evaluateLoadVector(actual);
            fresh.evaluateLoadVector(expected);
            EXPECT_LT(ippl::norm(actual - expected), 1e-13);
            // Four-point GLL underintegrates the P3 mass matrix, deliberately making
            // a stale quadrature cache observable. No solve uses this test load.
            if (rule == ippl::QuadratureNodeFamily::GaussLobatto)
                EXPECT_GT(ippl::norm(actual - previousLoad), 1e-8);
            else
                EXPECT_NEAR(space.computeAvg(input), fresh.computeAvg(input), 1e-13);
            previousLoad = actual;

            const auto device = space.getDeviceMirror();
            Kokkos::View<Point*> locations("device cache locations", 4);
            Kokkos::parallel_for("read cached DOF locations", 4, KOKKOS_LAMBDA(int i) {
                locations(i) = device.getRefElementDOFLocation(i);
            });
            auto result = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, locations);
            for (unsigned i = 0; i < 4; ++i)
                EXPECT_DOUBLE_EQ(result(i)[0], fresh.getRefElementDOFLocation(i)[0]);
        }
    }
}

void checkContainerArithmetic() {
    ippl::NDIndex<1> domain(ippl::Vector<unsigned, 1>(5));
    ippl::FieldLayout<1> layout(MPI_COMM_WORLD, domain, std::array<bool, 1>{true}, false);
    ippl::UniformCartesian<double, 1> mesh(domain, Point(0.25), Point(0));
    Field a(mesh, layout), b(mesh, layout), expected(mesh, layout);
    a = 6.; b = 2.; expected = 12.;
    EXPECT_DOUBLE_EQ(ippl::norm(a * b - expected), 0);
    expected = 3.;
    EXPECT_DOUBLE_EQ(ippl::norm(a / b - expected), 0);
    const auto views = a.getAllViews();
    const auto vertices = std::get<0>(views);
    double sum = 0;
    Kokkos::parallel_reduce("const DOFArray access", 1,
        KOKKOS_LAMBDA(int, double& value) {
            const auto dofs = vertices(1);
            value += dofs[0];
        }, sum);
    EXPECT_DOUBLE_EQ(sum, 6.);
}

TEST(FEMDevice, ReferenceCachesFollowInterpolationAndQuadrature) { checkReferenceCaches(); }
TEST(FEMDevice, ContainerArithmeticAndConstDofs) { checkContainerArithmetic(); }

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    if (ippl::Comm->rank() == 0)
        std::cout << "FEM execution space: " << Kokkos::DefaultExecutionSpace::name() << std::endl;
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
