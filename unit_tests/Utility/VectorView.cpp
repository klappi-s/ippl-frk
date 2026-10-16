#include "Ippl.h"

#include "Types/VectorView.h"

#include <sstream>

#include "Utility/ViewUtils.h"

#include "MaxwellSolvers/AbsorbingBC.h"
#include "gtest/gtest.h"

namespace {
    template <typename T, typename Layout, unsigned Rank>
    using View =
        ippl::VectorView<Kokkos::View<typename ippl::detail::NPtr<T, Rank + 1>::type, Layout>, 3,
                         std::is_same_v<Layout, Kokkos::LayoutRight>>;

    template <typename T, typename Layout>
    void checkParticleStorage() {
        static_assert(!Kokkos::is_view<typename View<T, Layout, 1>::uniform_type>::value);
        View<T, Layout, 1> values("particle components", 11);
        Kokkos::parallel_for(
            "initialize components", ippl::getRangePolicy(values), KOKKOS_LAMBDA(size_t i) {
                values(i) = ippl::Vector<T, 3>(T(i), T(100 + i), T(200 + i));
            });
        auto live = Kokkos::subview(values, Kokkos::pair<size_t, size_t>(2, 7));
        EXPECT_EQ(live.extent(0), 5);
        EXPECT_EQ(live.component(1).stride(0), 1);
        EXPECT_EQ(live.component(1).data() - live.component(0).data(),
                  values.component(1).data() - values.component(0).data());
        EXPECT_GE(values.component(1).data() - values.component(0).data(), 11);

        Kokkos::parallel_for(
            "proxy value copy and aliasing", Kokkos::RangePolicy<>(0, 1), KOKKOS_LAMBDA(size_t) {
                live(1)    = live(0);
                live(2)    = ippl::cross(live(2), ippl::Vector<T, 3>(T(1), T(2), T(3)));
                live(3)[1] = T(-9);
                live(4)    = {T(8), T(9), T(10)};
                Kokkos::atomic_add(&live(3)[2], T(7));
            });
        auto host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), values);
        for (unsigned d = 0; d < 3; ++d)
            EXPECT_EQ(host(3)[d], T(2 + d * 100));
        EXPECT_EQ(host(4)[0], T(104 * 3 - 204 * 2));
        EXPECT_EQ(host(4)[1], T(204 - 4 * 3));
        EXPECT_EQ(host(4)[2], T(4 * 2 - 104));
        EXPECT_EQ(host(5)[1], T(-9));
        EXPECT_EQ(host(5)[2], T(212));
        EXPECT_EQ(host(6)[0], T(8));
        EXPECT_EQ(host(6)[1], T(9));
        EXPECT_EQ(host(6)[2], T(10));

        // Resize must move component planes while preserving every live vector.
        Kokkos::resize(values, 23);
        auto grown = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), values);
        EXPECT_EQ(grown(10)[0], T(10));
        EXPECT_EQ(grown(10)[1], T(110));
        EXPECT_EQ(grown(10)[2], T(210));
        Kokkos::resize(values, 6);
        auto shrunk = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), values);
        EXPECT_EQ(shrunk(5)[1], T(-9));
        EXPECT_EQ(shrunk(5)[2], T(212));
        Kokkos::realloc(values, 0);
        EXPECT_EQ(values.size(), 0);
    }

    template <typename Layout>
    void checkFieldStorage() {
        View<double, Layout, 2> field("field components", 5, 7);
        using Index = ippl::RangePolicy<2>::index_array_type;
        ippl::parallel_for(
            "initialize vector field", ippl::getRangePolicy(field), KOKKOS_LAMBDA(const Index& ij) {
                field(ij[0], ij[1]) = ippl::Vector<double, 3>(double(ij[0]), double(ij[1]),
                                                              double(10 * ij[0] + ij[1]));
            });
        auto owned =
            Kokkos::subview(field, Kokkos::pair<int, int>(1, 4), Kokkos::pair<int, int>(2, 6));
        View<double, Layout, 2> copy("owned values", 3, 4);
        Kokkos::deep_copy(copy, owned);
        auto host = Kokkos::create_mirror(copy);
        Kokkos::deep_copy(host, copy);
        EXPECT_EQ(host(2, 3)[0], 3);
        EXPECT_EQ(host(2, 3)[1], 5);
        EXPECT_EQ(host(2, 3)[2], 35);
        Kokkos::View<ippl::Vector<double, 3>**, Kokkos::LayoutRight, Kokkos::HostSpace> aos(
            "cropped AoS export", 3, 4);
        Kokkos::deep_copy(aos, owned);
        EXPECT_EQ(aos(2, 3)[2], 35);
        if constexpr (std::is_same_v<Layout, Kokkos::LayoutLeft>) {
            EXPECT_EQ(copy.component(0).stride(0), 1);
        } else {
            EXPECT_EQ(copy.component(0).stride(1), 1);
        }
        Kokkos::deep_copy(copy, ippl::Vector<double, 3>(7., 8., 9.));
        Kokkos::deep_copy(host, copy);
        EXPECT_EQ(host(0, 0)[1], 8);
        EXPECT_EQ(host(2, 3)[2], 9);
        // A deep copy must not alias the original subview.
        auto original = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), field);
        EXPECT_EQ(original(3, 5)[2], 35);
    }

    template <unsigned Rank, unsigned Components>
    void checkRank() {
        using ScalarView = Kokkos::View<typename ippl::detail::NPtr<double, Rank + 1>::type>;
        using VectorView = ippl::VectorView<
            ScalarView, Components,
            std::is_same_v<typename ScalarView::array_layout, Kokkos::LayoutRight>>;
        auto values = []<size_t... I>(std::index_sequence<I...>) {
            return VectorView("arbitrary rank", (static_cast<void>(I), 2)...);
        }(std::make_index_sequence<Rank>{});
        using Index = typename ippl::RangePolicy<Rank>::index_array_type;
        ippl::parallel_for(
            "rank independent access", ippl::getRangePolicy(values),
            KOKKOS_LAMBDA(const Index& index) {
                ippl::apply(values, index) = 2.0;
                ippl::apply(values, index) *= 3.0;
            });
        auto host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), values);
        Index index(1);
        for (unsigned d = 0; d < Components; ++d)
            EXPECT_EQ(ippl::apply(host, index)[d], 6);
    }

    // NVCC extended lambdas must not be enclosed by GoogleTest's non-public TestBody.
    void checkAbsorbingBoundariesMatchAoS() {
        using Vector = ippl::Vector<double, 4>;
        using SoA    = ippl::VectorView<Kokkos::View<double****, Kokkos::LayoutRight>, 4, true>;
        SoA now("ABC now", 5, 5, 5), previous("ABC previous", 5, 5, 5), next("ABC next", 5, 5, 5);
        using Index = ippl::RangePolicy<3>::index_array_type;
        ippl::parallel_for(
            "ABC input", ippl::getRangePolicy(now), KOKKOS_LAMBDA(const Index& ijk) {
                for (unsigned d = 0; d < 4; ++d) {
                    const double value            = 1 + d + ijk[0] + 2 * ijk[1] + 3 * ijk[2];
                    ippl::apply(now, ijk)[d]      = value;
                    ippl::apply(previous, ijk)[d] = 0.7 * value;
                    ippl::apply(next, ijk)[d]     = 1.3 * value;
                }
            });
        Kokkos::View<Vector***> aosNow("ABC AoS now", 5, 5, 5);
        Kokkos::View<Vector***> aosPrevious("ABC AoS previous", 5, 5, 5);
        Kokkos::View<Vector***> aosNext("ABC AoS next", 5, 5, 5);
        Kokkos::deep_copy(aosNow, now);
        Kokkos::deep_copy(aosPrevious, previous);
        Kokkos::deep_copy(aosNext, next);
        Kokkos::View<double**> result("ABC results", 6, 4);
        Kokkos::parallel_for(
            "ABC compare", Kokkos::RangePolicy<>(0, 1), KOKKOS_LAMBDA(size_t) {
                const ippl::Vector<double, 3> spacing(1., 2., 3.);
                const ippl::Vector<unsigned, 3> point(2, 2, 2);
                second_order_abc_face<double, 0, 1, 2> face(spacing, 0.1, 1);
                second_order_abc_edge<double, 0, 1, 2, true, false> edge(spacing, 0.1);
                second_order_abc_corner<double, true, false, true> corner(spacing, 0.1);
                const Vector values[6] = {face(now, previous, next, point),
                                          edge(now, previous, next, point),
                                          corner(now, previous, next, point),
                                          face(aosNow, aosPrevious, aosNext, point),
                                          edge(aosNow, aosPrevious, aosNext, point),
                                          corner(aosNow, aosPrevious, aosNext, point)};
                for (unsigned i = 0; i < 6; ++i)
                    for (unsigned d = 0; d < 4; ++d)
                        result(i, d) = values[i][d];
            });
        auto host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, result);
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned d = 0; d < 4; ++d)
                EXPECT_DOUBLE_EQ(host(i, d), host(i + 3, d));
    }

    void checkArchiveInteroperabilityAndOffsets() {
        using Vector = ippl::Vector<double, 3>;
        using Memory = Kokkos::DefaultExecutionSpace::memory_space;
        View<double, Kokkos::LayoutRight, 1> values("archive input", 12);
        Kokkos::parallel_for(
            "archive values", ippl::getRangePolicy(values), KOKKOS_LAMBDA(size_t i) {
                values(i) = Vector(double(i), double(100 + i), double(200 + i));
            });
        ippl::detail::Archive<Memory> archive(12 * 3 * sizeof(double));
        archive.serialize(values, 7);
        EXPECT_EQ(archive.getSize(), 7 * 3 * sizeof(double));
        Kokkos::View<Vector*, Memory> aos;
        archive.deserialize(aos, 7);
        auto host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), aos);
        for (int i = 0; i < 7; ++i) {
            for (int d = 0; d < 3; ++d)
                EXPECT_EQ(host(i)[d], i + 100 * d);
        }

        Kokkos::View<int*, Memory> hash("sparse selection", 3);
        auto hostHash = Kokkos::create_mirror(hash);
        hostHash(0)   = 6;
        hostHash(1)   = 2;
        hostHash(2)   = 4;
        Kokkos::deep_copy(hash, hostHash);
        archive.resetReadPos();
        archive.resetWritePos();
        archive.serialize(aos, hash, 3);
        View<double, Kokkos::LayoutLeft, 1> received("archive destination", 2);
        Kokkos::deep_copy(received, Vector(-1.0));
        archive.deserialize(received, 2, 3);
        auto result = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), received);
        EXPECT_EQ(result.extent(0), 5);
        for (int d = 0; d < 3; ++d) {
            EXPECT_EQ(result(0)[d], -1);
            EXPECT_EQ(result(1)[d], -1);
            for (int i = 0; i < 3; ++i)
                EXPECT_EQ(result(i + 2)[d], hostHash(i) + 100 * d);
        }
        archive.resetReadPos();
        archive.resetWritePos();
        archive.serialize(values, hash, 3);
        archive.deserialize(aos, 3);
        host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), aos);
        for (int i = 0; i < 3; ++i) {
            for (int d = 0; d < 3; ++d)
                EXPECT_EQ(host(i)[d], hostHash(i) + 100 * d);
        }
    }

    void checkVectorFieldPeriodicHalosAndAccumulation() {
        using Vector = ippl::Vector<double, 3>;
        using Mesh   = ippl::UniformCartesian<double, 2>;
        using Field  = ippl::Field<Vector, 2, Mesh, Mesh::DefaultCentering>;
        const ippl::NDIndex<2> domain(ippl::Index(8), ippl::Index(6));
        ippl::FieldLayout<2> layout(*ippl::Comm, domain, std::array<bool, 2>{true, true}, true);
        Mesh mesh(domain, ippl::Vector<double, 2>(1.0), ippl::Vector<double, 2>(0.0));
        Field field(mesh, layout);
        field            = Vector(-999.0);
        auto view        = field.getView();
        const auto first = layout.getLocalNDIndex().first();
        using Index      = ippl::RangePolicy<2>::index_array_type;
        ippl::parallel_for(
            "vector halo interior", field.getFieldRangePolicy(), KOKKOS_LAMBDA(const Index& ij) {
                const double x     = first[0] + ij[0] - 1;
                const double y     = first[1] + ij[1] - 1;
                view(ij[0], ij[1]) = Vector(x, y, 100 * x + y);
            });
        field.fillHalo();
        auto host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), view);
        for (int i = 0; i < host.extent_int(0); ++i) {
            for (int j = 0; j < host.extent_int(1); ++j) {
                const int x = (first[0] + i - 1 + 8) % 8;
                const int y = (first[1] + j - 1 + 6) % 6;
                EXPECT_EQ(host(i, j)[0], x);
                EXPECT_EQ(host(i, j)[1], y);
                EXPECT_EQ(host(i, j)[2], 100 * x + y);
            }
        }
        field                 = Vector(1.0, 2.0, 3.0);
        const Vector expected = field.sum(1);
        field.accumulateHalo();
        const Vector actual = field.sum();
        for (int d = 0; d < 3; ++d)
            EXPECT_EQ(actual[d], expected[d]);
    }
}  // namespace

