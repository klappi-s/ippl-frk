#include "Ippl.h"

#include <string_view>

#include "FEM/FEMContainerInterpolate.h"
#include "FEM/Quadrature/SelectableQuadrature.h"
#include "Oracle/FEMOracleTestData.h"
#include "gtest/gtest.h"

template <unsigned Dim, unsigned P>
void checkParticleCoupling() {
    using Handler = ippl::LagrangeDOFHandler<double, Dim, P>;
    using Field   = typename Handler::FEMContainer_t;
    using Point   = ippl::Vector<double, Dim>;
    using Element = std::tuple_element_t<
        Dim - 1, std::tuple<ippl::EdgeElement<double>, ippl::QuadrilateralElement<double>,
                            ippl::HexahedralElement<double>>>;
    using Quad               = ippl::SelectableQuadrature<double, P + 1, Element>;
    using Space              = ippl::LagrangeSpace_wfc<double, Dim, P, Element, Quad, Field, Field>;
    constexpr unsigned Cells = 4;
    ippl::NDIndex<Dim> domain(ippl::Vector<unsigned, Dim>(Cells + 1));
    std::array<bool, Dim> parallel;
    parallel.fill(true);
    ippl::FieldLayout<Dim> layout(MPI_COMM_WORLD, domain, parallel, true);
    Point spacing;
    for (unsigned d = 0; d < Dim; ++d)
        spacing[d] = (1 + 0.25 * d) / Cells;
    ippl::UniformCartesian<double, Dim> mesh(domain, spacing, Point(0));
    Element element;
    Quad quad(element);
    Space space(mesh, element, quad, layout);
    Field potential(mesh, layout), load(mesh, layout), one(mesh, layout);
    std::array<ippl::FieldBC, 2 * Dim> bc;
    bc.fill(ippl::PERIODIC_FACE);
    potential.setFieldBC(bc);
    load.setFieldBC(bc);
    one.setFieldBC(bc);
    one = 1;
    Kokkos::View<Point*> positions("positions", 1), electric("electric", 1);
    Kokkos::View<double*> charges("charges", 1),
        coefficients("coefficients", space.numGlobalDOFs());
    auto host = Kokkos::create_mirror_view(coefficients);
    for (size_t i = 0; i < host.extent(0); ++i)
        host(i) = std::sin(0.7 * i);
    Kokkos::deep_copy(coefficients, host);
    Kokkos::deep_copy(charges, 0.7);
    const auto owned = space.getDOFHandler().getLocalElementDomain();
    for (const auto* family : {"gll", "equispaced"}) {
        SCOPED_TRACE(family);
        space.setInterpolationNodes(std::string_view(family) == "gll"
                                        ? ippl::LagrangeNodeFamily::GLL
                                        : ippl::LagrangeNodeFamily::Equispaced);
        const fem_oracle::Reference* ref = nullptr;
        for (const auto& c : fem_oracle::referenceCases())
            if (c.order == P && std::string_view(c.interpolationFamily) == family) {
                ref = &c.reference;
                break;
            }
        ASSERT_NE(ref, nullptr);
        const unsigned probe = ref->probePoints.size() - 1;
        auto pos             = Kokkos::create_mirror_view(positions);
        for (unsigned d = 0; d < Dim; ++d)
            pos(0)[d] = (owned[d].first() + ref->probePoints[probe]) * spacing[d];
        Kokkos::deep_copy(positions, pos);
        space.fillFromGlobalCoefficients(potential, coefficients);
        double phi = 0;
        Point expectedE(0);
        for (unsigned i = 0; i < Handler::dofsPerElement; ++i) {
            unsigned local = i, global = 0, stride = 1;
            std::array<unsigned, Dim> indices;
            for (unsigned d = 0; d < Dim; ++d) {
                const unsigned coordinate = local % (P + 1);
                indices[d] = coordinate == 0 ? 0 : coordinate == P ? 1 : coordinate + 1;
                global += ((owned[d].first() * P + coordinate) % (Cells * P)) * stride;
                stride *= Cells * P;
                local /= (P + 1);
            }
            double value = std::sin(0.7 * global);
            for (unsigned d = 0; d < Dim; ++d)
                value *= ref->basisValues[probe * (P + 1) + indices[d]];
            phi += value;
            for (unsigned deriv = 0; deriv < Dim; ++deriv) {
                double gradient = -std::sin(0.7 * global) / spacing[deriv];
                for (unsigned d = 0; d < Dim; ++d) {
                    const auto entry = probe * (P + 1) + indices[d];
                    gradient *= d == deriv ? ref->basisGradients[entry] : ref->basisValues[entry];
                }
                expectedE[deriv] += gradient;
            }
        }
        for (bool emptyOtherRanks : {false, true}) {
            const size_t count = emptyOtherRanks && ippl::Comm->rank() != 0 ? 0 : 1;
            ippl::depositFEMLoad(load, space, positions, charges, count);
            EXPECT_NEAR(innerProduct(one, load), 0.7 * (emptyOtherRanks ? 1 : ippl::Comm->size()),
                        1e-12);
            double localWork = count * 0.7 * phi, globalWork = 0;
            ippl::Comm->allreduce(localWork, globalWork, 1, std::plus<double>());
            EXPECT_NEAR(innerProduct(potential, load), globalWork, 2e-12);
            ippl::gatherFEMElectricField(potential, space, positions, electric, count);
            auto field = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), electric);
            if (count) {
                for (unsigned d = 0; d < Dim; ++d) {
                    EXPECT_NEAR(field(0)[d], expectedE[d], 2e-11);
                }
            }
            ippl::gatherFEMElectricField(one, space, positions, electric, count);
            field = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), electric);
            if (count) {
                for (unsigned d = 0; d < Dim; ++d) {
                    EXPECT_NEAR(field(0)[d], 0, 2e-11);
                }
            }
        }
        // Periodically translated coordinates must deposit the same load.
        ippl::depositFEMLoad(load, space, positions, charges, 1);
        auto before = load.deepCopy();
        for (unsigned d = 0; d < Dim; ++d)
            pos(0)[d] -= Cells * spacing[d];
        Kokkos::deep_copy(positions, pos);
        ippl::depositFEMLoad(load, space, positions, charges, 1);
        EXPECT_LT(ippl::norm(load - before), 2e-12);
    }
}

TEST(FEMParticleCoupling, P3_1D) {
    checkParticleCoupling<1, 3>();
}
TEST(FEMParticleCoupling, P5_1D) {
    checkParticleCoupling<1, 5>();
}
TEST(FEMParticleCoupling, P3_2D) {
    checkParticleCoupling<2, 3>();
}
TEST(FEMParticleCoupling, P3_3D) {
    checkParticleCoupling<3, 3>();
}

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
