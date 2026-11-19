// Unit test: the shared file-I/O API across openPMD/HDF5, openPMD/BP5 and H5hut.
// Checks exact native scalar/vector values, multiple species, changing/empty ranks,
// field ordering/geometry, source preservation, selection and collective error handling.
// Native H5hut reads independently verify the physical files. Tests run on one/two MPI ranks;
// IPPL_TEST_IO_BACKEND chooses the backend. Kernel helpers are outside GoogleTest classes.
#include "Ippl.h"

#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>

#include "Stream/IO/IOReader.h"
#include "Stream/IO/IOWriter.h"
#ifdef IPPL_ENABLE_H5HUT
#include <H5hut.h>
#endif

namespace {
    using Layout = ippl::detail::ParticleLayout<double, 3>;
    struct Particles : ippl::ParticleBase<Layout, Kokkos::HostSpace> {
        ippl::ParticleAttrib<ippl::Vector<float, 3>> P;
        ippl::ParticleAttrib<double> q;
        ippl::ParticleAttrib<std::int32_t> tag32;
        ippl::ParticleAttrib<std::int64_t> tag64;
        Particles(Layout& layout)
            : ippl::ParticleBase<Layout, Kokkos::HostSpace>(layout) {
            P.set_name("velocity");
            q.set_name("charge");
            tag32.set_name("tag32");
            tag64.set_name("tag64");
            addAttribute(P);
            addAttribute(q);
            addAttribute(tag32);
            addAttribute(tag64);
        }
    };
    const ippl::RecordMetadata SCALAR{{2.0, {0, 0, 1, 1, 0, 0, 0}}, {}, {0.25, -0.5}};
    const ippl::RecordMetadata VECTOR{{3.0, {1, 0, -1, 0, 0, 0, 0}}, {"x", "y", "z"}, {0, -0.5}};
    void bind(ippl::ParticleOutputBinding& binding, const Particles& particles) {
        binding.bind(particles.P, VECTOR);
        binding.bind(particles.q, SCALAR);
        binding.bind(particles.tag32, {{1.0, {}}, {}});
        binding.bind(particles.tag64, {{1.0, {}}, {}});
    }
    template <class Attribute, class Function>
    void fillAttribute(Attribute& attribute, Function value) {
        auto view = attribute.getView();
        auto host = Kokkos::create_mirror_view(view);
        for (std::size_t i = 0; i < host.extent(0); ++i)
            host(i) = value(i);
        Kokkos::deep_copy(view, host);
    }
    void fill(Particles& p, int base) {
        fillAttribute(p.R, [=](std::size_t i) {
            return ippl::Vector<double, 3>(base + i, base + i + 0.25, base + i + 0.5);
        });
        fillAttribute(p.P, [=](std::size_t i) {
            return ippl::Vector<float, 3>(base + i + 0.5f, base + i + 0.75f, base + i + 1.0f);
        });
        fillAttribute(p.q, [=](std::size_t i) {
            return double(base + i) + 0.125;
        });
        fillAttribute(p.tag32, [=](std::size_t i) {
            return std::int32_t(base + i);
        });
        fillAttribute(p.tag64, [=](std::size_t i) {
            return (std::int64_t{1} << 45) + base + i;
        });
        fillAttribute(p.ID, [=](std::size_t i) {
            return (std::int64_t{1} << 44) + base + i;
        });
    }
    MPI_Comm comm() {
        return ippl::Comm->getCommunicator();
    }
    std::uint64_t offset(std::uint64_t count) {
        std::uint64_t value = 0;
        MPI_Exscan(&count, &value, 1, MPI_UINT64_T, MPI_SUM, comm());
        return ippl::Comm->rank() ? value : 0;
    }
    class IOTest : public ::testing::Test {
    protected:
        std::filesystem::path path_m;
        void SetUp() override {
            path_m = std::string("io_")
                     + ::testing::UnitTest::GetInstance()->current_test_info()->name();
            if (!ippl::Comm->rank())
                std::filesystem::remove_all(path_m);
            ippl::Comm->barrier();
        }
        ippl::IOOptions options() const {
            ippl::IOOptions result;
            result.directory = path_m;
            const std::string backend =
                std::getenv("IPPL_TEST_IO_BACKEND") ? std::getenv("IPPL_TEST_IO_BACKEND") : "hdf5";
            if (backend == "h5hut" || backend == "h5hut-independent") {
                result.backend = ippl::IOBackend::H5hut;
                if (backend == "h5hut-independent")
                    result.h5hut.transferMode = ippl::H5hutTransferMode::Independent;
            } else if (backend == "bp5")
                result.openPMD.backend = ippl::OpenPMDBackend::BP5;
            else
                result.openPMD.backendConfig = R"({"hdf5":{"independent_stores":false}})";
            result.timeUnitSI   = 7;
            result.stagingBytes = 72;
            return result;
        }
    };

