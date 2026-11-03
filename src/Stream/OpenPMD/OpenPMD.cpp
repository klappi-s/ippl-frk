#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <numeric>
#include <openPMD/openPMD.hpp>
#include <openPMD/auxiliary/JSON.hpp>
#include <optional>
#include <set>
#include <sstream>

#include "Stream/OpenPMD/OpenPMDReader.h"
#include "Stream/OpenPMD/OpenPMDWriter.h"

namespace ippl::detail::openpmd {
    std::uint64_t volume(const std::vector<std::uint64_t>& extent) {
        std::uint64_t result = 1;
        for (auto n : extent) {
            if (!n)
                return 0;
            require(result <= std::numeric_limits<std::uint64_t>::max() / n,
                    "Array extent overflows uint64");
            result *= n;
        }
        return result;
    }
}  // namespace ippl::detail::openpmd

namespace ippl {
    namespace {
        using namespace detail::openpmd;

        struct Session {
            MPI_Comm comm_m = MPI_COMM_NULL;
            int rank_m      = 0;
            int size_m      = 1;
            void start(MPI_Comm communicator) {
                require(communicator != MPI_COMM_NULL,
                        "A valid simulation communicator is required");
                MPI_Comm_dup(communicator, &comm_m);
                MPI_Comm_rank(comm_m, &rank_m);
                MPI_Comm_size(comm_m, &size_m);
            }
            void release() {
                if (comm_m != MPI_COMM_NULL)
                    MPI_Comm_free(&comm_m);
            }
            ~Session() {
                int finalized = 0;
                MPI_Finalized(&finalized);
                if (!finalized)
                    release();
            }
        };

