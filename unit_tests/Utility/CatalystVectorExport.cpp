#include "Ippl.h"

#include "Stream/ConduitVector.h"
#include "Stream/InSitu/CatalystAdaptor.h"
#include "gtest/gtest.h"

namespace {
    using Vector = ippl::Vector<double, 3>;
    using SoA =
        ippl::VectorView<Kokkos::View<double**, Kokkos::LayoutLeft, Kokkos::HostSpace>, 3, false>;

    template <typename Layout>
    struct Particles : ippl::ParticleBase<Layout> {
        ippl::ParticleAttrib<Vector> velocity;
        explicit Particles(Layout& layout)
            : ippl::ParticleBase<Layout>(layout) {
            this->addAttribute(velocity);
            velocity.set_name("velocity");
        }
    };

    double element(conduit_cpp::Node&& node, size_t i) {
        return *static_cast<const double*>(node.element_ptr(i));
    }

    // NVCC extended lambdas must not be enclosed by GoogleTest's non-public TestBody.
    void checkFieldPackingOrder() {
        using DeviceView = ippl::detail::ViewType<Vector, 2>::view_type;
        using HostView =
            ippl::detail::ViewType<Vector, 2, Kokkos::LayoutLeft, Kokkos::HostSpace>::view_type;
        DeviceView device("field with ghosts", 5, 6);
        using Index = ippl::RangePolicy<2>::index_array_type;
        ippl::parallel_for(
            "export input", ippl::getRangePolicy(device), KOKKOS_LAMBDA(const Index& ij) {
                device(ij[0], ij[1]) = Vector(ij[0], ij[1], 100 * ij[0] + ij[1]);
            });
        HostView packed("field without ghosts", 3, 4);
        Kokkos::deep_copy(packed, Kokkos::subview(device, Kokkos::pair<int, int>(1, 4),
                                                  Kokkos::pair<int, int>(1, 5)));
        conduit_cpp::Node node;
        ippl::detail::setConduitVectorValues(node["values"], packed, 12);
        for (unsigned j = 0; j < 4; ++j) {
            for (unsigned i = 0; i < 3; ++i) {
                EXPECT_EQ(element(node["values/x"], i + 3 * j), i + 1);
                EXPECT_EQ(element(node["values/y"], i + 3 * j), j + 1);
                EXPECT_EQ(element(node["values/z"], i + 3 * j), 100 * (i + 1) + j + 1);
            }
        }
    }
}  // namespace

TEST(CatalystVectorExport, AoSAndSoAComponentsAndEmptyViews) {
    SoA soa("export capacity", 11);
    Kokkos::View<Vector*, Kokkos::HostSpace> aos("AoS export", 11);
    for (int i = 0; i < 11; ++i)
        soa(i) = aos(i) = Vector(i, 100 + i, 200 + i);
    auto liveSoA = Kokkos::subview(soa, Kokkos::pair<int, int>(2, 7));
    auto liveAoS = Kokkos::subview(aos, Kokkos::pair<int, int>(2, 7));
    conduit_cpp::Node node;
    ippl::detail::setConduitVectorValues(node["soa"], liveSoA, 5);
    ippl::detail::setConduitVectorValues(node["aos"], liveAoS, 5);
    const char* names[] = {"x", "y", "z"};
    for (unsigned d = 0; d < 3; ++d) {
        EXPECT_EQ(node[std::string("soa/") + names[d]].dtype().stride(), sizeof(double));
        EXPECT_EQ(node[std::string("aos/") + names[d]].dtype().stride(), sizeof(Vector));
        for (int i = 0; i < 5; ++i) {
            EXPECT_EQ(element(node[std::string("soa/") + names[d]], i), 2 + i + 100 * d);
            EXPECT_EQ(element(node[std::string("aos/") + names[d]], i), 2 + i + 100 * d);
        }
    }
    EXPECT_EQ(node["soa/y"].element_ptr(0), liveSoA.component(1).data());
    SoA empty("empty export", 0);
    ippl::detail::setConduitVectorValues(node["empty"], empty, 0);
    for (auto name : names)
        EXPECT_EQ(node[std::string("empty/") + name].dtype().number_of_elements(), 0);
}

TEST(CatalystVectorExport, FieldPackingOrder) {
    checkFieldPackingOrder();
}

TEST(CatalystVectorExport, AttributeMirrorLifetimeAndEmptyRanks) {
    ippl::detail::size_type count = ippl::Comm->rank() == 0 ? 3 : 0;
    ippl::ViewRegistry registry;
    conduit_cpp::Node node;
    Inform info("export-test"), warn("export-test");
    {
        ippl::ParticleAttrib<Vector> attribute;
        attribute.set_name("velocity");
        attribute.setParticleCount(count);
        attribute.resize(9);
        attribute = Vector(1., 2., 3.);
        attribute.signConduitBlueprintNode(count, node, registry, info, warn, true);
        attribute = Vector(99.);
    }
    // The registry must keep the snapshot alive after its source attribute is gone.
    EXPECT_EQ(node["velocity/values/x"].dtype().number_of_elements(), count);
    for (size_t i = 0; i < count; ++i)
        EXPECT_EQ(element(node["velocity/values/y"], i), 2.);
}

TEST(CatalystVectorExport, AdaptorChannelsAndRefresh) {
    using Mesh   = ippl::UniformCartesian<double, 3>;
    using Layout = ippl::ParticleSpatialLayout<double, 3, Mesh>;
    const ippl::NDIndex<3> domain(ippl::Index(8), ippl::Index(8), ippl::Index(8));
    ippl::FieldLayout<3> layout(*ippl::Comm, domain, std::array<bool, 3>{true, true, true}, true);
    Mesh mesh(domain, Vector(1.), Vector(0.));
    ippl::Field<Vector, 3, Mesh, Mesh::DefaultCentering> field(mesh, layout);
    Layout particleLayout(layout, mesh);
    Particles<Layout> particles(particleLayout);
    particles.create(ippl::Comm->rank() == 0 ? 3 : 0);
    particles.R        = Vector(1.);
    particles.velocity = Vector(2., 3., 4.);
    for (const char* ghosts : {"OFF", "ON"}) {
        setenv("IPPL_CATALYST_GHOST_MASKS", ghosts, 1);
        field      = Vector(1., 2., 3.);
        auto vis   = ippl::MakeVisRegistryRuntimePtr("field", field, "particles", particles);
        auto steer = ippl::MakeVisRegistryRuntimePtr();
        ippl::CatalystAdaptor adaptor;
        adaptor.Initialize(vis, steer);
        adaptor.Execute(0, 0.);
        field = Vector(4., 5., 6.);
        adaptor.rememberNow("field");
        adaptor.rememberNow("particles");
        adaptor.Execute(1, 0.1);
        adaptor.Finalize();
    }
}

int main(int argc, char* argv[]) {
    setenv("IPPL_CATALYST_PROXY_OPTION", "OFF", 1);
    setenv("IPPL_CATALYST_VERBOSITY", "0", 1);
    setenv("CATALYST_IMPLEMENTATION_NAME", "stub", 1);
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