    // Preserve exact values and units through chunked writes, empty species/ranks and source
    // reuse; two species have different counts and independent all-except/minimal selection.
    TEST_F(IOTest, ParticlesChangingCountsAndSelection) {
        Layout layout;
        Particles electrons(layout), ions(layout);
        electrons.create(9, true);
        ions.create(2, true);
        auto settings = options();
        ippl::IOWriter writer(settings, comm());
        bind(writer.addParticles("electrons", electrons, {5.0}), electrons);
        writer.setParticleAttributeSelection(
            "electrons", ippl::ParticleAttributeSelection::allExcept({"tag32"}));
        writer.addParticles("ions", ions, {5.0});
        writer.setParticleAttributeSelection("ions", ippl::ParticleAttributeSelection::minimal());
        writer.open();
        for (unsigned step = 0; step < 3; ++step) {
            electrons.setLocalNum(step == 1 ? 0
                                            : (ippl::Comm->rank() == 0 ? 5
                                               : step == 0             ? 0
                                                                       : 2));
            ions.setLocalNum(ippl::Comm->rank() == 0 ? 2 : 0);
            fill(electrons, 100 * step + 1000 * ippl::Comm->rank());
            fill(ions, 42);
            writer.write({step, 0.5 * step, 0.5});
        }
        fill(electrons, -999);
        writer.flush();
        writer.close();
        writer.close();
        ippl::IOReader reader(path_m, comm());
        reader.open();
        EXPECT_EQ(reader.info().backend, settings.backend);
        EXPECT_EQ(reader.info().iterations, (std::vector<std::uint64_t>{0, 1, 2}));
        EXPECT_DOUBLE_EQ(reader.info().timeUnitSI, 7);
        for (unsigned step : {2, 0, 1}) {
            const auto count = step == 1 ? 0u
                                         : (ippl::Comm->rank() == 0 ? 5u
                                            : step == 0             ? 0u
                                                                    : 2u);
            const auto start = offset(count);
            const auto base  = 100 * step + 1000 * ippl::Comm->rank();
            auto info        = reader.inspect(step);
            ASSERT_EQ(info.particles.size(), 2u);
            EXPECT_FALSE(info.particles[0].records.contains("tag32"));
            EXPECT_EQ(info.particles[1].records.size(), 2u);
            EXPECT_DOUBLE_EQ(info.particles[0].records.at("charge").metadata.timeOffset.constant,
                             0);
            EXPECT_DOUBLE_EQ(info.particles[0].records.at("velocity").metadata.timeOffset.constant,
                             -0.25);
            auto x = reader.readParticleComponent<double>(step, "electrons", "position", "y", start,
                                                          count);
            auto v = reader.readParticleComponent<float>(step, "electrons", "velocity", "z", start,
                                                         count);
            auto q =
                reader.readParticleComponent<double>(step, "electrons", "charge", "", start, count);
            auto ids  = reader.readParticleComponent<std::int64_t>(step, "electrons", "ippl_id", "",
                                                                   start, count);
            auto tags = reader.readParticleComponent<std::int64_t>(step, "electrons", "tag64", "",
                                                                   start, count);
            auto ranks = reader.readParticleComponent<std::int32_t>(step, "electrons", "ippl_rank",
                                                                    "", start, count);
            auto zeros = reader.readParticleComponent<double>(step, "electrons", "positionOffset",
                                                              "x", start, count);
            ASSERT_EQ(x.size(), count);
            for (unsigned i = 0; i < count; ++i) {
                EXPECT_DOUBLE_EQ(x[i], base + i + 0.25);
                EXPECT_FLOAT_EQ(v[i], base + i + 1.0f);
                EXPECT_DOUBLE_EQ(q[i], base + i + 0.125);
                EXPECT_EQ(ids[i], (std::int64_t{1} << 44) + base + i);
                EXPECT_EQ(tags[i], (std::int64_t{1} << 45) + base + i);
                EXPECT_EQ(ranks[i], ippl::Comm->rank());
                EXPECT_EQ(zeros[i], 0);
            }
        }
        EXPECT_THROW(reader.readParticleComponent<float>(0, "electrons", "position", "x", 0, 1),
                     IpplException);
        EXPECT_THROW(
            reader.readParticleComponent<double>(0, "electrons", "position", "x", UINT64_MAX, 1),
            IpplException);
        reader.close();
        reader.close();
#ifdef IPPL_ENABLE_H5HUT
        if (settings.backend == ippl::IOBackend::H5hut) {
            // Read the native H5Part schema without going through IPPL's reader.
            auto props    = H5CreateFileProp();
            MPI_Comm self = MPI_COMM_SELF;
            ASSERT_GE(H5SetPropFileMPIOCollective(props, &self), 0);
            auto file =
                H5OpenFile((path_m / "particles_electrons_0.h5part").c_str(), H5_O_RDONLY, props);
            ASSERT_NE(file, (h5_file_t)H5_ERR);
            ASSERT_GE(H5SetStep(file, 0), 0);
            EXPECT_EQ(H5PartGetNumParticles(file), 5);
            double values[5];
            ASSERT_GE(H5PartReadDataFloat64(file, "y", values), 0);
            for (unsigned i = 0; i < 5; ++i)
                EXPECT_DOUBLE_EQ(values[i], i + 0.25);
            EXPECT_GE(H5CloseFile(file), 0);
            EXPECT_GE(H5CloseProp(props), 0);
        }
#endif
    }