        // Agree local validation/allocation failures before any backend collective.
        template <class Function>
        void collective(const Session& session, Function&& function) {
            std::string error;
            try {
                function();
            } catch (const IpplException& e) {
                error = e.what();
            } catch (const std::exception& e) {
                error = e.what();
            } catch (...) {
                error = "Unknown local I/O error";
            }
            int failed = error.empty() ? session.size_m : session.rank_m;
            int first  = session.size_m;
            MPI_Allreduce(&failed, &first, 1, MPI_INT, MPI_MIN, session.comm_m);
            if (first == session.size_m)
                return;
            // Bound the diagnostic so MPI count conversion is always safe.
            if (error.size() > 4096)
                error.resize(4096);
            int length = static_cast<int>(error.size());
            MPI_Bcast(&length, 1, MPI_INT, first, session.comm_m);
            error.resize(length);
            MPI_Bcast(error.data(), length, MPI_CHAR, first, session.comm_m);
            throw IpplException("openPMD", "Rank " + std::to_string(first) + ": " + error);
        }
        void agree(const Session& session, const std::string& value,
                   const std::string& description) {
            collective(session, [&] {
                require(value.size() <= static_cast<std::size_t>(INT_MAX),
                        description + " exceeds MPI string length");
            });
            int length = static_cast<int>(value.size());
            MPI_Bcast(&length, 1, MPI_INT, 0, session.comm_m);
            std::string first(length, '\0');
            if (!session.rank_m)
                first = value;
            MPI_Bcast(first.data(), length, MPI_CHAR, 0, session.comm_m);
            collective(session, [&] {
                require(value == first, description + " differs between ranks");
            });
        }
        const char* seriesPattern(OpenPMDBackend backend) {
            switch (backend) {
                case OpenPMDBackend::HDF5:
                    require(IPPL_OPENPMD_HAVE_HDF5, "IPPL was built without openPMD HDF5 support");
                    return "data_%T.h5";
                case OpenPMDBackend::BP5:
                    require(IPPL_OPENPMD_HAVE_ADIOS2, "IPPL was built without openPMD ADIOS2 support");
                    return "data_%T.bp5";
            }
            throw IpplException("openPMD", "Unknown output backend");
        }
        std::string backendConfiguration(OpenPMDBackend backend, const std::string& input,
                                         bool reading = false) {
            seriesPattern(backend); // Check build capabilities before creating any files.
            const std::string required = backend == OpenPMDBackend::HDF5
                ? R"({"backend":"hdf5","iteration_encoding":"file_based"})"
                : std::string(R"({"backend":"adios2","iteration_encoding":"file_based","adios2":{"engine":{"type":"bp5","preferred_flush_target":"disk","access_mode":")")
                    + (reading ? "readrandomaccess" : "write")
                    + R"(","parameters":{"asyncwrite":"off"}}}})";
            // Resolve files/TOML locally, then agree the resulting JSON on all ranks.
            // Lowercase only the validation copy: ADIOS2 parameter names are case insensitive.
            const auto canonical = ::openPMD::json::merge(input, "{}");
            auto folded = canonical;
            for (auto& c : folded)
                if (c >= 'A' && c <= 'Z')
                    c += 'a' - 'A';
            const auto requested = ::openPMD::json::merge(required, folded);
            require(::openPMD::json::merge(requested, required) == requested,
                    "backendConfig conflicts with the selected backend, file-based encoding "
                    "or synchronous disk output");
            if (backend == OpenPMDBackend::BP5) {
                require(::openPMD::json::merge(requested,
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
                ? R"({"adios2":{"engine":{"preferred_flush_target":"disk"}}})" : "{}");
        }
        std::ostringstream stream() {
            std::ostringstream result;
            result.imbue(std::locale::classic());
            result << std::hexfloat;
            return result;
        }
        bool validName(const std::string& name) {
            return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                       || c == '_';
            });
        }
        bool reserved(const std::string& name) {
            return name == "position" || name == "positionOffset" || name == "ippl_id"
                   || name == "ippl_rank" || name == "id";
        }
        Buffer buffer(DataType type, std::size_t count = 0) {
            switch (type) {
#define IPPL_IO_BUFFER(TAG, TYPE) \
    case DataType::TAG:           \
        return std::vector<TYPE>(count)
                IPPL_IO_BUFFER(INT8, std::int8_t);
                IPPL_IO_BUFFER(UINT8, std::uint8_t);
                IPPL_IO_BUFFER(INT16, std::int16_t);
                IPPL_IO_BUFFER(UINT16, std::uint16_t);
                IPPL_IO_BUFFER(INT32, std::int32_t);
                IPPL_IO_BUFFER(UINT32, std::uint32_t);
                IPPL_IO_BUFFER(INT64, std::int64_t);
                IPPL_IO_BUFFER(UINT64, std::uint64_t);
                IPPL_IO_BUFFER(FLOAT32, float);
                IPPL_IO_BUFFER(FLOAT64, double);
#undef IPPL_IO_BUFFER
            }
            throw IpplException("openPMD", "Unsupported scalar type");
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
        std::size_t scalarBytes(DataType type) {
            return std::visit(
                [](const auto& values) {
                    return sizeof(typename std::decay_t<decltype(values)>::value_type);
                },
                buffer(type));
        }
        void validateRecord(const RecordSource& source, const std::string& name) {
            const auto& metadata = source.info_m.metadata;
            require(validName(name), "Invalid record name: " + name);
            require(std::isfinite(metadata.units.unitSI) && metadata.units.unitSI > 0,
                    name + ": unitSI must be finite and positive");
            for (auto exponent : metadata.units.dimension)
                require(std::isfinite(exponent), name + ": non-finite unit exponent");
            require(std::isfinite(metadata.timeOffset.constant)
                        && std::isfinite(metadata.timeOffset.timestepFraction),
                    name + ": non-finite time offset");
            require(source.vector_m ? metadata.components.size() == source.components_m
                                    : metadata.components.empty(),
                    name + ": component names must match the scalar/vector type");
            std::set<std::string> names;
            for (const auto& component : metadata.components)
                require(validName(component) && names.insert(component).second,
                        name + ": invalid or duplicate component name");
        }
        void signature(std::ostream& out, const RecordInfo& info) {
            out << static_cast<int>(info.dataType) << ' ' << info.metadata.units.unitSI << ' ';
            for (auto value : info.metadata.units.dimension)
                out << value << ' ';
            out << info.metadata.timeOffset.constant << ' '
                << info.metadata.timeOffset.timestepFraction << ' ';
            out << info.metadata.components.size() << ' ';
            for (const auto& name : info.metadata.components)
                out << std::quoted(name) << ' ';
        }
        void signature(std::ostream& out, const FieldInfo& info) {
            out << std::quoted(info.name) << ' ';
            signature(out, {info.dataType, info.metadata.record});
            out << info.metadata.gridUnitSI << ' ' << info.extent.size() << ' ';
            for (std::size_t d = 0; d < info.extent.size(); ++d)
                out << info.indexBegin[d] << ' ' << info.extent[d] << ' ' << info.meshOrigin[d]
                    << ' ' << info.spacing[d] << ' ' << std::quoted(info.metadata.axisLabels[d])
                    << ' ' << info.metadata.samplePosition[d] << ' ';
        }
        std::vector<std::string> componentNames(const RecordMetadata& metadata) {
            return metadata.components.empty()
                       ? std::vector<std::string>{::openPMD::RecordComponent::SCALAR}
                       : metadata.components;
        }
        void validateRegion(const Chunk& region, const std::vector<std::uint64_t>& shape) {
            require(region.offset.size() == shape.size() && region.extent.size() == shape.size(),
                    "Chunk dimensionality mismatch");
            for (std::size_t d = 0; d < shape.size(); ++d)
                require(
                    region.offset[d] <= shape[d] && region.extent[d] <= shape[d] - region.offset[d],
                    "Chunk exceeds record extent");
            (void)volume(region.extent);
        }

        // Rectangular tiles keep temporary buffers bounded, including for long rows.
        struct Tiles {
            std::vector<std::uint64_t> shape_m, tile_m, grid_m;
            std::uint64_t count_m;
            Tiles(const std::vector<std::uint64_t>& shape, std::uint64_t capacity)
                : shape_m(shape) {
                tile_m.resize(shape.size());
                grid_m.resize(shape.size());
                for (std::size_t d = shape.size(); d-- > 0;) {
                    tile_m[d] = std::max<std::uint64_t>(1, std::min(shape[d], capacity));
                    grid_m[d] = shape[d] / tile_m[d] + (shape[d] % tile_m[d] != 0);
                    capacity  = std::max<std::uint64_t>(1, capacity / tile_m[d]);
                }
                count_m = volume(grid_m);
            }
            Chunk at(std::uint64_t index) const {
                Chunk chunk{std::vector<std::uint64_t>(shape_m.size()),
                            std::vector<std::uint64_t>(shape_m.size())};
                for (std::size_t d = shape_m.size(); d-- > 0;) {
                    chunk.offset[d] = (index % grid_m[d]) * tile_m[d];
                    index /= grid_m[d];
                    chunk.extent[d] = std::min(tile_m[d], shape_m[d] - chunk.offset[d]);
                }
                return chunk;
            }
        };
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
                        data  = source.pack_m(index, local);
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
    }  // namespace

    struct OpenPMDWriter::Impl {
        OpenPMDOptions options_m;
        MPI_Comm requested_m;
        Session session_m;
        std::unique_ptr<::openPMD::Series> series_m;
        std::map<std::string, std::unique_ptr<ParticleOutputBinding>> particles_m;
        std::map<std::string, FieldSource> fields_m;
        std::optional<std::uint64_t> lastStep_m;
        bool closed_m = false;
        std::string schema_m;
        bool selected(const ParticleOutputBinding& binding, const std::string& name) const {
            if (name == "position")
                return true;
            if (name == "ippl_id")
                return binding.selection_m.includesParticleIds();
            return binding.selection_m.selects(name);
        }
        std::string validate() const {
            auto out = stream();
            out << std::quoted(options_m.directory.string()) << ' '
                << static_cast<int>(options_m.backend) << ' ' << options_m.timeUnitSI << ' '
                << std::quoted(options_m.backendConfig) << ' ' << options_m.stagingBytes << ' ';
            require(!options_m.directory.empty(), "An output directory is required");
            seriesPattern(options_m.backend);
            require(std::isfinite(options_m.timeUnitSI) && options_m.timeUnitSI > 0,
                    "Invalid timeUnitSI");
            require(options_m.stagingBytes >= 3 * sizeof(double),
                    "stagingBytes is too small for one value");
            require(!particles_m.empty() || !fields_m.empty(), "No output data registered");
            for (const auto& [species, ptr] : particles_m) {
                const auto& binding = *ptr;
                require(validName(species), "Invalid species name: " + species);
                const auto attributes = binding.attributes_m();
                std::map<std::string, const void*> names;
                for (const auto& [identity, name] : attributes) {
                    require(validName(name) && !reserved(name),
                            species + ": invalid/reserved attribute " + name);
                    require(names.emplace(name, identity).second,
                            species + ": duplicate attribute " + name);
                }
                for (const auto& name : binding.selection_m.names())
                    require(names.contains(name),
                            species + ": unknown selected/excluded attribute " + name);
                for (const auto& [name, identity] : names)
                    if (binding.selection_m.selects(name))
                        require(binding.records_m.contains(name),
                                species + ": missing binding for " + name);
                out << "particles " << std::quoted(species) << ' '
                    << binding.selection_m.includesRankIds() << ' ';
                for (const auto& [name, source] : binding.records_m) {
                    if (name != "position" && name != "ippl_id")
                        require(
                            names.contains(name) && names.at(name) == source.identity_m
                                && source.name_m() == name,
                            species
                                + ": attribute renamed, removed or belongs to another container: "
                                + name);
                    if (!selected(binding, name))
                        continue;
                    validateRecord(source, species + "_" + name);
                    require(source.size_m() == binding.count_m(),
                            species + ": live attribute length mismatch: " + name);
                    out << std::quoted(name) << ' ';
                    signature(out, source.info_m);
                }
            }
            for (const auto& [name, source] : fields_m) {
                validateRecord(source.values_m, name);
                const auto info = source.info_m();
                const auto dim  = info.extent.size();
                require(info.metadata.axisLabels.size() == dim
                            && info.metadata.samplePosition.size() == dim,
                        name + ": axis/placement dimensionality mismatch");
                require(std::isfinite(info.metadata.gridUnitSI) && info.metadata.gridUnitSI > 0,
                        name + ": invalid gridUnitSI");
                std::set<std::string> axes;
                for (std::size_t d = 0; d < dim; ++d) {
                    require(
                        info.extent[d] > 0 && info.extent[d] <= static_cast<std::uint64_t>(INT_MAX),
                        name + ": invalid field extent");
                    require(std::isfinite(info.spacing[d]) && info.spacing[d] > 0
                                && std::isfinite(info.meshOrigin[d])
                                && std::isfinite(info.gridGlobalOffset[d]),
                            name + ": invalid field geometry");
                    require(validName(info.metadata.axisLabels[d])
                                && axes.insert(info.metadata.axisLabels[d]).second,
                            name + ": invalid/duplicate axis label");
                    require(std::isfinite(info.metadata.samplePosition[d])
                                && info.metadata.samplePosition[d] >= 0
                                && info.metadata.samplePosition[d] < 1,
                            name + ": samplePosition must be in [0,1)");
                }
                validateRegion(source.owned_m(), info.extent);
                int comparison;
                MPI_Comm_compare(source.communicator_m(), session_m.comm_m, &comparison);
                require(comparison == MPI_IDENT || comparison == MPI_CONGRUENT,
                        name + ": field communicator differs");
                out << "field ";
                signature(out, info);
            }
            return out.str();
        }
        void configuring() const {
            require(!series_m && !closed_m, "Registration is only allowed before open()");
        }
    };

    void ParticleOutputBinding::addAttribute(std::string name, RecordSource source) {
        require(!frozen_m, "Particle bindings are frozen after open()");
        require(!reserved(name), "Reserved particle record: " + name);
        require(records_m.emplace(std::move(name), std::move(source)).second,
                "Duplicate particle binding");
    }
    OpenPMDWriter::OpenPMDWriter(OpenPMDOptions options, MPI_Comm communicator)
        : impl_m(std::make_unique<Impl>()) {
        impl_m->options_m   = std::move(options);
        impl_m->requested_m = communicator;
    }
    OpenPMDWriter::~OpenPMDWriter() {
        try {
            close();
        } catch (...) {
        }
    }
    ParticleOutputBinding& OpenPMDWriter::addParticleSource(
        std::string name, std::unique_ptr<ParticleOutputBinding> binding) {
        impl_m->configuring();
        auto [it, inserted] = impl_m->particles_m.emplace(std::move(name), std::move(binding));
        require(inserted, "Duplicate particle species");
        return *it->second;
    }
    void OpenPMDWriter::addFieldSource(std::string name, FieldSource source) {
        impl_m->configuring();
        require(impl_m->fields_m.emplace(std::move(name), std::move(source)).second,
                "Duplicate field record");
    }
    void OpenPMDWriter::setParticleAttributeSelection(std::string_view name,
                                                      ParticleAttributeSelection selection) {
        impl_m->configuring();
        const auto it = impl_m->particles_m.find(std::string(name));
        require(it != impl_m->particles_m.end(), "Unknown particle species: " + std::string(name));
        it->second->selection_m = std::move(selection);
    }
    void OpenPMDWriter::open() {
        auto& self = *impl_m;
        self.configuring();
        self.session_m.start(self.requested_m);
        try {
            collective(self.session_m, [&] {
                self.options_m.backendConfig = backendConfiguration(
                    self.options_m.backend, self.options_m.backendConfig);
                self.schema_m = self.validate();
            });
            agree(self.session_m, self.schema_m, "Output schema/options");
            collective(self.session_m, [&] {
                if (self.session_m.rank_m)
                    return;
                const auto parent = self.options_m.directory.parent_path();
                if (!parent.empty())
                    std::filesystem::create_directories(parent);
                require(std::filesystem::create_directory(self.options_m.directory),
                        "Output directory already exists");
                std::ofstream descriptor(self.options_m.directory / "ippl-series.txt");
                descriptor << (self.options_m.backend == OpenPMDBackend::HDF5
                    ? "IPPL_OPENPMD 1 data_%T.h5\n" : "IPPL_OPENPMD 2 BP5 data_%T.bp5\n");
                descriptor.close();
                require(!descriptor.fail(), "Cannot write series descriptor");
                std::ofstream config(self.options_m.directory / "ippl-backend.json");
                config << self.options_m.backendConfig << '\n';
                config.close();
                require(!config.fail(), "Cannot write backend configuration");
            });
            self.series_m = std::make_unique<::openPMD::Series>(
                (self.options_m.directory / seriesPattern(self.options_m.backend)).string(), ::openPMD::Access::CREATE,
                self.session_m.comm_m, self.options_m.backendConfig);
            collective(self.session_m, [&] {
                checkBackend(*self.series_m, self.options_m.backend);
            });
            self.series_m->setAttribute("ippl_schema", std::uint32_t{1});
            for (auto& [name, binding] : self.particles_m)
                binding->frozen_m = true;
        } catch (...) {
            self.series_m.reset();
            self.session_m.release();
            throw;
        }
    }
    void OpenPMDWriter::write(const StepInfo& step) {
        auto& self = *impl_m;
        require(bool(self.series_m), "write() requires an open writer");
        auto& session = self.session_m;
        collective(session, [&] {
            require(std::isfinite(step.time) && std::isfinite(step.dt) && step.dt >= 0,
                    "Invalid iteration time/dt");
            require(!self.lastStep_m || step.step > *self.lastStep_m,
                    "Iteration numbers must increase strictly");
            require(self.validate() == self.schema_m,
                    "Output schema or global field geometry changed; create a new segment");
            for (const auto& [name, binding] : self.particles_m)
                for (const auto& [recordName, source] : binding->records_m)
                    if (self.selected(*binding, recordName))
                        require(std::isfinite(source.info_m.metadata.timeOffset.constant
                                              + source.info_m.metadata.timeOffset.timestepFraction
                                                    * step.dt),
                                name + ": time offset overflow");
            for (const auto& [name, source] : self.fields_m) {
                const auto& offset = source.values_m.info_m.metadata.timeOffset;
                require(std::isfinite(offset.constant + offset.timestepFraction * step.dt),
                        name + ": time offset overflow");
            }
        });
        auto stamp = stream();
        stamp << step.step << ' ' << step.time << ' ' << step.dt;
        agree(session, stamp.str(), "Iteration time/step");
        // Validate/count every species before opening an iteration on any rank.
        std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> counts;
        for (const auto& [name, binding] : self.particles_m) {
            const auto local = binding->count_m();
            std::vector<std::uint64_t> perRank(session.size_m);
            MPI_Allgather(&local, 1, MPI_UINT64_T, perRank.data(), 1, MPI_UINT64_T, session.comm_m);
            std::uint64_t total = 0, offset = 0;
            collective(session, [&] {
                for (int rank = 0; rank < session.size_m; ++rank) {
                    if (rank == session.rank_m)
                        offset = total;
                    require(perRank[rank] <= std::numeric_limits<std::uint64_t>::max() - total,
                            "Particle count overflow");
                    total += perRank[rank];
                }
            });
            counts.emplace(name, std::make_pair(total, offset));
        }
        auto iteration = self.series_m->iterations[step.step];
        iteration.open();
        iteration.setTime(step.time).setDt(step.dt).setTimeUnitSI(self.options_m.timeUnitSI);
        for (const auto& [name, binding] : self.particles_m) {
            const auto [total, offset] = counts.at(name);
            auto species               = iteration.particles[name];
            const Chunk owned{{offset}, {binding->count_m()}};
            for (const auto& [recordName, source] : binding->records_m) {
                if (!self.selected(*binding, recordName))
                    continue;
                auto record = species[recordName];
                recordMetadata(record, source.info_m, step);
                const auto names = componentNames(source.info_m.metadata);
                for (unsigned c = 0; c < names.size(); ++c) {
                    auto component = record[names[c]];
                    component.resetDataset(
                        ::openPMD::Dataset(backendType(source.info_m.dataType), {total}));
                    component.setUnitSI(source.info_m.metadata.units.unitSI);
                    writeChunks(*self.series_m, component, source, c, owned,
                                self.options_m.stagingBytes, session);
                }
            }
            const auto& position = binding->records_m.at("position");
            auto offsetRecord    = species["positionOffset"];
            recordMetadata(offsetRecord, position.info_m, step);
            for (const auto& componentName : position.info_m.metadata.components) {
                auto component = offsetRecord[componentName];
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
            if (binding->selection_m.includesRankIds()) {
                RecordSource ranks{{DataType::INT32, {{1.0, {}}, {}}},
                                   1,
                                   false,
                                   nullptr,
                                   {},
                                   {},
                                   [&session](unsigned, const Chunk& chunk) -> Buffer {
                                       return std::vector<std::int32_t>(chunk.extent[0],
                                                                        session.rank_m);
                                   }};
                auto record = species["ippl_rank"];
                recordMetadata(record, ranks.info_m, step);
                auto component = record[::openPMD::RecordComponent::SCALAR];
                component.resetDataset(::openPMD::Dataset(backendType(DataType::INT32), {total}));
                component.setUnitSI(1.0);
                writeChunks(*self.series_m, component, ranks, 0, owned, self.options_m.stagingBytes,
                            session);
            }
        }
        for (const auto& [name, source] : self.fields_m) {
            const auto info = source.info_m();
            auto mesh       = iteration.meshes[name];
            recordMetadata(mesh, source.values_m.info_m, step);
            mesh.setGeometry(::openPMD::Mesh::Geometry::cartesian);
            mesh.setDataOrder(::openPMD::Mesh::DataOrder::C);
            mesh.setAxisLabels(info.metadata.axisLabels);
            mesh.setGridSpacing(info.spacing)
                .setGridGlobalOffset(info.gridGlobalOffset)
                .setGridUnitSI(info.metadata.gridUnitSI);
            mesh.setAttribute("ippl_indexBegin", info.indexBegin);
            mesh.setAttribute("ippl_meshOrigin", info.meshOrigin);
            const auto names = componentNames(source.values_m.info_m.metadata);
            for (unsigned c = 0; c < names.size(); ++c) {
                auto component = mesh[names[c]];
                component.resetDataset(::openPMD::Dataset(backendType(info.dataType), info.extent));
                component.setUnitSI(info.metadata.record.units.unitSI);
                component.setPosition(info.metadata.samplePosition);
                writeChunks(*self.series_m, component, source.values_m, c, source.owned_m(),
                            self.options_m.stagingBytes, session);
            }
        }
        iteration.close();
        flushOutput(*self.series_m);
        self.lastStep_m = step.step;
    }
    void OpenPMDWriter::flush() {
        require(bool(impl_m->series_m), "flush() requires an open writer");
        flushOutput(*impl_m->series_m);
    }
    void OpenPMDWriter::close() {
        if (impl_m->closed_m)
            return;
        if (impl_m->series_m) {
            impl_m->series_m->close();
            impl_m->series_m.reset();
        }
        impl_m->session_m.release();
        impl_m->closed_m = true;
    }

    struct OpenPMDReader::Impl {
        std::filesystem::path directory_m;
        MPI_Comm requested_m;
        Session session_m;
        std::unique_ptr<::openPMD::Series> series_m;
        SeriesInfo info_m{OpenPMDBackend::HDF5, 1.0, {}};
        std::map<std::uint64_t, IterationInfo> iterations_m;
        bool closed_m = false;
    };
    OpenPMDReader::OpenPMDReader(std::filesystem::path directory, MPI_Comm communicator)
        : impl_m(std::make_unique<Impl>()) {
        impl_m->directory_m = std::move(directory);
        impl_m->requested_m = communicator;
    }
    OpenPMDReader::~OpenPMDReader() {
        try {
            close();
        } catch (...) {
        }
    }
    void OpenPMDReader::open() {
        auto& self = *impl_m;
        require(!self.series_m && !self.closed_m, "Reader cannot be reopened");
        self.session_m.start(self.requested_m);
        try {
            agree(self.session_m, self.directory_m.string(), "Input directory");
            std::string pattern, config;
            collective(self.session_m, [&] {
                std::ifstream file(self.directory_m / "ippl-series.txt");
                std::string header, token, extra;
                unsigned version = 0;
                file >> header >> version >> token;
                require(bool(file) && header == "IPPL_OPENPMD", "Invalid IPPL series descriptor");
                if (version == 1 && token == "data_%T.h5") {
                    self.info_m.backend = OpenPMDBackend::HDF5;
                    pattern = token;
                } else if (version == 2 && token == "BP5") {
                    self.info_m.backend = OpenPMDBackend::BP5;
                    file >> pattern;
                    require(bool(file), "Missing BP5 series pattern");
                } else
                    throw IpplException("openPMD", "Unsupported IPPL series descriptor");
                require(pattern == seriesPattern(self.info_m.backend) && !(file >> extra),
                        "Invalid IPPL series pattern or trailing descriptor data");
                config = backendConfiguration(self.info_m.backend, "{}", true);
            });
            agree(self.session_m, pattern, "Input series pattern");
            agree(self.session_m, config, "Input backend configuration");
            self.series_m = std::make_unique<::openPMD::Series>(
                (self.directory_m / pattern).string(), ::openPMD::Access::READ_ONLY,
                self.session_m.comm_m, config);
            collective(self.session_m, [&] {
                checkBackend(*self.series_m, self.info_m.backend);
                require(self.series_m->containsAttribute("ippl_schema")
                            && self.series_m->getAttribute("ippl_schema").get<std::uint32_t>() == 1,
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
                            for (const auto& componentName : componentNames(metadata.metadata)) {
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
                        field.indexBegin =
                            mesh.getAttribute("ippl_indexBegin").get<std::vector<std::int64_t>>();
                        field.meshOrigin =
                            mesh.getAttribute("ippl_meshOrigin").get<std::vector<double>>();
                        field.spacing          = mesh.gridSpacing<double>();
                        field.gridGlobalOffset = mesh.gridGlobalOffset();
                        bool first             = true;
                        for (const auto& componentName : componentNames(record.metadata)) {
                            auto component = mesh[componentName];
                            if (first) {
                                field.extent                  = component.getExtent();
                                field.metadata.samplePosition = component.position<double>();
                                first                         = false;
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
            self.session_m.release();
            throw;
        }
    }
    SeriesInfo OpenPMDReader::info() const {
        require(bool(impl_m->series_m), "info() requires an open reader");
        return impl_m->info_m;
    }
    IterationInfo OpenPMDReader::inspect(std::uint64_t step) const {
        require(bool(impl_m->series_m), "inspect() requires an open reader");
        const auto it = impl_m->iterations_m.find(step);
        require(it != impl_m->iterations_m.end(), "Unknown iteration");
        return it->second;
    }
    Buffer OpenPMDReader::read(std::uint64_t step, std::string_view species,
                               std::string_view record, std::string_view component,
                               const Chunk& region, DataType type, bool field) {
        auto& self = *impl_m;
        require(bool(self.series_m), "read requires an open reader");
        auto name = stream();
        name << step << ' ' << field << ' ' << std::quoted(std::string(species)) << ' '
             << std::quoted(std::string(record)) << ' ' << std::quoted(std::string(component))
             << ' ' << static_cast<int>(type);
        agree(self.session_m, name.str(), "Component read");
        Buffer result;
        const auto key =
            component.empty() ? ::openPMD::RecordComponent::SCALAR : std::string(component);
        collective(self.session_m, [&] {
            const auto iteration     = inspect(step);
            const RecordInfo* values = nullptr;
            RecordInfo fieldRecord;
            std::vector<std::uint64_t> shape;
            if (field) {
                const auto it = std::find_if(iteration.fields.begin(), iteration.fields.end(),
                                             [&](const auto& value) {
                                                 return value.name == record;
                                             });
                require(it != iteration.fields.end(), "Unknown field record");
                shape       = it->extent;
                fieldRecord = {it->dataType, it->metadata.record};
                values      = &fieldRecord;
            } else {
                const auto it = std::find_if(iteration.particles.begin(), iteration.particles.end(),
                                             [&](const auto& value) {
                                                 return value.name == species;
                                             });
                require(it != iteration.particles.end(), "Unknown particle species");
                const auto found = it->records.find(std::string(record));
                require(found != it->records.end(), "Unknown particle record");
                values = &found->second;
                shape  = {it->count};
            }
            require(type == values->dataType, "Requested scalar type differs from stored type");
            const auto names = componentNames(values->metadata);
            require(std::find(names.begin(), names.end(), key) != names.end(), "Unknown component");
            validateRegion(region, shape);
            const auto count = volume(region.extent);
            require(count <= std::numeric_limits<std::size_t>::max() / scalarBytes(type),
                    "Read buffer too large");
            // openPMD requires a non-null pointer even for an empty selection.
            result = buffer(type, static_cast<std::size_t>(std::max<std::uint64_t>(1, count)));
        });
        auto iteration = self.series_m->iterations[step];
        auto load      = [&](auto source) {
            std::visit(
                [&](auto& values) {
                    using Scalar = typename std::decay_t<decltype(values)>::value_type;
                    // Submit empty selections too: HDF5 dataset opens are collective.
                    auto destination = std::shared_ptr<Scalar>(values.data(), [](Scalar*) {});
                    source.loadChunk(destination, region.offset, region.extent);
                },
                result);
        };
        if (field)
            load(iteration.meshes[std::string(record)][key]);
        else
            load(iteration.particles[std::string(species)][std::string(record)][key]);
        self.series_m->flush();
        if (!volume(region.extent))
            std::visit(
                [](auto& values) {
                    values.clear();
                },
                result);
        return result;
    }
    void OpenPMDReader::close() {
        if (impl_m->closed_m)
            return;
        if (impl_m->series_m) {
            // HDF5 file closes are collective. Use iteration order, not backend handle order.
            for (auto& [step, iteration] : impl_m->series_m->iterations)
                iteration.close();
            impl_m->series_m->close();
            impl_m->series_m.reset();
        }
        impl_m->session_m.release();
        impl_m->closed_m = true;
    }
}  // namespace ippl
