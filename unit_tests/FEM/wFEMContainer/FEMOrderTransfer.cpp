// Check same-mesh FEM order transfer through the shared device entity-view helpers.
// Periodic MPI1/MPI2 runs cover const input gathers, output stores and halo refresh.
#include "Ippl.h"
#include "LinearSolvers/FEMOrderTransfer.h"
#include "gtest/gtest.h"

namespace {
    template <unsigned Dim>
    void checkConstantRoundTrip() {
        using Transfer = ippl::FEMOrderTransfer<double, Dim, 2, 1>;
        using Fine = typename Transfer::ContainerFine;
        using Coarse = typename Transfer::ContainerCoarse;
        using Point = ippl::Vector<double, Dim>;
        ippl::NDIndex<Dim> domain;
        std::array<bool, Dim> parallel;
        for (unsigned d = 0; d < Dim; ++d) {
            domain[d] = ippl::Index(9);
            parallel[d] = true;
        }
        ippl::FieldLayout<Dim> layout(MPI_COMM_WORLD, domain, parallel, true);
        ippl::UniformCartesian<double, Dim> mesh(domain, Point(0.125), Point(0));
        typename Transfer::HandlerFine handlerFine(mesh, layout);
        typename Transfer::HandlerCoarse handlerCoarse(mesh, layout);
        Fine fine(mesh, layout), roundTrip(mesh, layout), expectedFine(mesh, layout);
        Coarse coarse(mesh, layout), expectedCoarse(mesh, layout);
        std::array<ippl::FieldBC, 2 * Dim> bc;
        bc.fill(ippl::PERIODIC_FACE);
        fine.setFieldBC(bc);
        coarse.setFieldBC(bc);
        roundTrip.setFieldBC(bc);
        const Kokkos::View<const size_t*> elements = handlerFine.getElementIndices();
        Transfer transfer;
        for (const double value : {2.5, -1.25, 0.0}) {
            fine = value;
            fine.fillHalo();
            coarse = 31;
            roundTrip = -17;
            expectedFine = value;
            expectedCoarse = value;
            transfer.restrict(fine, coarse, handlerFine, handlerCoarse, elements);
            EXPECT_NEAR(ippl::norm(coarse - expectedCoarse), 0, 1e-12);
            transfer.prolong(coarse, roundTrip, handlerFine, handlerCoarse, elements);
            EXPECT_NEAR(ippl::norm(roundTrip - expectedFine), 0, 1e-12);
            EXPECT_DOUBLE_EQ(ippl::norm(fine - expectedFine), 0);
        }
    }
}

// P2->P1->P2 must preserve positive, negative and zero constants across periodic
// rank boundaries in the configured backend. P1/P2 GLL and equispaced nodes coincide;
// the current transfer API has no independent node-family selector.
TEST(FEMOrderTransfer, PeriodicConstantRoundTrip1D) { checkConstantRoundTrip<1>(); }

// Exercise vertex, both edge orientations and face fields through the same
// const/mutable tuple helpers with MPI1/MPI2 and repeated poisoned outputs.
TEST(FEMOrderTransfer, PeriodicConstantRoundTrip2D) { checkConstantRoundTrip<2>(); }

// Exercise all eight 3D entity types and their shared element/rank boundaries.
TEST(FEMOrderTransfer, PeriodicConstantRoundTrip3D) { checkConstantRoundTrip<3>(); }

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