    template <unsigned DIM, class Value>
    void fieldRoundTrip(ippl::IOOptions settings) {
        using Mesh  = ippl::UniformCartesian<double, DIM>;
        using Field = ippl::Field<Value, DIM, Mesh, typename Mesh::DefaultCentering>;
        ippl::NDIndex<DIM> domain;
        ippl::Vector<double, DIM> spacing, origin;
        std::array<bool, DIM> parallel;
        for (unsigned d = 0; d < DIM; ++d) {
            domain[d]   = ippl::Index(-2, int(3 + d));
            spacing[d]  = 0.25 * (d + 1);
            origin[d]   = 3 + d;
            parallel[d] = true;
        }
        ippl::FieldLayout<DIM> layout(comm(), domain, parallel);
        Mesh mesh(domain, spacing, origin);
        Field field(mesh, layout);
        auto view    = field.getView();
        auto host    = Kokkos::create_mirror_view(view);
        using Traits = ippl::detail::io::Components<Value>;
        using Scalar = typename Traits::scalar_type;
        ippl::IOWriter writer(settings, comm());
        ippl::FieldMetadata meta{{{2, {1, 0, -1, 0, 0, 0, 0}}, {}}, 3, {}, {}};
        for (unsigned d = 0; d < DIM; ++d) {
            meta.axisLabels.push_back(std::string(1, 'x' + d));
            meta.samplePosition.push_back(0.5);
        }
        if constexpr (Traits::VECTOR)
            for (unsigned c = 0; c < Traits::COUNT; ++c)
                meta.record.components.push_back("c" + std::to_string(c));
        writer.addField("values", field, meta);
        ippl::detail::io::FieldSource source = ippl::detail::io::fieldSource("values", field, meta);
        const auto owned                     = source.owned_m();
        const auto count                     = ippl::detail::io::volume(owned.extent);
        std::uint64_t size                   = 1;
        for (unsigned d = 0; d < DIM; ++d)
            size *= host.extent(d);
        for (std::uint64_t i = 0; i < size; ++i) {
            ippl::Vector<std::size_t, DIM> index;
            auto rest     = i;
            bool interior = true;
            double value  = 0;
            for (unsigned d = DIM; d-- > 0;) {
                index[d] = rest % host.extent(d);
                rest /= host.extent(d);
                interior &= index[d] >= std::size_t(field.getNghost())
                            && index[d] < field.getNghost() + owned.extent[d];
                value = value * 100 + double(field.getOwned()[d].first()) + double(index[d])
                        - field.getNghost();
            }
            if constexpr (std::is_same_v<Scalar, std::int64_t>)
                value += double(std::int64_t{1} << 45);
            Value result;
            if constexpr (Traits::VECTOR)
                for (unsigned c = 0; c < Traits::COUNT; ++c)
                    result[c] = Scalar(interior ? value + c * 0.25 : -999);
            else
                result = Scalar(interior ? value : -999);
            ippl::apply(host, index) = result;
        }
        Kokkos::deep_copy(view, host);
        std::vector<Scalar> expected(count);
        for (std::uint64_t i = 0; i < count; ++i) {
            auto rest          = i;
            std::int64_t value = 0;
            for (unsigned d = DIM; d-- > 0;) {
                const auto coordinate = rest % owned.extent[d];
                rest /= owned.extent[d];
                value = value * 100 + field.getOwned()[d].first() + std::int64_t(coordinate);
            }
            if constexpr (std::is_same_v<Scalar, std::int64_t>)
                value += std::int64_t{1} << 45;
            expected[i] = Scalar(value);
        }
        writer.open();
        writer.write({7, 1.25, 0.25});
        writer.close();
        field = Value(777);  // Saved files must retain the original values.
        ippl::IOReader reader(settings.directory, comm());
        reader.open();
        auto info = reader.inspect(7).fields.at(0);
        EXPECT_EQ(info.indexBegin, std::vector<std::int64_t>(DIM, -2));
        EXPECT_EQ(info.metadata.axisLabels, meta.axisLabels);
        EXPECT_EQ(info.metadata.samplePosition, meta.samplePosition);
        for (unsigned d = 0; d < DIM; ++d)
            EXPECT_DOUBLE_EQ(info.gridGlobalOffset[d], origin[d] - 2 * spacing[d]);
        auto values =
            reader.readFieldComponent<Scalar>(7, "values", Traits::VECTOR ? "c0" : "", owned);
        EXPECT_EQ(values, expected);
        EXPECT_EQ(values.size(), count);
        auto empty      = owned;
        empty.extent[0] = 0;
        EXPECT_TRUE(
            reader.readFieldComponent<Scalar>(7, "values", Traits::VECTOR ? "c0" : "", empty)
                .empty());
        const auto partial = ippl::Comm->rank() ? empty : owned;
        auto mixed =
            reader.readFieldComponent<Scalar>(7, "values", Traits::VECTOR ? "c0" : "", partial);
        EXPECT_EQ(mixed.size(), ippl::Comm->rank() ? 0 : count);
        if constexpr (Traits::VECTOR) {
            auto component = reader.readFieldComponent<Scalar>(7, "values", "c1", owned);
            for (std::size_t i = 0; i < values.size(); ++i)
                EXPECT_EQ(component[i], values[i] + Scalar(0.25));
        }
        reader.close();
#ifdef IPPL_ENABLE_H5HUT
        if (settings.backend == ippl::IOBackend::H5hut) {
            auto props    = H5CreateFileProp();
            MPI_Comm self = MPI_COMM_SELF;
            ASSERT_GE(H5SetPropFileMPIOCollective(props, &self), 0);
            auto file =
                H5OpenFile((settings.directory / "field_values_7.h5").c_str(), H5_O_RDONLY, props);
            ASSERT_NE(file, (h5_file_t)H5_ERR);
            ASSERT_GE(H5SetStep(file, 7), 0);
            double x, y, z;
            ASSERT_GE(H5Block3dGetFieldOrigin(file, "component_0", &x, &y, &z), 0);
            EXPECT_DOUBLE_EQ(x, origin[0] + (-2 + 0.5) * spacing[0]);
            if constexpr (DIM > 1) {
                EXPECT_DOUBLE_EQ(y, origin[1] + (-2 + 0.5) * spacing[1]);
            }
            std::array<h5_int64_t, 3> end{0, 0, 0};
            std::uint64_t total = 1;
            for (unsigned d = 0; d < DIM; ++d) {
                end[d] = domain[d].length() - 1;
                total *= domain[d].length();
            }
            ASSERT_GE(H5Block3dSetView(file, 0, end[0], 0, end[1], 0, end[2]), 0);
            std::vector<Scalar> native(total);
            if constexpr (std::is_same_v<Scalar, double>) {
                ASSERT_GE(H5Block3dReadScalarFieldFloat64(file, "component_0", native.data()), 0);
            }
            if constexpr (std::is_same_v<Scalar, float>) {
                ASSERT_GE(H5Block3dReadScalarFieldFloat32(file, "component_0", native.data()), 0);
            }
            if constexpr (std::is_same_v<Scalar, std::int32_t>) {
                ASSERT_GE(H5Block3dReadScalarFieldInt32(file, "component_0", native.data()), 0);
            }
            if constexpr (std::is_same_v<Scalar, std::int64_t>) {
                ASSERT_GE(H5Block3dReadScalarFieldInt64(file, "component_0", native.data()), 0);
            }
            for (std::uint64_t i = 0; i < total; ++i) {
                auto rest          = i;
                std::int64_t value = 0, stride = 1;
                for (unsigned d = 0; d < DIM; ++d) {
                    value += stride * (std::int64_t(rest % domain[d].length()) - 2);
                    rest /= domain[d].length();
                    stride *= 100;
                }
                if constexpr (std::is_same_v<Scalar, std::int64_t>)
                    value += std::int64_t{1} << 45;
                EXPECT_EQ(native[i], Scalar(value));
            }
            EXPECT_GE(H5CloseFile(file), 0);
            EXPECT_GE(H5CloseProp(props), 0);
        }
#endif
    }

