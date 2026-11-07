// Check owned FEM scalar reductions across all entity types, dimensions and
// orders, excluding poisoned ghosts and retaining global MPI sums.
#include "Ippl.h"
#include "FEM/DOFHandler.h"
#include "gtest/gtest.h"

namespace {
    template <unsigned Dim, unsigned Order>
    void checkReductions() {
        using Handler = ippl::LagrangeDOFHandler<double, Dim, Order>;
        using Field = typename Handler::FEMContainer_t;
        using Entities = decltype(Field::getEntityTypes());
        ippl::NDIndex<Dim> domain;
        for (unsigned d = 0; d < Dim; ++d) domain[d] = ippl::Index(7 + d);
        std::array<bool, Dim> parallel;
        parallel.fill(true);
        for (bool periodic : {false, true}) {
            ippl::FieldLayout<Dim> layout(MPI_COMM_WORLD, domain, parallel, periodic);
            ippl::UniformCartesian<double, Dim> mesh(domain, ippl::Vector<double, Dim>(.25),
                                                       ippl::Vector<double, Dim>(0));
            for (int ghosts : {1, 2}) {
                SCOPED_TRACE("Dim=" + std::to_string(Dim) + " P=" + std::to_string(Order)
                             + " ghosts=" + std::to_string(ghosts) + " periodic=" + std::to_string(periodic));
                Field a(mesh, layout, ghosts), b(mesh, layout, ghosts);
                double expectedLocal[5] = {};
                size_t countLocal = 0;
                [&]<size_t... Is>(std::index_sequence<Is...>) {
                    ([&] {
                        using Entity = std::tuple_element_t<Is, Entities>;
                        auto av = a.template getView<Entity>();
                        auto bv = b.template getView<Entity>();
                        using Array = typename decltype(av)::value_type;
                        // Ghosts deliberately differ from valid data by many orders.
                        Kokkos::deep_copy(av, Array(1e6));
                        Kokkos::deep_copy(bv, Array(-2e6));
                        auto ah = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), av);
                        auto bh = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), bv);
                        const auto owned = a.template getLayout<Entity>().getLocalNDIndex();
                        size_t positions = 1;
                        for (unsigned d = 0; d < Dim; ++d) {
                            EXPECT_EQ(av.extent(d), static_cast<size_t>(owned[d].length() + 2 * ghosts));
                            positions *= owned[d].length();
                        }
                        const unsigned dofs = a.template getNumDOFs<Entity>();
                        for (size_t flat = 0; flat < positions; ++flat) {
                            size_t remaining = flat;
                            ippl::Vector<size_t, Dim> index;
                            double coordinate = 0;
                            for (unsigned d = 0; d < Dim; ++d) {
                                index[d] = remaining % owned[d].length();
                                remaining /= owned[d].length();
                                coordinate += owned[d].first() + index[d];
                                index[d] += ghosts;
                            }
                            for (unsigned dof = 0; dof < dofs; ++dof) {
                                const double x = ippl::Comm->rank() + 1 + .25 * Is - .125 * coordinate - .0625 * dof;
                                const double y = -1 + .125 * (Is + 1) + .0625 * dof + .03125 * coordinate;
                                ippl::apply(ah, index)[dof] = x;
                                ippl::apply(bh, index)[dof] = y;
                                expectedLocal[0] += x * y;
                                expectedLocal[1] += x * x;
                                expectedLocal[2] += std::abs(x);
                                expectedLocal[3] += std::pow(std::abs(x), 3);
                                expectedLocal[4] += x;
                                ++countLocal;
                            }
                        }
                        Kokkos::deep_copy(av, ah);
                        Kokkos::deep_copy(bv, bh);
                    }(), ...);
                }(std::make_index_sequence<std::tuple_size_v<Entities>>{});
                double expected[5] = {};
                ippl::Comm->allreduce<double>(expectedLocal, expected, 5, std::plus<double>());
                size_t count = 0;
                ippl::Comm->allreduce(countLocal, count, 1, std::plus<size_t>());
                size_t theoretical = 1;
                for (unsigned d = 0; d < Dim; ++d)
                    theoretical *= (domain[d].length() - 1) * Order + (periodic ? 0 : 1);
                EXPECT_EQ(count, theoretical);
                const auto near = [](double actual, double wanted) {
                    EXPECT_NEAR(actual, wanted, 5e-12 * std::max(1.0, std::abs(wanted)));
                };
                near(innerProduct(a, b), expected[0]);
                near(innerProduct(a, a), expected[1]);
                near(ippl::norm(a, 1), expected[2]);
                near(ippl::norm(a, 2), std::sqrt(expected[1]));
                near(ippl::norm(a, 3), std::pow(expected[3], 1.0 / 3));
                // Constant-weight inner product is the actual periodic CG mean sum.
                b = 1;
                near(innerProduct(b, a), expected[4]);
                near(innerProduct(b, b), static_cast<double>(theoretical));
                // A reduction must leave input storage and poisoned ghosts untouched.
                const auto av = a.template getView<ippl::Vertex<Dim>>();
                const auto ah = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), av);
                EXPECT_DOUBLE_EQ(ippl::apply(ah, ippl::Vector<size_t, Dim>(0))[0], 1e6);
            }
        }
    }
}

// P1-P5 reductions cover all centering/DOF counts in 1D, with MPI and wide ghosts.
TEST(FEMContainerReduction, AllOrders1D) {
    checkReductions<1,1>(); checkReductions<1,2>(); checkReductions<1,3>();
    checkReductions<1,4>(); checkReductions<1,5>();
}
// 2D entity layouts have different nonperiodic extents; ghosts must stay excluded.
TEST(FEMContainerReduction, AllOrders2D) {
    checkReductions<2,1>(); checkReductions<2,2>(); checkReductions<2,3>();
    checkReductions<2,4>(); checkReductions<2,5>();
}
// 3D tests include vertex, edge, face and cell DOFs and one global owned sum.
TEST(FEMContainerReduction, AllOrders3D) {
    checkReductions<3,1>(); checkReductions<3,2>(); checkReductions<3,3>();
    checkReductions<3,4>(); checkReductions<3,5>();
}

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
