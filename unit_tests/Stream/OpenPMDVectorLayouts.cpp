//
// Unit test: openPMD output for independent AoS/SoA particle and field layouts
//
// Checks all four storage combinations with LayoutLeft/LayoutRight spatial indexing,
// mixed-layout HostSpace attributes, empty ranks, spare capacity and reallocation.
// Direct packing and exact readback verify ghost exclusion, component order and metadata.
// IPPL_TEST_BP5 selects BP5; otherwise the suite uses HDF5.
//

#include <cstdlib>
#include "Ippl.h"
#include "Stream/OpenPMD/OpenPMDReader.h"
#include "Stream/OpenPMD/OpenPMDWriter.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace {
    template <class Storage>
    using ParticleLayout = ippl::detail::ParticleLayout<double, 3, Storage>;

    template <class Storage>
    struct Particles : ippl::ParticleBase<ParticleLayout<Storage>, Kokkos::HostSpace> {
        using Base = ippl::ParticleBase<ParticleLayout<Storage>, Kokkos::HostSpace>;
        using OtherStorage = std::conditional_t<std::is_same_v<Storage, ippl::AoS>,
                                                ippl::SoA, ippl::AoS>;
        ippl::VectorAttribute<double, 3, Storage, Kokkos::LayoutRight> velocity_m;
        ippl::VectorAttribute<float, 2, OtherStorage, Kokkos::HostSpace> diagnostic_m;

        explicit Particles(ParticleLayout<Storage>& layout) : Base(layout) {
            velocity_m.set_name("velocity");
            diagnostic_m.set_name("diagnostic");
            this->addAttribute(velocity_m);
            this->addAttribute(diagnostic_m);
        }
    };

    template <class P>
    void fillParticles(P& particles, double bias) {
        const auto r = particles.R.getView();
        const auto v = particles.velocity_m.getView();
        const int rank = ippl::Comm->rank();
        Kokkos::parallel_for(
            "layout output particles", ippl::getRangePolicy(r),
            KOKKOS_LAMBDA(std::size_t i) {
                for (unsigned d = 0; d < 3; ++d) {
                    r(i)[d] = bias + rank * 100 + 4 * i + d;
                    v(i)[d] = -r(i)[d];
                }
            });
        auto hostDiagnostic = particles.diagnostic_m.getView();
        for (std::size_t i = 0; i < particles.getLocalNum(); ++i) {
            hostDiagnostic(i)[0] = float(i + rank * 100);
            hostDiagnostic(i)[1] = -hostDiagnostic(i)[0];
        }
    }

    template <class F>
    void fillField(F& field, float bias) {
        const auto view = field.getView();
        const auto owned = field.getOwned();
        const int ghost = field.getNghost();
        const int firstX = owned[0].first(), firstY = owned[1].first();
        using Index = typename ippl::RangePolicy<2, typename F::execution_space>::index_array_type;
        ippl::parallel_for(
            "layout output field", field.getFieldRangePolicy(),
            KOKKOS_LAMBDA(const Index& index) {
                for (unsigned d = 0; d < 3; ++d)
                    ippl::apply(view, index)[d] = bias + d * 1000
                        + 100 * (index[0] - ghost + firstX) + index[1] - ghost + firstY;
            });
    }

    template <class ParticleStorage, class FieldStorage, class SpatialLayout>
    void checkLayouts(const std::string& directory) {
        const auto comm = ippl::Comm->getCommunicator();
        const int rank = ippl::Comm->rank();
        if (rank == 0)
            std::filesystem::remove_all(directory);
        ippl::Comm->barrier();

        ParticleLayout<ParticleStorage> particleLayout;
        Particles<ParticleStorage> particles(particleLayout);
        particles.create(rank == 0 ? 5 : 0);
        // Capacity differs from the live count, including on initially empty ranks.
        particles.R.reserve(37);
        particles.velocity_m.reserve(37);
        using Mesh = ippl::UniformCartesian<double, 2>;
        using Field = ippl::VectorField<float, 2, FieldStorage, Mesh,
                                        Mesh::DefaultCentering, 3, SpatialLayout>;
        ippl::NDIndex<2> domain(ippl::Index(3, 9), ippl::Index(-2, 2));
        ippl::FieldLayout<2> layout(comm, domain, {true, true});
        Mesh mesh(domain, ippl::Vector<double, 2>(0.2, 0.7),
                  ippl::Vector<double, 2>(1.5, -2.25));
        Field field(mesh, layout, 2);
        const ippl::RecordMetadata VELOCITY{
            {2.0, {1, 0, -1, 0, 0, 0, 0}}, {"x", "y", "z"}};
        const ippl::RecordMetadata DIAGNOSTIC{{1.0, {}}, {"first", "second"}};
        const ippl::FieldMetadata FIELD{
            {{4.0, {}}, {"third", "first", "second"}},
            5.0, {"x", "y"}, {0.5, 0.5}};
        ippl::OpenPMDWriter writer({.directory = directory,
            .backend = std::getenv("IPPL_TEST_BP5") ? ippl::OpenPMDBackend::BP5
                                                  : ippl::OpenPMDBackend::HDF5,
            .stagingBytes = 144}, comm);
        auto& species = writer.addParticles("particles", particles, {.lengthUnitSI = 3.0});
        species.bind(particles.velocity_m, VELOCITY);
        species.bind(particles.diagnostic_m, DIAGNOSTIC);
        writer.addField("field", field, FIELD);
        writer.open();
        std::array<std::vector<std::int64_t>, 2> expectedIds;
        for (unsigned step = 0; step < 2; ++step) {
            SCOPED_TRACE(directory + " write step " + std::to_string(step + 1));
            if (step != 0)
                particles.create(40, true);  // Reallocation after registration and first write.
            fillParticles(particles, step + 0.25);
            Kokkos::deep_copy(field.getView(), ippl::Vector<float, 3>(-9999.0f));
            fillField(field, step + 0.5f);
            // Check the device-to-host adapter separately from the file backend.
            const auto source = ippl::detail::openpmd::fieldSource("field", field, FIELD);
            const auto owned = field.getOwned();
            const ippl::Chunk region{{0, 0}, {static_cast<std::uint64_t>(owned[0].length()),
                                              static_cast<std::uint64_t>(owned[1].length())}};
            for (unsigned d = 0; d < 3; ++d) {
                const auto packed = source.values_m.pack_m(d, region);
                const auto& values = std::get<std::vector<float>>(packed);
                for (std::uint64_t i = 0; i < region.extent[0]; ++i)
                    for (std::uint64_t j = 0; j < region.extent[1]; ++j)
                        EXPECT_FLOAT_EQ(values.at(i * region.extent[1] + j),
                            step + 0.5f + d * 1000 + 100 * (owned[0].first() + i)
                            + owned[1].first() + static_cast<std::int64_t>(j));
            }
            const auto ids = particles.ID.getView();
            for (std::size_t i = 0; i < particles.getLocalNum(); ++i)
                expectedIds[step].push_back(ids(i));
            writer.write({step + 1, 0.1 * (step + 1), 0.1});
            // Staging must not modify either layout, including halo values.
            const auto host = Kokkos::create_mirror_view_and_copy(
                Kokkos::HostSpace(), field.getView());
            EXPECT_FLOAT_EQ(host(0, 0)[0], -9999.0f);
            EXPECT_FLOAT_EQ(host(2, 2)[2], step + 0.5f + 2000
                           + 100 * field.getOwned()[0].first() + field.getOwned()[1].first());
            const auto positions = Kokkos::create_mirror_view_and_copy(
                Kokkos::HostSpace(), particles.R.getView());
            for (std::size_t i = 0; i < particles.getLocalNum(); ++i)
                EXPECT_DOUBLE_EQ(positions(i)[2], step + 0.25 + rank * 100 + 4 * i + 2);
        }
        writer.close();

        ippl::OpenPMDReader reader(directory, comm);
        reader.open();
        const std::array<std::string, 3> axes{"x", "y", "z"};
        for (unsigned step = 0; step < 2; ++step) {
            SCOPED_TRACE(directory + " read step " + std::to_string(step + 1));
            const auto info = reader.inspect(step + 1);
            EXPECT_EQ(info.particles.at(0).count, 5u + step * 40u * ippl::Comm->size());
            EXPECT_EQ(info.fields.at(0).extent, (std::vector<std::uint64_t>{7, 5}));
            EXPECT_EQ(info.fields.at(0).indexBegin, (std::vector<std::int64_t>{3, -2}));
            EXPECT_EQ(info.fields.at(0).metadata.record.components, FIELD.record.components);
            EXPECT_DOUBLE_EQ(info.fields.at(0).metadata.record.units.unitSI, 4.0);
            EXPECT_DOUBLE_EQ(info.particles.at(0).records.at("position").metadata.units.unitSI, 3.0);
            const std::uint64_t count = expectedIds[step].size();
            const std::uint64_t offset = rank == 0 ? 0 : 5 + step * 40u * rank;
            EXPECT_EQ(reader.readParticleComponent<std::int64_t>(
                step + 1, "particles", "ippl_id", "", offset, count), expectedIds[step]);
            for (unsigned d = 0; d < 3; ++d) {
                const auto r = reader.readParticleComponent<double>(
                    step + 1, "particles", "position", axes[d], offset, count);
                const auto v = reader.readParticleComponent<double>(
                    step + 1, "particles", "velocity", axes[d], offset, count);
                for (std::size_t i = 0; i < count; ++i) {
                    EXPECT_DOUBLE_EQ(r.at(i), step + 0.25 + rank * 100 + 4 * i + d);
                    EXPECT_DOUBLE_EQ(v.at(i), -r.at(i));
                }
            }
            const auto diagnostic = reader.readParticleComponent<float>(
                step + 1, "particles", "diagnostic", "second", offset, count);
            for (std::size_t i = 0; i < count; ++i)
                EXPECT_FLOAT_EQ(diagnostic.at(i), -float(i + rank * 100));
            // Read the same global rectangle on each rank, independently of write decomposition.
            for (unsigned d = 0; d < 3; ++d) {
                const auto values = reader.readFieldComponent<float>(
                    step + 1, "field", FIELD.record.components[d], {{0, 0}, {7, 5}});
                ASSERT_EQ(values.size(), 35u);
                for (unsigned i = 0; i < 7; ++i)
                    for (unsigned j = 0; j < 5; ++j)
                        EXPECT_FLOAT_EQ(values[i * 5 + j],
                            step + 0.5f + d * 1000 + 100 * (3 + i) - 2 + j);
            }
        }
        reader.close();
    }

    template <class ParticleStorage, class FieldStorage>
    void checkSpatialLayouts() {
        const auto name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        checkLayouts<ParticleStorage, FieldStorage, Kokkos::LayoutLeft>(
            std::string(name) + "_left");
        checkLayouts<ParticleStorage, FieldStorage, Kokkos::LayoutRight>(
            std::string(name) + "_right");
    }

    // Check exact AoS particle/AoS field output with both spatial index orders.
    TEST(OpenPMDVectorLayouts, AoSParticlesAoSField) { checkSpatialLayouts<ippl::AoS, ippl::AoS>(); }

    // Check exact AoS particle/SoA field output with both spatial index orders.
    TEST(OpenPMDVectorLayouts, AoSParticlesSoAField) { checkSpatialLayouts<ippl::AoS, ippl::SoA>(); }

    // Check exact SoA particle/AoS field output with both spatial index orders.
    TEST(OpenPMDVectorLayouts, SoAParticlesAoSField) { checkSpatialLayouts<ippl::SoA, ippl::AoS>(); }

    // Check exact SoA particle/SoA field output with both spatial index orders.
    TEST(OpenPMDVectorLayouts, SoAParticlesSoAField) { checkSpatialLayouts<ippl::SoA, ippl::SoA>(); }
}  // namespace

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
