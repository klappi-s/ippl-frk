#include "Ippl.h"

#include "Field/VectorField.h"

#include "FFT/NUFFTTestUtils.h"
#include "Particle/VectorAttribute.h"
#include "gtest/gtest.h"

namespace {
    using Vector           = ippl::Vector<double, 3>;
    using Mesh             = ippl::UniformCartesian<double, 3>;
    using ScalarField      = ippl::Field<double, 3, Mesh, Mesh::DefaultCentering>;
    using DefaultAttribute = ippl::VectorAttribute<double, 3>;
    using DefaultField     = ippl::VectorField<double, 3>;
    static_assert(!Kokkos::is_view<DefaultAttribute::view_type>::value);
    static_assert(Kokkos::is_view<DefaultField::view_type>::value);
    // Native AoS can use Kokkos's maximum rank without instantiating a rank-9 scalar view.
    static_assert(ippl::detail::ViewType<Vector, 8, ippl::AoS>::view_type::rank == 8);
    using Legacy = ippl::ParticleAttrib<Vector>::view_type;
#ifdef IPPL_ENABLE_VECTOR_SOA
    static_assert(!Kokkos::is_view<Legacy>::value);
#else
    static_assert(Kokkos::is_view<Legacy>::value);
#endif

    template <typename Storage>
    struct Bunch : ippl::ParticleBase<ippl::ParticleSpatialLayout<double, 3, Mesh, Storage>> {
        using Layout = ippl::ParticleSpatialLayout<double, 3, Mesh, Storage>;
        using Base   = ippl::ParticleBase<Layout>;
        using OtherStorage =
            std::conditional_t<std::is_same_v<Storage, ippl::AoS>, ippl::SoA, ippl::AoS>;
        ippl::VectorAttribute<double, 3, Storage> P;
        ippl::VectorAttribute<double, 3, OtherStorage> E;
        ippl::ParticleAttrib<double> q;
        explicit Bunch(Layout& layout)
            : Base(layout) {
            this->addAttribute(P);
            this->addAttribute(E);
            this->addAttribute(q);
            this->setParticleBC(ippl::BC::PERIODIC);
        }
    };

    // Free helpers keep extended device lambdas out of Google's protected TestBody.
    template <typename ParticleStorage, typename FieldStorage>
    void checkCoupling() {
        using Field = ippl::VectorField<double, 3, FieldStorage>;
        static_assert(std::is_same_v<typename Field::view_type::uniform_type,
                                     typename Field::uniform_type::view_type>);
        const ippl::NDIndex<3> domain(ippl::Index(8), ippl::Index(8), ippl::Index(8));
        ippl::FieldLayout<3> layout(*ippl::Comm, domain, {true, true, true}, true);
        Mesh mesh(domain, Vector(0.125), Vector(0.0));
        typename Bunch<ParticleStorage>::Layout particleLayout(layout, mesh);
        Bunch<ParticleStorage> particles(particleLayout);
        constexpr size_t COUNT = 64;
        // Empty ranks on entry exercise allocation and migration of different attribute types.
        particles.create(ippl::Comm->rank() == 0 ? COUNT : 0);
        auto positions = particles.R.getHostMirror();
        auto momenta   = particles.P.getHostMirror();
        for (size_t i = 0; i < positions.extent(0); ++i) {
            Vector r((double(i % 4) + 0.5) / 4, (double((i / 4) % 4) + 0.5) / 4,
                     (double(i / 16) + 0.5) / 4);
            positions(i) = r;
            momenta(i)   = 2.0 * r;
        }
        Kokkos::deep_copy(particles.R.getView(), positions);
        Kokkos::deep_copy(particles.P.getView(), momenta);
        particles.E = 3.0 * particles.P;
        particles.q = 1.0 / COUNT;
        particles.update();
        EXPECT_EQ(particles.getTotalNum(), COUNT);
        positions = particles.R.getHostMirror();
        momenta   = particles.P.getHostMirror();
        Kokkos::deep_copy(positions, particles.R.getView());
        Kokkos::deep_copy(momenta, particles.P.getView());
        auto migratedElectric = particles.E.getHostMirror();
        Kokkos::deep_copy(migratedElectric, particles.E.getView());
        for (size_t i = 0; i < positions.extent(0); ++i)
            for (unsigned d = 0; d < 3; ++d) {
                EXPECT_EQ(momenta(i)[d], 2.0 * positions(i)[d]);
                EXPECT_EQ(migratedElectric(i)[d], 6.0 * positions(i)[d]);
            }

        Field field(mesh, layout);
        field = Vector(1.0, 2.0, 3.0);
        ippl::gather(particles.E, field, particles.R);
        ippl::gather(particles.E, field, particles.R, true);
        auto electric = particles.E.getHostMirror();
        Kokkos::deep_copy(electric, particles.E.getView());
        for (size_t i = 0; i < electric.extent(0); ++i)
            for (unsigned d = 0; d < 3; ++d)
                EXPECT_NEAR(electric(i)[d], 2.0 * (d + 1), 1e-13);

        ScalarField density(mesh, layout);
        density = 0.0;
        ippl::scatter(particles.q, density, particles.R);
        EXPECT_NEAR(density.sum(), 1.0, 1e-13);
        // The subset/hash path used by OPALX must also accept independent position properties.
        using Hash = typename decltype(particles.q)::hash_type;
        Hash selected("even particles", (particles.getLocalNum() + 1) / 2);
        auto hostSelected = Kokkos::create_mirror_view(selected);
        for (size_t i = 0; i < selected.extent(0); ++i)
            hostSelected(i) = 2 * i;
        Kokkos::deep_copy(selected, hostSelected);
        density = 0.0;
        particles.q.scatter(
            density, particles.R,
            Kokkos::RangePolicy<typename ScalarField::execution_space>(0, selected.extent(0)),
            selected);
        double localCount = selected.extent(0), globalCount = 0;
        MPI_Allreduce(&localCount, &globalCount, 1, MPI_DOUBLE, MPI_SUM, *ippl::Comm);
        EXPECT_NEAR(density.sum(), globalCount / COUNT, 1e-13);

        // A whole-vector expression stays one assignment; reserve must preserve live values.
        particles.R.reserve(particles.getLocalNum() + 100);
        particles.R   = particles.R + 0.01 * particles.P;
        auto advanced = particles.R.getHostMirror();
        Kokkos::deep_copy(advanced, particles.R.getView());
        for (size_t i = 0; i < advanced.extent(0); ++i)
            for (unsigned d = 0; d < 3; ++d)
                EXPECT_NEAR(advanced(i)[d], 1.02 * positions(i)[d], 1e-14);

        auto px = particles.P.component(0);
        Kokkos::parallel_for(
            "component update", ippl::getRangePolicy(px),
            KOKKOS_LAMBDA(size_t i) { px(i) += 1.0; });
        auto updated = particles.P.getHostMirror();
        Kokkos::deep_copy(updated, particles.P.getView());
        for (size_t i = 0; i < updated.extent(0); ++i)
            for (unsigned d = 0; d < 3; ++d)
                EXPECT_EQ(updated(i)[d], momenta(i)[d] + (d == 0 ? 1.0 : 0.0));
        EXPECT_THROW(particles.P.component(3), std::out_of_range);
    }

