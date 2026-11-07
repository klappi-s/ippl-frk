// Check fused value updates, every input/output alias, owned/ghost separation,
// and the algebraic mean projector on periodic and nonperiodic MPI layouts.
#include "Ippl.h"
#include "FEM/DOFHandler.h"
#include "gtest/gtest.h"

namespace {
    template <unsigned Dim, unsigned Order>
    void checkValueUpdates() {
        using Field = typename ippl::LagrangeDOFHandler<double, Dim, Order>::FEMContainer_t;
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
                Field x(mesh, layout, ghosts), y(mesh, layout, ghosts), out(mesh, layout, ghosts);
                std::array<ippl::FieldBC, 2 * Dim> bc;
                bc.fill(periodic ? ippl::PERIODIC_FACE : ippl::ZERO_FACE);
                out.setFieldBC(bc);
                const auto initialBC = out.getFieldBCTypes();
                // Different values in every scalar DOF and rank detect missed entity
                // ranges, while poisoned halos detect writes beyond owned ranges.
                auto visit = [&](auto operation) {
                    [&]<size_t... Is>(std::index_sequence<Is...>) {
                        (operation.template operator()<std::tuple_element_t<Is, Entities>, Is>(), ...);
                    }(std::make_index_sequence<std::tuple_size_v<Entities>>{});
                };
                auto reset = [&] {
                    visit([&]<typename Entity, size_t I>() {
                        auto xv = x.template getView<Entity>();
                        auto yv = y.template getView<Entity>();
                        auto ov = out.template getView<Entity>();
                        using Array = typename decltype(xv)::value_type;
                        Kokkos::deep_copy(xv, Array(1e5));
                        Kokkos::deep_copy(yv, Array(-2e5));
                        Kokkos::deep_copy(ov, Array(3e5));
                        auto xh = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), xv);
                        auto yh = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), yv);
                        const auto owned = x.template getLayout<Entity>().getLocalNDIndex();
                        size_t count = 1;
                        for (unsigned d = 0; d < Dim; ++d) count *= owned[d].length();
                        for (size_t flat = 0; flat < count; ++flat) {
                            size_t remaining = flat;
                            ippl::Vector<size_t, Dim> index;
                            for (unsigned d = 0; d < Dim; ++d) {
                                index[d] = ghosts + remaining % owned[d].length();
                                remaining /= owned[d].length();
                            }
                            for (unsigned dof = 0; dof < x.template getNumDOFs<Entity>(); ++dof) {
                                ippl::apply(xh,index)[dof] = 1 + ippl::Comm->rank() + .25 * I + .125 * flat + .0625 * dof;
                                ippl::apply(yh,index)[dof] = -2 - .5 * I - .25 * flat - .125 * dof;
                            }
                        }
                        Kokkos::deep_copy(xv,xh);
                        Kokkos::deep_copy(yv,yh);
                    });
                };
                auto verify = [&](const Field& result, double alpha, double beta, double shift,
                                  double ghostValue) {
                    visit([&]<typename Entity, size_t I>() {
                        const auto view = result.template getView<Entity>();
                        const auto host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(),view);
                        const auto owned = result.template getLayout<Entity>().getLocalNDIndex();
                        size_t count = 1;
                        for (unsigned d = 0; d < Dim; ++d) count *= owned[d].length();
                        for (size_t flat = 0; flat < count; ++flat) {
                            size_t remaining = flat;
                            ippl::Vector<size_t, Dim> index;
                            for (unsigned d = 0; d < Dim; ++d) {
                                index[d] = ghosts + remaining % owned[d].length();
                                remaining /= owned[d].length();
                            }
                            for (unsigned dof = 0; dof < result.template getNumDOFs<Entity>(); ++dof) {
                                const double a = 1 + ippl::Comm->rank() + .25 * I + .125 * flat + .0625 * dof;
                                const double b = -2 - .5 * I - .25 * flat - .125 * dof;
                                EXPECT_DOUBLE_EQ(ippl::apply(host,index)[dof], alpha * a + beta * b - shift);
                            }
                        }
                        // Check every halo position, not only the corner sentinel.
                        size_t total = host.size();
                        for (size_t flat = 0; flat < total; ++flat) {
                            size_t remaining = flat;
                            ippl::Vector<size_t, Dim> index;
                            bool ghost = false;
                            for (unsigned d = 0; d < Dim; ++d) {
                                index[d] = remaining % host.extent(d);
                                remaining /= host.extent(d);
                                ghost |= index[d] < static_cast<size_t>(ghosts)
                                         || index[d] >= host.extent(d) - ghosts;
                            }
                            if (ghost) {
                                for (unsigned dof = 0; dof < result.template getNumDOFs<Entity>(); ++dof) {
                                    EXPECT_DOUBLE_EQ(ippl::apply(host,index)[dof],ghostValue);
                                }
                            }
                        }
                    });
                };
                reset();
                const auto storage = out.template getView<ippl::Vertex<Dim>>().data();
                const auto* entityLayout = &out.template getLayout<ippl::Vertex<Dim>>();
                out.assignLinearCombination(.25,x,-.75,y);
                verify(out,.25,-.75,0,3e5);
                verify(x,1,0,0,1e5);
                verify(y,0,1,0,-2e5);
                EXPECT_EQ(out.template getView<ippl::Vertex<Dim>>().data(),storage);
                EXPECT_EQ(&out.template getLayout<ippl::Vertex<Dim>>(),entityLayout);
                EXPECT_EQ(out.getFieldBCTypes(),initialBC);
                x.assignLinearCombination(.25,x,-.75,y);
                verify(x,.25,-.75,0,1e5);
                reset();
                y.assignLinearCombination(.25,x,-.75,y);
                verify(y,.25,-.75,0,-2e5);
                reset();
                x.assignLinearCombination(.25,x,-.75,x);
                verify(x,-.5,0,0,1e5);
                reset();
                x.subtractOwnedValue(.375);
                verify(x,1,0,.375,1e5);
                reset();
                y = 1;
                const double mean = innerProduct(y,x) / innerProduct(y,y);
                x.subtractOwnedValue(mean);
                verify(x,1,0,mean,1e5);
                EXPECT_NEAR(innerProduct(y,x),0,2e-11 * ippl::norm(x) * ippl::norm(y));
            }
        }
    }
}

// Aliases and ghosts are checked in 1D and 2D using periodic/nonperiodic MPI layouts.
TEST(FEMContainerValueUpdate, P3LowerDimensions) {
    checkValueUpdates<1,3>(); checkValueUpdates<2,3>();
}
// P1-P5 cover every 3D entity and scalar DOF count; MPI2 exercises owned boundaries.
TEST(FEMContainerValueUpdate, AllOrders3D) {
    checkValueUpdates<3,1>(); checkValueUpdates<3,2>(); checkValueUpdates<3,3>();
    checkValueUpdates<3,4>(); checkValueUpdates<3,5>();
}

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