TEST(VectorView, ParticleLeftFloat) {
    checkParticleStorage<float, Kokkos::LayoutLeft>();
}
TEST(VectorView, ParticleRightFloat) {
    checkParticleStorage<float, Kokkos::LayoutRight>();
}
TEST(VectorView, ParticleLeftDouble) {
    checkParticleStorage<double, Kokkos::LayoutLeft>();
}
TEST(VectorView, ParticleRightDouble) {
    checkParticleStorage<double, Kokkos::LayoutRight>();
}
TEST(VectorView, FieldLeft) {
    checkFieldStorage<Kokkos::LayoutLeft>();
}
TEST(VectorView, FieldRight) {
    checkFieldStorage<Kokkos::LayoutRight>();
}
TEST(VectorView, StreamOutput) {
    View<double, Kokkos::LayoutRight, 2> values("print vectors", 2, 2);
    Kokkos::deep_copy(values, ippl::Vector<double, 3>(7., 8., 9.));
    std::ostringstream plain, list;
    ippl::detail::write<ippl::Vector<double, 3>, 2>(values, plain);
    ippl::detail::write_as_list<ippl::Vector<double, 3>, 2>(values, list);
    EXPECT_NE(plain.str().find('9'), std::string::npos);
    EXPECT_NE(list.str().find('9'), std::string::npos);
    EXPECT_EQ(list.str().front(), '[');
    EXPECT_EQ(list.str().back(), ']');
}
TEST(VectorView, RanksAndComponentCounts) {
    checkRank<1, 1>();
    checkRank<2, 2>();
    checkRank<3, 4>();
    checkRank<4, 3>();
    checkRank<5, 3>();
    checkRank<6, 3>();
}

