//
// Unit test: openPMD particle and field output/readback
//
// Checks exact component values, units, geometry, attribute selection, IDs and reader/writer
// validation. Small staging budgets, changing particle counts and empty ranks exercise
// chunk writes and MPI collectives. IPPL_TEST_BP5 selects BP5; otherwise the suite uses HDF5.
//

#include <cstdlib>
#include "Ippl.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <openPMD/openPMD.hpp>

#include "Stream/OpenPMD/OpenPMDReader.h"
#include "Stream/OpenPMD/OpenPMDWriter.h"

namespace {
    using Layout = ippl::detail::ParticleLayout<double, 3>;
    struct Particles : ippl::ParticleBase<Layout, Kokkos::HostSpace> {
        ippl::ParticleAttrib<ippl::Vector<double, 3>> P;
        ippl::ParticleAttrib<double> q;
        ippl::ParticleAttrib<std::int32_t> debug;
        explicit Particles(Layout& layout)
            : ippl::ParticleBase<Layout, Kokkos::HostSpace>(layout) {
            P.set_name("velocity");
            q.set_name("charge");
            debug.set_name("debug");
            addAttribute(P);
            addAttribute(q);
            addAttribute(debug);
        }
    };
    using Mesh        = ippl::UniformCartesian<double, 2>;
    using Field       = ippl::Field<double, 2, Mesh, Mesh::DefaultCentering>;
    using VectorField = ippl::Field<ippl::Vector<float, 2>, 2, Mesh, Mesh::DefaultCentering>;
    const ippl::RecordMetadata VELOCITY{
        {2.0, {1, 0, -1, 0, 0, 0, 0}}, {"x", "y", "z"}, {0.0, -0.5}};
    const ippl::RecordMetadata CHARGE{{3.0, {0, 0, 1, 1, 0, 0, 0}}, {}};
    const ippl::RecordMetadata DEBUG{{1.0, {}}, {}};
    const ippl::FieldMetadata DENSITY{
        {{4.0, {-3, 0, 1, 1, 0, 0, 0}}, {}}, 5.0, {"x", "y"}, {0.5, 0.5}};

    template <class ParticleContainer>
    void fillPositions(ParticleContainer& particles, double value) {
        const auto r = particles.R.getView();
        Kokkos::parallel_for(
            "fill positions", ippl::getRangePolicy(r),
            KOKKOS_LAMBDA(std::size_t i) { r(i) = ippl::Vector<double, 3>(value); });
    }

    void fillParticles(Particles& particles, double bias) {
        const auto r     = particles.R.getView();
        const auto p     = particles.P.getView();
        const auto q     = particles.q.getView();
        const auto debug = particles.debug.getView();
        const int rank   = ippl::Comm->rank();
        Kokkos::parallel_for(
            "fill output particles", ippl::getRangePolicy(q), KOKKOS_LAMBDA(std::size_t i) {
                q(i)     = bias + rank * 1000 + i;
                debug(i) = -1;
                for (unsigned d = 0; d < 3; ++d) {
                    r(i)[d] = bias + rank * 1000 + i * 10 + d;
                    p(i)[d] = -r(i)[d];
                }
            });
    }
    template <class F>
    void fillField(F& field, double bias) {
        const auto view  = field.getView();
        const auto owned = field.getOwned();
        const int ghost  = field.getNghost();
        const int firstX = owned[0].first(), firstY = owned[1].first();
        using Index = typename ippl::RangePolicy<2, typename F::execution_space>::index_array_type;
        ippl::parallel_for(
            "fill output field", field.getFieldRangePolicy(), KOKKOS_LAMBDA(const Index& index) {
                const double value =
                    bias + 100 * (index[0] - ghost + firstX) + index[1] - ghost + firstY;
                auto&& cell = ippl::apply(view, index);
                if constexpr (std::is_same_v<typename F::value_type, double>)
                    cell = value;
                else {
                    cell[0] = value;
                    cell[1] = -value;
                }
            });
    }
    template <class T>
    void checkScalarType(ippl::OpenPMDOptions settings, const std::string& name, T value) {
        settings.directory /= name;
        Layout layout;
        ippl::ParticleBase<Layout> particles(layout);
        ippl::ParticleAttrib<T> attribute;
        attribute.set_name("value");
        particles.addAttribute(attribute);
        particles.create(ippl::Comm->rank() == 0 ? 5 : 0);
        fillPositions(particles, 0.0);
        Kokkos::deep_copy(attribute.getView(), value);
        const auto comm = ippl::Comm->getCommunicator();
        ippl::OpenPMDWriter writer(settings, comm);
        writer.addParticles("sample", particles, {.lengthUnitSI = 1.0}).bind(attribute, DEBUG);
        writer.open();
        writer.write({1, 1.0, 1.0});
        Kokkos::deep_copy(attribute.getView(), T{});
        writer.close();
        ippl::OpenPMDReader reader(settings.directory, comm);
        reader.open();
        const auto values = reader.readParticleComponent<T>(
            1, "sample", "value", "", 0, particles.getLocalNum());
        EXPECT_EQ(values.size(), particles.getLocalNum());
        for (const auto stored : values)
            EXPECT_EQ(stored, value) << name;
        reader.close();
    }