    template <typename Storage, typename SpatialLayout>
    void checkFieldComponentsAndHalos() {
        using Mesh2 = ippl::UniformCartesian<double, 2>;
        using Field =
            ippl::VectorField<double, 2, Storage, Mesh2, Mesh2::DefaultCentering, 3, SpatialLayout>;
        static_assert(std::is_same_v<typename Field::view_type::uniform_type,
                                     typename Field::uniform_type::view_type>);
        const ippl::NDIndex<2> domain(ippl::Index(8), ippl::Index(6));
        ippl::FieldLayout<2> layout(*ippl::Comm, domain, {true, true}, true);
        Mesh2 mesh(domain, ippl::Vector<double, 2>(1.0), ippl::Vector<double, 2>(0.0));
        Field field(mesh, layout);
        field  = Vector(-999.0);
        auto x = field.component(0), y = field.component(1), z = field.component(2);
        const auto first = layout.getLocalNDIndex().first();
        using Index      = ippl::RangePolicy<2>::index_array_type;
        ippl::parallel_for(
            "component field", field.getFieldRangePolicy(), KOKKOS_LAMBDA(const Index& ij) {
                x(ij[0], ij[1]) = first[0] + ij[0] - 1;
                y(ij[0], ij[1]) = first[1] + ij[1] - 1;
                z(ij[0], ij[1]) = 100 * x(ij[0], ij[1]) + y(ij[0], ij[1]);
            });
        field.fillHalo();
        auto host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, field.getView());
        for (int i = 0; i < host.extent_int(0); ++i)
            for (int j = 0; j < host.extent_int(1); ++j) {
                const int expectedX = (first[0] + i - 1 + 8) % 8;
                const int expectedY = (first[1] + j - 1 + 6) % 6;
                EXPECT_EQ(host(i, j)[0], expectedX);
                EXPECT_EQ(host(i, j)[1], expectedY);
                EXPECT_EQ(host(i, j)[2], 100 * expectedX + expectedY);
            }
        auto copy             = field.deepCopy();
        field                 = Vector(1.0, 2.0, 3.0);
        const Vector expected = field.sum(1);
        field.accumulateHalo();
        const Vector actual = field.sum();
        for (unsigned d = 0; d < 3; ++d)
            EXPECT_EQ(actual[d], expected[d]);
        EXPECT_NE(copy.getView().data(), field.getView().data());
        EXPECT_THROW(field.component(3), std::out_of_range);
    }

    void checkMixedExpression() {
        ippl::VectorAttribute<double, 3, ippl::AoS> a;
        ippl::VectorAttribute<double, 3, ippl::SoA> b;
        ippl::detail::size_type count = 7;
        a.setParticleCount(count);
        b.setParticleCount(count);
        a.realloc(count);
        b.realloc(count);
        a         = Vector(1., 2., 3.);
        b         = Vector(4., 5., 6.);
        a         = a + 0.5 * b;
        b         = a + b;
        auto host = b.getHostMirror();
        Kokkos::deep_copy(host, b.getView());
        for (size_t i = 0; i < count; ++i)
            for (unsigned d = 0; d < 3; ++d)
                EXPECT_EQ(host(i)[d], d + 1.0 + 1.5 * (d + 4.0));
    }

    void checkMixedNUFFT() {
        using Mesh2     = ippl::UniformCartesian<double, 2>;
        using Real      = ippl::Field<double, 2, Mesh2, Mesh2::DefaultCentering>;
        using Transform = ippl::FFT<ippl::NUFFTransform, Real>;
        using Complex   = typename Transform::ComplexField;
        const ippl::NDIndex<2> domain(ippl::Index(16), ippl::Index(16));
        ippl::FieldLayout<2> layout(*ippl::Comm, domain, {true, true}, true);
        const double twoPi = 2.0 * Kokkos::numbers::pi_v<double>;
        Mesh2 mesh(domain, ippl::Vector<double, 2>(twoPi / 16), ippl::Vector<double, 2>(0.0));
        ippl::VectorAttribute<double, 2, ippl::AoS> aos;
        ippl::VectorAttribute<double, 2, ippl::SoA> soa;
        ippl::ParticleAttrib<double> q, outA, outS;
        ippl::detail::size_type count = 8;
        aos.setParticleCount(count);
        soa.setParticleCount(count);
        q.setParticleCount(count);
        outA.setParticleCount(count);
        outS.setParticleCount(count);
        aos.realloc(count);
        soa.realloc(count);
        q.realloc(count);
        outA.realloc(count);
        outS.realloc(count);
        auto host        = aos.getHostMirror();
        const auto first = layout.getLocalNDIndex().first();
        for (size_t i = 0; i < count; ++i)
            for (unsigned d = 0; d < 2; ++d)
                host(i)[d] = (first[d] + 0.25 + 0.05 * i) * twoPi / 16;
        Kokkos::deep_copy(aos.getView(), host);
        Kokkos::deep_copy(soa.getView(), host);
        q = 1.0;
        auto params =
            ippl::test::NUFFTParams::createNativeParams<double>(1e-6, false, "atomic", "atomic");
        Transform forward(layout, count, 1, params), backward(layout, count, 2, params);
        Complex fieldA(mesh, layout), fieldS(mesh, layout);
        fieldA = Kokkos::complex<double>(0.0);
        fieldS = Kokkos::complex<double>(0.0);
        forward.transform(aos, q, fieldA);
        forward.transform(soa, q, fieldS);
        auto a = fieldA.getHostMirror(), s = fieldS.getHostMirror();
        Kokkos::deep_copy(a, fieldA.getView());
        Kokkos::deep_copy(s, fieldS.getView());
        for (int i = 1; i < a.extent_int(0) - 1; ++i)
            for (int j = 1; j < a.extent_int(1) - 1; ++j) {
                EXPECT_NEAR(a(i, j).real(), s(i, j).real(), 1e-11);
                EXPECT_NEAR(a(i, j).imag(), s(i, j).imag(), 1e-11);
            }
        backward.transform(aos, outA, fieldA);
        backward.transform(soa, outS, fieldS);
        auto ha = outA.getHostMirror(), hs = outS.getHostMirror();
        Kokkos::deep_copy(ha, outA.getView());
        Kokkos::deep_copy(hs, outS.getView());
        for (size_t i = 0; i < count; ++i)
            EXPECT_NEAR(ha(i), hs(i), 1e-9 * std::max(1.0, std::abs(ha(i))));
    }
}  // namespace