TEST(VectorView, AbsorbingBoundariesMatchAoS) {
    checkAbsorbingBoundariesMatchAoS();
}

TEST(VectorView, LayoutAndAoSCopies) {
    using Vector = ippl::Vector<double, 3>;
    Kokkos::View<Vector*, Kokkos::HostSpace> input("external AoS", 9);
    for (int i = 0; i < 9; ++i)
        input(i) = Vector(double(i), double(i + 10), double(i + 20));
    View<double, Kokkos::LayoutLeft, 1> left("left", 9);
    View<double, Kokkos::LayoutRight, 1> right("right", 9);
    Kokkos::deep_copy(left, input);
    Kokkos::deep_copy(right, left);
    Kokkos::View<Vector*, Kokkos::HostSpace> output("output AoS", 9);
    Kokkos::deep_copy(output, right);
    for (int i = 0; i < 9; ++i) {
        for (int d = 0; d < 3; ++d)
            EXPECT_EQ(output(i)[d], input(i)[d]);
    }
    View<double, Kokkos::LayoutLeft, 1> wrongSize("wrong size", 8);
    EXPECT_THROW(Kokkos::deep_copy(left, wrongSize), std::invalid_argument);
    // Native const scalar views enforce constness through the proxy as well.
    using ConstView = ippl::VectorView<Kokkos::View<const double**, Kokkos::LayoutLeft>, 3, false>;
    ConstView readOnly(left);
    static_assert(std::is_same_v<decltype(readOnly(0)[0]), const double&>);
    Kokkos::deep_copy(output, readOnly);
    EXPECT_EQ(output(8)[2], 28);
}

TEST(VectorView, ArchiveInteroperabilityAndOffsets) {
    checkArchiveInteroperabilityAndOffsets();
}

TEST(VectorView, VectorFieldPeriodicHalosAndAccumulation) {
    checkVectorFieldPeriodicHalosAndAccumulation();
}

int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
