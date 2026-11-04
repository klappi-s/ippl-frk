#include <cstdlib>
#include <fstream>
#include <openPMD/auxiliary/JSON.hpp>
#include <openPMD/openPMD.hpp>

#include "Stream/IO/detail/Backend.h"
namespace ippl::detail::io {
    namespace {
        const char* seriesPattern(OpenPMDBackend backend) {
            switch (backend) {
                case OpenPMDBackend::HDF5:
                    require(IPPL_OPENPMD_HAVE_HDF5, "IPPL was built without openPMD HDF5 support");
                    return "data_%T.h5";
                case OpenPMDBackend::BP5:
                    require(IPPL_OPENPMD_HAVE_ADIOS2,
                            "IPPL was built without openPMD ADIOS2 support");
                    return "data_%T.bp5";
            }
            throw IpplException("openPMD", "Unknown output backend");
        }
        std::string backendConfiguration(OpenPMDBackend backend, const std::string& input,
                                         bool reading = false) {
            seriesPattern(backend);  // Check build capabilities before creating any files.
            const std::string required =
                backend == OpenPMDBackend::HDF5
                    ? R"({"backend":"hdf5","iteration_encoding":"file_based"})"
                    : std::string(
                          R"({"backend":"adios2","iteration_encoding":"file_based","adios2":{"engine":{"type":"bp5","preferred_flush_target":"disk","access_mode":")")
                          + (reading ? "readrandomaccess" : "write")
                          + R"(","parameters":{"asyncwrite":"off"}}}})";
            // Resolve files/TOML locally, then agree the resulting JSON on all ranks.
            // Lowercase only the validation copy: ADIOS2 parameter names are case insensitive.
            const auto canonical = ::openPMD::json::merge(input, "{}");
            auto folded          = canonical;
            for (auto& c : folded)
                if (c >= 'A' && c <= 'Z')
                    c += 'a' - 'A';
            const auto requested = ::openPMD::json::merge(required, folded);
            require(::openPMD::json::merge(requested, required) == requested,
                    "backendConfig conflicts with the selected backend, file-based encoding "
                    "or synchronous disk output");
            if (backend == OpenPMDBackend::BP5) {
                require(::openPMD::json::merge(
                            requested,
                            R"({"adios2":{"engine":{"treat_unsupported_engine_like":null}}})")
                            == requested,
                        "Engine emulation is unsupported for BP5 output");
                const auto pretend = std::getenv("OPENPMD_ADIOS2_PRETEND_ENGINE");
                require(!pretend || !*pretend,
                        "OPENPMD_ADIOS2_PRETEND_ENGINE is unsupported for BP5 output");
            }
            // Explicit engine/AsyncWrite settings take precedence over environment defaults.
            return ::openPMD::json::merge(required, canonical);
        }
        void checkBackend(const ::openPMD::Series& series, OpenPMDBackend backend) {
            require(series.backend() == (backend == OpenPMDBackend::HDF5 ? "MPI_HDF5" : "ADIOS2"),
                    "openPMD did not select the requested parallel backend");
            require(series.iterationEncoding() == ::openPMD::IterationEncoding::fileBased,
                    "openPMD must use file-based iteration encoding");
        }
        void flushOutput(::openPMD::Series& series) {
            series.flush(series.backend() == "ADIOS2"
                             ? R"({"adios2":{"engine":{"preferred_flush_target":"disk"}}})"
                             : "{}");
        }
        ::openPMD::Datatype backendType(DataType type) {
            return std::visit(
                [](const auto& values) {
                    return ::openPMD::determineDatatype<
                        typename std::decay_t<decltype(values)>::value_type>();
                },
                buffer(type));
        }
        DataType scalarType(::openPMD::Datatype type) {
            for (unsigned i = 0; i <= static_cast<unsigned>(DataType::FLOAT64); ++i) {
                auto candidate = static_cast<DataType>(i);
                if (::openPMD::isSame(type, backendType(candidate)))
                    return candidate;
            }
            throw IpplException("openPMD", "Unsupported stored scalar type");
        }
        template <class Component>
        void writeChunks(::openPMD::Series& series, Component component, const RecordSource& source,
                         unsigned index, const Chunk& owned, std::size_t budget,
                         const Session& session) {
            // Device pack, host mirror and returned vector can coexist.
            const auto capacity = budget / (3 * scalarBytes(source.info_m.dataType));
            Tiles tiles(owned.extent, capacity);
            std::uint64_t rounds = 0;
            MPI_Allreduce(&tiles.count_m, &rounds, 1, MPI_UINT64_T, MPI_MAX, session.comm_m);
            for (std::uint64_t i = 0; i < rounds; ++i) {
                Buffer data;
                Chunk local;
                collective(session, [&] {
                    if (i < tiles.count_m) {
                        local = tiles.at(i);
                        data  = source.pack_m(index, local, ArrayOrder::C);
                    } else {
                        local = {std::vector<std::uint64_t>(owned.extent.size(), 0),
                                 std::vector<std::uint64_t>(owned.extent.size(), 0)};
                        data  = buffer(source.info_m.dataType, 1);
                    }
                });
                auto offset = local.offset;
                if (i < tiles.count_m) {
                    for (std::size_t d = 0; d < offset.size(); ++d)
                        offset[d] += owned.offset[d];
                }
                // Empty selections still open the dataset on every rank. HDF5 collective
                // metadata requires matching operations, including ranks with no data.
                std::visit(
                    [&](auto& values) {
                        component.storeChunk(values, offset, local.extent);
                    },
                    data);
                flushOutput(series);
            }
        }
        template <class Record>
        void recordMetadata(Record record, const RecordInfo& info, const StepInfo& step) {
            record.setUnitDimension(info.metadata.units.dimension);
            record.setTimeOffset(info.metadata.timeOffset.constant
                                 + info.metadata.timeOffset.timestepFraction * step.dt);
            if (!info.metadata.components.empty())
                record.setAttribute("ippl_components", info.metadata.components);
        }
        template <class Record>
        RecordInfo readRecord(Record record) {
            RecordInfo result;
            result.metadata.units.dimension = record.unitDimension();
            result.metadata.timeOffset      = {record.template timeOffset<double>(), 0.0};
            if (record.containsAttribute("ippl_components"))
                result.metadata.components =
                    record.getAttribute("ippl_components").template get<std::vector<std::string>>();
            const auto names = componentNames(result.metadata);
            bool first       = true;
            for (const auto& name : names) {
                require(record.contains(name), "Stored component is missing: " + name);
                auto component  = record[name];
                const auto type = scalarType(component.getDatatype());
                if (first) {
                    result.dataType              = type;
                    result.metadata.units.unitSI = component.unitSI();
                    first                        = false;
                } else
                    require(type == result.dataType
                                && component.unitSI() == result.metadata.units.unitSI,
                            "Mixed component types or units are unsupported");
            }
            return result;
        }

        class OpenPMDOutput final : public WriterBackend {
            IOOptions options_m;
            const Session& session_m;
            std::unique_ptr<::openPMD::Series> series_m;

        public:
            OpenPMDOutput(IOOptions& options, const Session& session)
                : session_m(session) {
                options.openPMD.backendConfig =
                    backendConfiguration(options.openPMD.backend, options.openPMD.backendConfig);
                options_m = options;
            }
            ~OpenPMDOutput() override {
                try {
                    close();
                } catch (...) {
                }
            }
            void validate(const Snapshot&) const override {}
            void open() override {
                collective(session_m, [&] {
                    if (session_m.rank_m)
                        return;
                    std::ofstream descriptor(options_m.directory / "ippl-series.txt");
                    descriptor << (options_m.openPMD.backend == OpenPMDBackend::HDF5
                                       ? "IPPL_OPENPMD 1 data_%T.h5\n"
                                       : "IPPL_OPENPMD 2 BP5 data_%T.bp5\n");
                    descriptor.close();
                    require(!descriptor.fail(), "Cannot write series descriptor");
                    std::ofstream config(options_m.directory / "ippl-backend.json");
                    config << options_m.openPMD.backendConfig << '\n';
                    config.close();
                    require(!config.fail(), "Cannot write backend configuration");
                });
                series_m = std::make_unique<::openPMD::Series>(
                    (options_m.directory / seriesPattern(options_m.openPMD.backend)).string(),
                    ::openPMD::Access::CREATE, session_m.comm_m, options_m.openPMD.backendConfig);
                collective(session_m, [&] {
                    checkBackend(*series_m, options_m.openPMD.backend);
                });
                series_m->setAttribute("ippl_schema", std::uint32_t{1});
            }
            void write(const Snapshot& snapshot) override {
                const auto& step = snapshot.step_m;
                auto iteration   = series_m->iterations[step.step];
                iteration.open();
                iteration.setTime(step.time).setDt(step.dt).setTimeUnitSI(options_m.timeUnitSI);
                for (const auto& data : snapshot.particles_m) {
                    auto species = iteration.particles[data.info_m.name];
                    const Chunk owned{{data.offset_m}, {data.local_m}};
                    const auto total = data.info_m.count;
                    for (const auto& [name, source] : data.sources_m) {
                        auto record = species[name];
                        recordMetadata(record, source.info_m, step);
                        const auto names = componentNames(source.info_m.metadata);
                        for (unsigned c = 0; c < names.size(); ++c) {
                            auto component = record[names[c]];
                            component.resetDataset(
                                ::openPMD::Dataset(backendType(source.info_m.dataType), {total}));
                            component.setUnitSI(source.info_m.metadata.units.unitSI);
                            writeChunks(*series_m, component, source, c, owned,
                                        options_m.stagingBytes, session_m);
                        }
                    }
                    const auto& position = data.sources_m.at("position");
                    auto offsetRecord    = species["positionOffset"];
                    recordMetadata(offsetRecord, position.info_m, step);
                    for (const auto& name : position.info_m.metadata.components) {
                        auto component = offsetRecord[name];
                        component.resetDataset(
                            ::openPMD::Dataset(backendType(position.info_m.dataType), {total}));
                        component.setUnitSI(position.info_m.metadata.units.unitSI);
                        std::visit(
                            [&](const auto& values) {
                                using Scalar = typename std::decay_t<decltype(values)>::value_type;
                                component.makeConstant(Scalar{0});
                            },
                            buffer(position.info_m.dataType));
                    }
                }
                for (const auto& data : snapshot.fields_m) {
                    const auto& info = data.info_m;
                    auto mesh        = iteration.meshes[info.name];
                    recordMetadata(mesh, data.source_m.info_m, step);
                    mesh.setGeometry(::openPMD::Mesh::Geometry::cartesian);
                    mesh.setDataOrder(::openPMD::Mesh::DataOrder::C);
                    mesh.setAxisLabels(info.metadata.axisLabels);
                    mesh.setGridSpacing(info.spacing)
                        .setGridGlobalOffset(info.gridGlobalOffset)
                        .setGridUnitSI(info.metadata.gridUnitSI);
                    mesh.setAttribute("ippl_indexBegin", info.indexBegin);
                    mesh.setAttribute("ippl_meshOrigin", info.meshOrigin);
                    const auto names = componentNames(info.metadata.record);
                    for (unsigned c = 0; c < names.size(); ++c) {
                        auto component = mesh[names[c]];
                        component.resetDataset(
                            ::openPMD::Dataset(backendType(info.dataType), info.extent));
                        component.setUnitSI(info.metadata.record.units.unitSI);
                        component.setPosition(info.metadata.samplePosition);
                        writeChunks(*series_m, component, data.source_m, c, data.owned_m,
                                    options_m.stagingBytes, session_m);
                    }
                }
                iteration.close();
                flushOutput(*series_m);
            }
            void flush() override { flushOutput(*series_m); }
            void close() override {
                if (series_m) {
                    series_m->close();
                    series_m.reset();
                }
            }
        };
        class OpenPMDInput final : public ReaderBackend {
            std::filesystem::path directory_m;
            const Session& session_m;
            std::unique_ptr<::openPMD::Series> series_m;

        public:
            OpenPMDInput(std::filesystem::path directory, const Session& session)
                : directory_m(std::move(directory))
                , session_m(session) {
                info_m = {IOBackend::OpenPMD, OpenPMDBackend::HDF5, 1.0, {}};
            }
            ~OpenPMDInput() override {
                try {
                    close();
                } catch (...) {
                }
            }
            void open() override {
                auto& self = *this;
                try {
                    agree(self.session_m, self.directory_m.string(), "Input directory");
                    std::string pattern, config;
                    collective(self.session_m, [&] {
                        std::ifstream file(self.directory_m / "ippl-series.txt");
                        std::string header, token, extra;
                        unsigned version = 0;
                        file >> header >> version >> token;
                        require(bool(file) && header == "IPPL_OPENPMD",
                                "Invalid IPPL series descriptor");
                        if (version == 1 && token == "data_%T.h5") {
                            self.info_m.openPMDBackend.value() = OpenPMDBackend::HDF5;
                            pattern                            = token;
                        } else if (version == 2 && token == "BP5") {
                            self.info_m.openPMDBackend.value() = OpenPMDBackend::BP5;
                            file >> pattern;
                            require(bool(file), "Missing BP5 series pattern");
                        } else
                            throw IpplException("openPMD", "Unsupported IPPL series descriptor");
                        require(pattern == seriesPattern(self.info_m.openPMDBackend.value())
                                    && !(file >> extra),
                                "Invalid IPPL series pattern or trailing descriptor data");
                        config =
                            backendConfiguration(self.info_m.openPMDBackend.value(), "{}", true);
                    });
                    agree(self.session_m, pattern, "Input series pattern");
                    agree(self.session_m, config, "Input backend configuration");
                    self.series_m = std::make_unique<::openPMD::Series>(
                        (self.directory_m / pattern).string(), ::openPMD::Access::READ_ONLY,
                        self.session_m.comm_m, config);
                    collective(self.session_m, [&] {
                        checkBackend(*self.series_m, self.info_m.openPMDBackend.value());
                        require(
                            self.series_m->containsAttribute("ippl_schema")
                                && self.series_m->getAttribute("ippl_schema").get<std::uint32_t>()
                                       == 1,
                            "Unsupported IPPL output schema");
                    });
                    for (auto& [index, iteration] : self.series_m->iterations) {
                        iteration.open();
                        IterationInfo info{
                            {index, iteration.time<double>(), iteration.dt<double>()}, {}, {}};
                        collective(self.session_m, [&] {
                            if (self.info_m.iterations.empty())
                                self.info_m.timeUnitSI = iteration.timeUnitSI();
                            require(self.info_m.timeUnitSI == iteration.timeUnitSI(),
                                    "Changing time units in series");
                            for (auto& [name, species] : iteration.particles) {
                                ParticleInfo particle{name, 0, {}};
                                bool first = true;
                                for (auto& [recordName, record] : species) {
                                    auto metadata = readRecord(record);
                                    for (const auto& componentName :
                                         componentNames(metadata.metadata)) {
                                        auto shape = record[componentName].getExtent();
                                        require(shape.size() == 1,
                                                "Particle components must be one-dimensional");
                                        if (first) {
                                            particle.count = shape[0];
                                            first          = false;
                                        }
                                        require(shape[0] == particle.count,
                                                "Particle record lengths differ");
                                    }
                                    particle.records.emplace(recordName, std::move(metadata));
                                }
                                require(particle.records.contains("position")
                                            && particle.records.contains("positionOffset"),
                                        "Missing particle positions");
                                info.particles.push_back(std::move(particle));
                            }
                            for (auto& [name, mesh] : iteration.meshes) {
                                const auto record = readRecord(mesh);
                                FieldInfo field{name, record.dataType, {}, {}, {}, {}, {}, {}};
                                field.metadata.record     = record.metadata;
                                field.metadata.gridUnitSI = mesh.gridUnitSI();
                                field.metadata.axisLabels = mesh.axisLabels();
                                field.indexBegin          = mesh.getAttribute("ippl_indexBegin")
                                                                .get<std::vector<std::int64_t>>();
                                field.meshOrigin =
                                    mesh.getAttribute("ippl_meshOrigin").get<std::vector<double>>();
                                field.spacing          = mesh.gridSpacing<double>();
                                field.gridGlobalOffset = mesh.gridGlobalOffset();
                                bool first             = true;
                                for (const auto& componentName : componentNames(record.metadata)) {
                                    auto component = mesh[componentName];
                                    if (first) {
                                        field.extent = component.getExtent();
                                        field.metadata.samplePosition =
                                            component.position<double>();
                                        first = false;
                                    }
                                    require(field.extent == component.getExtent()
                                                && field.metadata.samplePosition
                                                       == component.position<double>(),
                                            "Field components have differing shapes or placement");
                                }
                                require(mesh.geometry() == ::openPMD::Mesh::Geometry::cartesian
                                            && mesh.dataOrder() == ::openPMD::Mesh::DataOrder::C,
                                        "Unsupported mesh geometry/order");
                                info.fields.push_back(std::move(field));
                            }
                        });
                        self.info_m.iterations.push_back(index);
                        self.iterations_m.emplace(index, std::move(info));
                    }
                } catch (...) {
                    self.series_m.reset();
                    throw;
                }
            }
            Buffer read(std::uint64_t step, std::string_view species, std::string_view record,
                        std::string_view key, const Chunk& region, DataType type,
                        bool field) override {
                auto& self = *this;
                Buffer result;
                collective(session_m, [&] {
                    result = buffer(type, std::max<std::uint64_t>(1, volume(region.extent)));
                });
                auto iteration = self.series_m->iterations[step];
                auto load      = [&](auto source) {
                    std::visit(
                        [&](auto& values) {
                            using Scalar = typename std::decay_t<decltype(values)>::value_type;
                            // Submit empty selections too: HDF5 dataset opens are collective.
                            auto destination =
                                std::shared_ptr<Scalar>(values.data(), [](Scalar*) {});
                            source.loadChunk(destination, region.offset, region.extent);
                        },
                        result);
                };
                if (field)
                    load(iteration.meshes[std::string(record)][std::string(key)]);
                else
                    load(iteration.particles[std::string(species)][std::string(record)]
                                            [std::string(key)]);
                self.series_m->flush();
                if (!volume(region.extent))
                    std::visit(
                        [](auto& values) {
                            values.clear();
                        },
                        result);
                return result;
            }
            void close() override {
                if (series_m) {
                    for (auto& [step, iteration] : series_m->iterations)
                        iteration.close();
                    series_m->close();
                    series_m.reset();
                }
            }
        };
    }  // namespace
    std::unique_ptr<WriterBackend> openPMDWriter(IOOptions& options, const Session& session) {
        return std::make_unique<OpenPMDOutput>(options, session);
    }
    std::unique_ptr<ReaderBackend> openPMDReader(const std::filesystem::path& directory,
                                                 const Session& session) {
        return std::make_unique<OpenPMDInput>(directory, session);
    }
}  // namespace ippl::detail::io