    // Check a one-dimensional integer field, including singleton padding and empty reads.
    TEST_F(IOTest, Field1DInt32) {
        auto s         = options();
        s.stagingBytes = 1024 * 1024;
        fieldRoundTrip<1, std::int32_t>(s);
    }
    // Check a non-square two-component float field, nonzero indices, sample origins and halos.
    TEST_F(IOTest, Field2DVectorFloat) {
        auto s         = options();
        s.stagingBytes = 1024 * 1024;
        fieldRoundTrip<2, ippl::Vector<float, 2>>(s);
    }
    // Check three-dimensional field order against the original source, including subregion reads.
    TEST_F(IOTest, Field3DDouble) {
        auto s         = options();
        s.stagingBytes = 1024 * 1024;
        fieldRoundTrip<3, double>(s);
    }
    // Check native 64-bit integer field storage without narrowing or floating-point conversion.
    TEST_F(IOTest, Field2DInt64) {
        auto s         = options();
        s.stagingBytes = 1024 * 1024;
        fieldRoundTrip<2, std::int64_t>(s);
    }

    // H5hut rejects unsupported selected types before creating output; excluded attributes
    // do not constrain the backend. openPMD retains its wider unsigned-integer support.
    TEST_F(IOTest, UnsupportedTypesAndExcludedAttributes) {
        Layout layout;
        ippl::ParticleBase<Layout> particles(layout);
        ippl::ParticleAttrib<std::uint64_t> large;
        large.set_name("large");
        particles.addAttribute(large);
        particles.create(2, true);
        fillAttribute(large, [](auto) {
            return UINT64_MAX;
        });
        auto settings = options();
        {
            ippl::IOWriter writer(settings, comm());
            auto& binding = writer.addParticles("p", particles, {1});
            binding.bind(large, {{1, {}}, {}});
            if (settings.backend == ippl::IOBackend::H5hut) {
                EXPECT_THROW(writer.open(), IpplException);
                EXPECT_FALSE(std::filesystem::exists(path_m));
            } else {
                writer.open();
                writer.write({0, 0, 0});
                writer.close();
                ippl::IOReader reader(path_m, comm());
                reader.open();
                auto values = reader.readParticleComponent<std::uint64_t>(
                    0, "p", "large", "", 2 * ippl::Comm->rank(), 2);
                EXPECT_EQ(values, (std::vector<std::uint64_t>{UINT64_MAX, UINT64_MAX}));
                reader.close();
            }
        }
        settings.directory = path_m / "excluded";
        ippl::IOWriter writer(settings, comm());
        writer.addParticles("p", particles, {1});
        writer.setParticleAttributeSelection(
            "p", ippl::ParticleAttributeSelection::allExcept({"large"}));
        writer.open();
        writer.write({0, 0, 0});
        writer.close();
    }

