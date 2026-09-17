#include "Ippl.h"

#include "TestUtils.h"

template <typename Tag>
class FEMThinHalo : public ::testing::Test {};
using ThinDimensions =
    ::testing::Types<std::integral_constant<unsigned, 2>, std::integral_constant<unsigned, 3>>;
TYPED_TEST_SUITE(FEMThinHalo, ThinDimensions);

template <unsigned Dim>
void checkThinHalo() {
    ippl::NDIndex<Dim> domain;
    for (unsigned d = 0; d < Dim; ++d)
        domain[d] = ippl::Index(3);
    std::array<bool, Dim> parallel;
    parallel.fill(true);
    ippl::FieldLayout<Dim> layout(MPI_COMM_WORLD, domain, parallel, false);
    ippl::UniformCartesian<double, Dim> mesh(domain, ippl::Vector<double, Dim>(1),
                                             ippl::Vector<double, Dim>(0));
    ippl::Field<double, Dim, decltype(mesh), Cell> field(mesh, layout);
    auto view          = field.getView();
    auto host          = Kokkos::create_mirror_view(view);
    const auto owned   = layout.getLocalNDIndex();
    const auto domains = layout.getHostLocalDomains();
    // A classification error must fail collectively before attempting communication.
    int invalid           = 0;
    const auto& neighbors = layout.getNeighbors();
    for (size_t index = 0; index < neighbors.size(); ++index) {
        for (int rank : neighbors[index]) {
            size_t code = index;
            for (unsigned d = 0; d < Dim; ++d) {
                const unsigned direction = domains(rank)[d].last() < owned[d].first()   ? 0
                                           : domains(rank)[d].first() > owned[d].last() ? 1
                                                                                        : 2;
                invalid += code % 3 != direction;
                code /= 3;
            }
        }
    }
    MPI_Allreduce(MPI_IN_PLACE, &invalid, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    ASSERT_EQ(invalid, 0);
    auto globalIndex = [&](auto... args) {
        std::array<int, Dim> local{static_cast<int>(args)...};
        ippl::Vector<int, Dim> global;
        for (unsigned d = 0; d < Dim; ++d)
            global[d] = local[d] - field.getNghost() + owned[d].first();
        return global;
    };
    auto value = [](const auto& global) {
        double result = 1, stride = 1;
        for (unsigned d = 0; d < Dim; ++d) {
            result += stride * global[d];
            stride *= 7;
        }
        return result;
    };
    Kokkos::deep_copy(host, 0.0);
    nestedViewLoop(host, field.getNghost(), [&](auto... args) {
        host(args...) = value(globalIndex(args...));
    });
    Kokkos::deep_copy(view, host);
    field.fillHalo();
    Kokkos::deep_copy(host, view);
    nestedViewLoop(host, 0, [&](auto... args) {
        const auto global = globalIndex(args...);
        bool inside       = true;
        for (unsigned d = 0; d < Dim; ++d)
            inside &= global[d] >= 0 && global[d] < 3;
        EXPECT_EQ(host(args...), inside ? value(global) : 0.0);
        host(args...) = inside ? 1.0 : 0.0;
    });
    Kokkos::deep_copy(view, host);
    field.accumulateHalo();
    Kokkos::deep_copy(host, view);
    nestedViewLoop(host, field.getNghost(), [&](auto... args) {
        const auto global = globalIndex(args...);
        unsigned copies   = 0;
        for (int rank = 0; rank < ippl::Comm->size(); ++rank) {
            bool contains = true;
            for (unsigned d = 0; d < Dim; ++d)
                contains &= global[d] >= domains(rank)[d].first() - 1
                            && global[d] <= domains(rank)[d].last() + 1;
            copies += contains;
        }
        EXPECT_EQ(host(args...), copies);
    });
}
TYPED_TEST(FEMThinHalo, NeighborDirectionsFillAndCornerAccumulation) {
    checkThinHalo<TypeParam::value>();
}
int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