TEST(VectorLayout, AoSParticlesAoSField) {
    checkCoupling<ippl::AoS, ippl::AoS>();
}
TEST(VectorLayout, SoAParticlesAoSField) {
    checkCoupling<ippl::SoA, ippl::AoS>();
}
TEST(VectorLayout, AoSParticlesSoAField) {
    checkCoupling<ippl::AoS, ippl::SoA>();
}
TEST(VectorLayout, SoAParticlesSoAField) {
    checkCoupling<ippl::SoA, ippl::SoA>();
}
TEST(VectorLayout, MixedAttributeExpression) {
    checkMixedExpression();
}
TEST(VectorLayout, MixedNUFFT) {
    checkMixedNUFFT();
}
TEST(VectorLayout, AoSLeftHalos) {
    checkFieldComponentsAndHalos<ippl::AoS, Kokkos::LayoutLeft>();
}
TEST(VectorLayout, AoSRightHalos) {
    checkFieldComponentsAndHalos<ippl::AoS, Kokkos::LayoutRight>();
}
TEST(VectorLayout, SoALeftHalos) {
    checkFieldComponentsAndHalos<ippl::SoA, Kokkos::LayoutLeft>();
}
TEST(VectorLayout, SoARightHalos) {
    checkFieldComponentsAndHalos<ippl::SoA, Kokkos::LayoutRight>();
}

int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