    // Reject rank-dependent metadata and options before any backend creates output; this must
    // fail collectively on two ranks rather than hanging in a file-open collective.
    TEST_F(IOTest, CollectivePreflight) {
        Layout layout;
        ippl::ParticleBase<Layout> particles(layout);
        ippl::IOWriter writer(options(), comm());
        writer.addParticles("p", particles, {ippl::Comm->rank() == 0 ? -1.0 : 1.0});
        EXPECT_THROW(writer.open(), IpplException);
        EXPECT_FALSE(std::filesystem::exists(path_m));
        if (ippl::Comm->size() > 1) {
            auto settings = options();
            if (ippl::Comm->rank())
                settings.timeUnitSI = 8;
            ippl::IOWriter other(settings, comm());
            other.addParticles("p", particles, {1});
            EXPECT_THROW(other.open(), IpplException);
            EXPECT_FALSE(std::filesystem::exists(path_m));
        }
    }

    // H5Block's full-component staging requirement is enforced before directory creation.
    TEST_F(IOTest, FieldBudgetPreflight) {
        auto settings = options();
        if (settings.backend != ippl::IOBackend::H5hut)
            GTEST_SKIP() << "H5hut-specific memory contract";
        using Mesh  = ippl::UniformCartesian<double, 2>;
        using Field = ippl::Field<double, 2, Mesh, Mesh::DefaultCentering>;
        ippl::NDIndex<2> domain{ippl::Index(8), ippl::Index(9)};
        ippl::FieldLayout<2> layout(comm(), domain, {true, true});
        Mesh mesh(domain, {1, 1}, {0, 0});
        Field field(mesh, layout);
        ippl::IOWriter writer(settings, comm());
        writer.addField("f", field, {{{1, {}}, {}}, 1, {"x", "y"}, {0.5, 0.5}});
        EXPECT_THROW(writer.open(), IpplException);
        EXPECT_FALSE(std::filesystem::exists(path_m));
    }