    class OpenPMDTest : public ::testing::Test {
    public:
        std::filesystem::path path_m;
        void SetUp() override {
            path_m = std::string("output_")
                     + ::testing::UnitTest::GetInstance()->current_test_info()->name();
            if (ippl::Comm->rank() == 0)
                std::filesystem::remove_all(path_m);
            ippl::Comm->barrier();
        }
        void TearDown() override { ippl::Comm->barrier(); }
        ippl::OpenPMDOptions options() const {
            // Three doubles fit: forces repeated bounded chunks in the small arrays.
            return {.directory = path_m,
                    .backend = std::getenv("IPPL_TEST_BP5") ? ippl::OpenPMDBackend::BP5
                                                          : ippl::OpenPMDBackend::HDF5,
                    .timeUnitSI = 2.0, .stagingBytes = 72};
        }
        MPI_Comm communicator() const { return ippl::Comm->getCommunicator(); }
    };

    // Check exact selected-particle readback and metadata over two writes, including empty
    // ranks/species and source reallocation; reject duplicate steps and late bindings.
    TEST_F(OpenPMDTest, ParticleRoundTripAndReallocation) {
        Layout layout;
        Particles particles(layout), empty(layout);
        const auto rank = ippl::Comm->rank();
        particles.create(rank == 0 ? 5 : 0);
        fillParticles(particles, 0.25);
        ippl::OpenPMDWriter output(options(), communicator());
        auto& species = output.addParticles("electrons", particles, {.lengthUnitSI = 7.0});
        species.bind(particles.P, VELOCITY);
        species.bind(particles.q, CHARGE);
        output.setParticleAttributeSelection(
            "electrons", ippl::ParticleAttributeSelection::minimal({"velocity", "charge"}));
        output.addParticles("empty", empty, {.lengthUnitSI = 1.0});
        output.setParticleAttributeSelection("empty", ippl::ParticleAttributeSelection::minimal());
        output.open();
        output.write({1, 0.1, 0.1});
        particles.create(2, true);
        fillParticles(particles, 1.25);
        output.write({2, 0.2, 0.1});
        EXPECT_THROW(output.write({2, 0.2, 0.1}), IpplException);
        EXPECT_THROW(species.bind(particles.debug, DEBUG), IpplException);
        output.close();
        output.close();

        ippl::OpenPMDReader input(path_m, communicator());
        input.open();
        EXPECT_EQ(input.info().backend, options().backend);
        EXPECT_EQ(input.info().iterations, (std::vector<std::uint64_t>{1, 2}));
        EXPECT_DOUBLE_EQ(input.info().timeUnitSI, 2.0);
        const auto first = input.inspect(1);
        const auto& info = first.particles.at(0);  // names are sorted: electrons, empty
        EXPECT_EQ(info.name, "electrons");
        EXPECT_EQ(info.count, 5u);
        EXPECT_FALSE(info.records.contains("debug"));
        EXPECT_FALSE(info.records.contains("ippl_id"));
        EXPECT_FALSE(info.records.contains("ippl_rank"));
        EXPECT_DOUBLE_EQ(info.records.at("velocity").metadata.timeOffset.constant, -0.05);
        const auto count = rank == 0 ? 5u : 0u;
        const auto x =
            input.readParticleComponent<double>(1, "electrons", "position", "x", 0, count);
        const auto p =
            input.readParticleComponent<double>(1, "electrons", "velocity", "z", 0, count);
        const auto offset =
            input.readParticleComponent<double>(1, "electrons", "positionOffset", "x", 0, count);
        for (std::size_t i = 0; i < x.size(); ++i) {
            EXPECT_DOUBLE_EQ(x[i], 0.25 + i * 10);
            EXPECT_DOUBLE_EQ(p[i], -(2.25 + i * 10));
            EXPECT_DOUBLE_EQ(offset[i], 0.0);
        }
        const auto local          = particles.getLocalNum();
        const std::uint64_t start = rank == 0 ? 0 : 7 + (rank - 1) * 2;
        const auto q =
            input.readParticleComponent<double>(2, "electrons", "charge", "", start, local);
        const auto source =
            Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), particles.q.getView());
        for (std::size_t i = 0; i < q.size(); ++i) {
            EXPECT_DOUBLE_EQ(q[i], 1.25 + rank * 1000 + i);
            EXPECT_DOUBLE_EQ(source(i), q[i]);
        }
        EXPECT_TRUE(input.readParticleComponent<double>(1, "empty", "position", "x", 0, 0).empty());
        EXPECT_THROW(input.readParticleComponent<float>(1, "electrons", "charge", "", 0, 0),
                     IpplException);
        EXPECT_THROW(
            input.readParticleComponent<double>(1, "electrons", "charge", "", 0, rank == 0 ? 6 : 0),
            IpplException);
        input.close();
        EXPECT_EQ(q.size(), local);  // returned storage outlives the reader
    }

    // Check owned scalar/vector values, shifted mesh geometry and component order, excluding
    // halos; independently inspect the stored metadata with the openPMD API.
    TEST_F(OpenPMDTest, FieldsExcludeHalosAndPreserveGeometry) {
        ippl::NDIndex<2> domain(ippl::Index(3, 9), ippl::Index(-2, 2));
        std::array<bool, 2> decomposition{true, true};
        ippl::FieldLayout<2> layout(communicator(), domain, decomposition);
        Mesh mesh(domain, ippl::Vector<double, 2>(0.2, 0.7), ippl::Vector<double, 2>(1.5, -2.25));
        Field rho(mesh, layout, 2);
        VectorField electric(mesh, layout, 2);
        Kokkos::deep_copy(rho.getView(), -999.0);
        Kokkos::deep_copy(electric.getView(), ippl::Vector<float, 2>(-999.0f));
        fillField(rho, 0.5);
        fillField(electric, 0.5);
        ippl::OpenPMDWriter output(options(), communicator());
        output.addField("rho", rho, DENSITY);
        auto vectorMetadata              = DENSITY;
        vectorMetadata.record.components = {"y", "x"};  // preserve declared order
        output.addField("E", electric, vectorMetadata);
        output.open();
        output.write({4, 1.0, 0.25});
        output.close();
        ippl::OpenPMDReader input(path_m, communicator());
        input.open();
        const auto info      = input.inspect(4);
        const auto& geometry = info.fields.at(1);
        EXPECT_EQ(geometry.name, "rho");
        EXPECT_EQ(geometry.extent, (std::vector<std::uint64_t>{7, 5}));
        EXPECT_EQ(geometry.indexBegin, (std::vector<std::int64_t>{3, -2}));
        EXPECT_DOUBLE_EQ(geometry.gridGlobalOffset[0], 2.1);
        EXPECT_DOUBLE_EQ(geometry.gridGlobalOffset[1], -3.65);
        EXPECT_DOUBLE_EQ(geometry.metadata.gridUnitSI, 5.0);
        EXPECT_EQ(info.fields.at(0).metadata.record.components,
                  (std::vector<std::string>{"y", "x"}));
        ippl::Chunk region{{static_cast<std::uint64_t>(rho.getOwned()[0].first() - 3),
                            static_cast<std::uint64_t>(rho.getOwned()[1].first() + 2)},
                           {static_cast<std::uint64_t>(rho.getOwned()[0].length()),
                            static_cast<std::uint64_t>(rho.getOwned()[1].length())}};
        const auto values = input.readFieldComponent<double>(4, "rho", "", region);
        const auto y      = input.readFieldComponent<float>(4, "E", "y", region);
        const auto x      = input.readFieldComponent<float>(4, "E", "x", region);
        for (std::size_t i = 0; i < region.extent[0]; ++i)
            for (std::size_t j = 0; j < region.extent[1]; ++j) {
                const auto k = i * region.extent[1] + j;
                const double expected =
                    0.5 + 100 * (3 + region.offset[0] + i) - 2 + region.offset[1] + j;
                EXPECT_DOUBLE_EQ(values[k], expected);
                EXPECT_FLOAT_EQ(y[k], expected);
                EXPECT_FLOAT_EQ(x[k], -expected);
            }
        input.close();
        // Inspect with the backend directly, independently of IPPL's reader.
        ::openPMD::Series raw((path_m / (options().backend == ippl::OpenPMDBackend::BP5
            ? "data_%T.bp5" : "data_%T.h5")).string(), ::openPMD::Access::READ_ONLY,
                              communicator());
        auto component = raw.iterations[4].meshes["rho"][::openPMD::RecordComponent::SCALAR];
        EXPECT_EQ(component.getExtent(), (::openPMD::Extent{7, 5}));
        EXPECT_DOUBLE_EQ(component.unitSI(), 4.0);
        EXPECT_EQ(component.position<double>(), (std::vector<double>{0.5, 0.5}));
        raw.close();
    }

    // Check default all() and allExcept() selections for separate species, preserving particle
    // IDs and including rank IDs only where requested.
    TEST_F(OpenPMDTest, AllAndAllExceptWithIds) {
        Layout layout;
        Particles particles(layout);
        particles.create(3);
        fillParticles(particles, 0.25);
        ippl::OpenPMDWriter output(options(), communicator());
        auto bind = [&](const std::string& name) {
            auto& species = output.addParticles(name, particles, {.lengthUnitSI = 1.0});
            species.bind(particles.P, VELOCITY);
            species.bind(particles.q, CHARGE);
            return &species;
        };
        bind("all")->bind(particles.debug, DEBUG);  // default selection requires no setter
        bind("subset");
        output.setParticleAttributeSelection(
            "subset", ippl::ParticleAttributeSelection::allExcept({"debug"}).withRankIds(false));
        output.open();
        output.write({0, 0.0, 0.0});
        output.close();
        ippl::OpenPMDReader input(path_m, communicator());
        input.open();
        const auto info = input.inspect(0);
        EXPECT_TRUE(info.particles[0].records.contains("debug"));
        EXPECT_TRUE(info.particles[0].records.contains("ippl_rank"));
        EXPECT_FALSE(info.particles[1].records.contains("debug"));
        EXPECT_FALSE(info.particles[1].records.contains("ippl_rank"));
        const auto offset = std::uint64_t(ippl::Comm->rank()) * 3;
        const auto ids    = input.readParticleComponent<typename Particles::index_type>(
            0, "subset", "ippl_id", "", offset, 3);
        const auto original =
            Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), particles.ID.getView());
        const auto ranks =
            input.readParticleComponent<std::int32_t>(0, "all", "ippl_rank", "", offset, 3);
        for (unsigned i = 0; i < 3; ++i) {
            EXPECT_EQ(ids[i], original(i));
            EXPECT_EQ(ranks[i], ippl::Comm->rank());
        }
        input.close();
    }

    // Check independent output on split communicators: counts and rank IDs use the subgroup,
    // and particle IDs are absent when disabled in the container.
    TEST_F(OpenPMDTest, SubcommunicatorsAndIdsDisabled) {
        const int color = ippl::Comm->rank() % 2;
        MPI_Comm group;
        MPI_Comm_split(communicator(), color, ippl::Comm->rank(), &group);
        int rank, size;
        MPI_Comm_rank(group, &rank);
        MPI_Comm_size(group, &size);
        Layout layout;
        ippl::ParticleBase<Layout> particles(layout);
        static_assert(!decltype(particles)::EnableIDs);
        particles.create(1);
        fillPositions(particles, color + 0.25);
        auto settings = options();
        settings.directory /= std::to_string(color);
        {
            ippl::OpenPMDWriter output(settings, group);
            output.addParticles("electrons", particles, {.lengthUnitSI = 1.0});
            output.open();
            output.write({std::uint64_t(color), double(color), 0.1});
            output.close();
            ippl::OpenPMDReader input(settings.directory, group);
            input.open();
            const auto info = input.inspect(color).particles.at(0);
            EXPECT_EQ(info.count, std::uint64_t(size));
            EXPECT_FALSE(info.records.contains("ippl_id"));
            const auto x =
                input.readParticleComponent<double>(color, "electrons", "position", "x", rank, 1);
            const auto ranks = input.readParticleComponent<std::int32_t>(color, "electrons",
                                                                         "ippl_rank", "", rank, 1);
            EXPECT_EQ(x, std::vector<double>{color + 0.25});
            EXPECT_EQ(ranks, std::vector<std::int32_t>{rank});
            input.close();
        }
        MPI_Comm_free(&group);
    }

    // Reject selected attributes without metadata bindings and unknown selection names before
    // creating an output directory.
    TEST_F(OpenPMDTest, RejectMissingBindingsAndUnknownNames) {
        Layout layout;
        Particles particles(layout);
        ippl::OpenPMDWriter output(options(), communicator());
        output.addParticles("electrons", particles, {.lengthUnitSI = 1.0});
        EXPECT_THROW(output.open(), IpplException);
        EXPECT_FALSE(std::filesystem::exists(path_m));
        output.setParticleAttributeSelection(
            "electrons", ippl::ParticleAttributeSelection::minimal({"notRegistered"}));
        EXPECT_THROW(output.open(), IpplException);
        EXPECT_FALSE(std::filesystem::exists(path_m));
    }

    // Reject an invalid backend enum before creating an output directory.
    TEST_F(OpenPMDTest, RejectUnknownBackend) {
        Layout layout;
        ippl::ParticleBase<Layout> particles(layout);
        auto settings    = options();
        settings.backend = static_cast<ippl::OpenPMDBackend>(99);
        ippl::OpenPMDWriter output(settings, communicator());
        output.addParticles("electrons", particles, {.lengthUnitSI = 1.0});
        EXPECT_THROW(output.open(), IpplException);
        EXPECT_FALSE(std::filesystem::exists(path_m));
    }

    // Round-trip all supported signed/unsigned integer widths and floating-point types exactly,
    // including empty ranks and reuse of the source storage after writing.
    TEST_F(OpenPMDTest, AllScalarTypes) {
        checkScalarType<std::int8_t>(options(), "int8", -7);
        checkScalarType<std::uint8_t>(options(), "uint8", 250);
        checkScalarType<std::int16_t>(options(), "int16", -2345);
        checkScalarType<std::uint16_t>(options(), "uint16", 65530);
        checkScalarType<std::int32_t>(options(), "int32", -12345678);
        checkScalarType<std::uint32_t>(options(), "uint32", 4000000000u);
        checkScalarType<std::int64_t>(options(), "int64", -1234567890123LL);
        checkScalarType<std::uint64_t>(options(), "uint64", 12345678901234567890ULL);
        checkScalarType<float>(options(), "float32", -1.25f);
        checkScalarType<double>(options(), "float64", 1.23456789);
    }

    // Reject unsupported series versions, paths outside the series directory and trailing
    // descriptor tokens when opening a reader.
    TEST_F(OpenPMDTest, RejectInvalidDescriptors) {
        if (!ippl::Comm->rank())
            std::filesystem::create_directories(path_m);
        ippl::Comm->barrier();
        for (const auto& descriptor : {"IPPL_OPENPMD 3 BP5 data_%T.bp5",
                                      "IPPL_OPENPMD 2 BP5 ../data_%T.bp5",
                                      "IPPL_OPENPMD 1 data_%T.h5 extra"}) {
            if (!ippl::Comm->rank())
                std::ofstream(path_m / "ippl-series.txt") << descriptor;
            ippl::Comm->barrier();
            ippl::OpenPMDReader reader(path_m, communicator());
            EXPECT_THROW(reader.open(), IpplException);
        }
    }

    // Reject configuration that conflicts with the selected backend or synchronous BP5 output
    // before creating an output directory.
    TEST_F(OpenPMDTest, RejectConflictingConfigurationBeforeCreation) {
        const std::vector<std::string> configs = options().backend == ippl::OpenPMDBackend::BP5
            ? std::vector<std::string>{R"({"backend":"hdf5"})",
                  R"({"adios2":{"engine":{"type":"sst"}}})",
                  R"({"adios2":{"engine":{"parameters":{"AsyncWrite":"On"}}}})",
                  R"({"adios2":{"engine":{"preferred_flush_target":"buffer"}}})",
                  R"({"adios2":{"engine":{"access_mode":"append"}}})"}
            : std::vector<std::string>{R"({"backend":"adios2"})"};
        for (const auto& config : configs) {
            Layout layout;
            ippl::ParticleBase<Layout> particles(layout);
            auto settings = options();
            settings.backendConfig = config;
            ippl::OpenPMDWriter writer(settings, communicator());
            writer.addParticles("electrons", particles, {.lengthUnitSI = 1.0});
            EXPECT_THROW(writer.open(), IpplException) << config;
            EXPECT_FALSE(std::filesystem::exists(path_m));
        }
    }

    // Check writes after live counts shrink to zero and grow again, then read iterations out
    // of order and repeatedly after the source values have been overwritten.
    TEST_F(OpenPMDTest, ChangingCountsAndRandomReads) {
        Layout layout;
        ippl::ParticleBase<Layout> particles(layout);
        particles.create(7, true);
        auto settings = options();
        // Exercise TOML and non-default parameter capitalization without a parser in IPPL.
        if (settings.backend == ippl::OpenPMDBackend::BP5)
            settings.backendConfig = "[adios2.engine.parameters]\nStatsLevel = '0'\n";
        ippl::OpenPMDWriter writer(settings, communicator());
        writer.addParticles("electrons", particles, {.lengthUnitSI = 1.0});
        writer.open();
        for (unsigned step = 1; step <= 3; ++step) {
            // Export reads live counts, including ranks that shrink to zero.
            particles.setLocalNum(step == 1 ? 7 : (step == 2 ? 0 : 1));
            fillPositions(particles, 10.0 * step);
            writer.write({step, double(step), 1.0});
        }
        fillPositions(particles, -999.0);
        writer.close();
        ippl::OpenPMDReader reader(path_m, communicator());
        reader.open();
        for (const auto step : {3u, 1u, 2u, 1u}) {
            const std::uint64_t count = step == 1 ? 7 : (step == 2 ? 0 : 1);
            EXPECT_EQ(reader.inspect(step).particles[0].count, count * ippl::Comm->size());
            const auto values = reader.readParticleComponent<double>(
                step, "electrons", "position", "y", count * ippl::Comm->rank(), count);
            EXPECT_EQ(values.size(), count);
            for (const auto value : values)
                EXPECT_DOUBLE_EQ(value, 10.0 * step);
        }
        reader.close();
    }

    // Check that invalid units supplied on rank zero cause every rank to reject open(),
    // without creating an output directory.
    TEST_F(OpenPMDTest, AgreeLocalMetadataErrors) {
        Layout layout;
        Particles particles(layout);
        ippl::OpenPMDWriter output(options(), communicator());
        output.addParticles("electrons", particles,
                            {.lengthUnitSI = ippl::Comm->rank() == 0 ? -1.0 : 1.0});
        output.setParticleAttributeSelection("electrons",
                                             ippl::ParticleAttributeSelection::minimal());
        EXPECT_THROW(output.open(), IpplException);
        EXPECT_FALSE(std::filesystem::exists(path_m));
    }

    // With at least two MPI ranks, reject differing attribute selections collectively before
    // creating output.
    TEST_F(OpenPMDTest, RejectSchemaDisagreement) {
        if (ippl::Comm->size() < 2)
            GTEST_SKIP() << "Requires two ranks";
        Layout layout;
        Particles particles(layout);
        ippl::OpenPMDWriter output(options(), communicator());
        auto& species = output.addParticles("electrons", particles, {.lengthUnitSI = 1.0});
        species.bind(particles.q, CHARGE);
        species.bind(particles.P, VELOCITY);
        output.setParticleAttributeSelection(
            "electrons", ippl::ParticleAttributeSelection::minimal(
                             {ippl::Comm->rank() == 0 ? "charge" : "velocity"}));
        EXPECT_THROW(output.open(), IpplException);
        EXPECT_FALSE(std::filesystem::exists(path_m));
    }

    // Refuse an existing output directory and retain the file already stored in it.
    TEST_F(OpenPMDTest, RefuseExistingDirectory) {
        Layout layout;
        Particles particles(layout);
        if (!ippl::Comm->rank()) {
            std::filesystem::create_directory(path_m);
            std::ofstream(path_m / "keep.txt") << "existing data";
        }
        ippl::Comm->barrier();
        ippl::OpenPMDWriter output(options(), communicator());
        output.addParticles("electrons", particles, {.lengthUnitSI = 1.0});
        output.setParticleAttributeSelection("electrons",
                                             ippl::ParticleAttributeSelection::minimal());
        EXPECT_THROW(output.open(), IpplException);
        EXPECT_TRUE(std::filesystem::exists(path_m / "keep.txt"));
    }
}  // namespace

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