    // The generic reader detects H5hut independently of the filename suffix, ignores unfinished
    // steps, and rejects a completed step whose native payload has disappeared.
    TEST_F(IOTest, CompletionAndMissingPayload) {
        auto settings = options();
        if (settings.backend != ippl::IOBackend::H5hut)
            GTEST_SKIP() << "H5hut completion descriptor";
        Layout layout;
        ippl::ParticleBase<Layout> p(layout);
        p.create(1, true);
        ippl::IOWriter writer(settings, comm());
        writer.addParticles("p", p, {1});
        writer.open();
        writer.write({0, 0, 0});
        writer.close();
        if (!ippl::Comm->rank())
            std::ofstream(path_m / "step_1.complete.tmp") << "incomplete";
        ippl::Comm->barrier();
        {
            ippl::IOReader reader(path_m, comm());
            reader.open();
            EXPECT_EQ(reader.info().iterations, (std::vector<std::uint64_t>{0}));
            reader.close();
        }
        if (!ippl::Comm->rank())
            std::filesystem::remove(path_m / "particles_p_0.h5part");
        ippl::Comm->barrier();
        ippl::IOReader reader(path_m, comm());
        EXPECT_THROW(reader.open(), IpplException);
    }

    // Output on independent MPI subcommunicators must not accidentally use MPI_COMM_WORLD.
    TEST_F(IOTest, Subcommunicators) {
        MPI_Comm local;
        MPI_Comm_split(comm(), ippl::Comm->rank(), 0, &local);
        {
            Layout layout;
            ippl::ParticleBase<Layout> p(layout);
            p.create(1, true);
            auto settings      = options();
            settings.directory = path_m / std::to_string(ippl::Comm->rank());
            ippl::IOWriter writer(settings, local);
            writer.addParticles("p", p, {1});
            writer.open();
            writer.write({0, 0, 0});
            writer.close();
            ippl::IOReader reader(settings.directory, local);
            reader.open();
            EXPECT_EQ(reader.inspect(0).particles.at(0).count, 1u);
            reader.close();
        }
        MPI_Comm_free(&local);
    }
}  // namespace
int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
