#include <fstream>

#include "Stream/IO/IOReader.h"
#include "Stream/IO/IOWriter.h"
#include "Stream/IO/detail/Backend.h"
namespace ippl::detail::io {
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
}  // namespace ippl::detail::io

namespace ippl {
    using namespace detail::io;
    struct IOWriter::Impl {
        IOOptions options_m;
        MPI_Comm requested_m;
        Session session_m;
        std::unique_ptr<WriterBackend> backend_m;
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
                << static_cast<int>(options_m.openPMD.backend) << ' '
                << static_cast<int>(options_m.h5hut.transferMode) << ' '
                << static_cast<int>(options_m.backend) << ' ' << options_m.timeUnitSI << ' '
                << std::quoted(options_m.openPMD.backendConfig) << ' ' << options_m.stagingBytes
                << ' ';
            require(!options_m.directory.empty(), "An output directory is required");
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
            require(!backend_m && !closed_m, "Registration is only allowed before open()");
        }
    };

    void ParticleOutputBinding::addAttribute(std::string name, RecordSource source) {
        require(!frozen_m, "Particle bindings are frozen after open()");
        require(!reserved(name), "Reserved particle record: " + name);
        require(records_m.emplace(std::move(name), std::move(source)).second,
                "Duplicate particle binding");
    }
    IOWriter::IOWriter(IOOptions options, MPI_Comm communicator)
        : impl_m(std::make_unique<Impl>()) {
        impl_m->options_m   = std::move(options);
        impl_m->requested_m = communicator;
    }
    IOWriter::~IOWriter() {
        try {
            close();
        } catch (...) {
        }
    }
    ParticleOutputBinding& IOWriter::addParticleSource(
        std::string name, std::unique_ptr<ParticleOutputBinding> binding) {
        impl_m->configuring();
        auto [it, inserted] = impl_m->particles_m.emplace(std::move(name), std::move(binding));
        require(inserted, "Duplicate particle species");
        return *it->second;
    }
    void IOWriter::addFieldSource(std::string name, FieldSource source) {
        impl_m->configuring();
        require(impl_m->fields_m.emplace(std::move(name), std::move(source)).second,
                "Duplicate field record");
    }
    void IOWriter::setParticleAttributeSelection(std::string_view name,
                                                 ParticleAttributeSelection selection) {
        impl_m->configuring();
        const auto it = impl_m->particles_m.find(std::string(name));
        require(it != impl_m->particles_m.end(), "Unknown particle species: " + std::string(name));
        it->second->selection_m = std::move(selection);
    }

    void IOWriter::open() {
        auto& self = *impl_m;
        self.configuring();
        self.session_m.start(self.requested_m);
        bool creatingOutput = false;
        try {
            collective(self.session_m, [&] {
                switch (self.options_m.backend) {
#ifdef IPPL_ENABLE_OPENPMD
                    case IOBackend::OpenPMD:
                        self.options_m.h5hut = {};
                        self.backend_m       = openPMDWriter(self.options_m, self.session_m);
                        break;
#endif
#ifdef IPPL_ENABLE_H5HUT
                    case IOBackend::H5hut:
                        self.options_m.openPMD = {};
                        self.backend_m         = h5hutWriter(self.options_m, self.session_m);
                        break;
#endif
                    default:
                        throw IpplException("I/O", "Requested I/O backend is unavailable");
                }
                self.schema_m = self.validate();
                // Preflight selected types and field staging before creating the directory.
                Snapshot schema{};
                for (const auto& [name, binding] : self.particles_m) {
                    ParticleData data{};
                    data.info_m.name = name;
                    data.local_m     = binding->count_m();
                    for (const auto& [record, source] : binding->records_m)
                        if (self.selected(*binding, record))
                            data.info_m.records.emplace(record, source.info_m);
                    schema.particles_m.push_back(std::move(data));
                }
                for (const auto& [name, source] : self.fields_m)
                    schema.fields_m.push_back({source.info_m(), source.owned_m(), source.values_m});
                self.backend_m->validate(schema);
            });
            agree(self.session_m, self.schema_m, "Output schema/options");
            creatingOutput = true;
            collective(self.session_m, [&] {
                if (self.session_m.rank_m)
                    return;
                const auto parent = self.options_m.directory.parent_path();
                if (!parent.empty())
                    std::filesystem::create_directories(parent);
                require(std::filesystem::create_directory(self.options_m.directory),
                        "Output directory already exists");
            });
            self.backend_m->open();
            for (auto& [name, binding] : self.particles_m)
                binding->frozen_m = true;
        } catch (...) {
            self.backend_m.reset();
            self.session_m.release();
            self.closed_m = creatingOutput;
            throw;
        }
    }

    void IOWriter::write(const StepInfo& step) {
        auto& self = *impl_m;
        require(bool(self.backend_m) && !self.closed_m, "write() requires an open writer");
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

        Snapshot snapshot{step, {}, {}};
        collective(session, [&] {
            for (const auto& [name, binding] : self.particles_m) {
                const auto [total, offset] = counts.at(name);
                ParticleData data{};
                data.info_m.name  = name;
                data.info_m.count = total;
                data.local_m      = binding->count_m();
                data.offset_m     = offset;
                for (const auto& [record, source] : binding->records_m)
                    if (self.selected(*binding, record)) {
                        data.info_m.records.emplace(record, source.info_m);
                        data.sources_m.emplace(record, source);
                    }
                if (binding->selection_m.includesRankIds()) {
                    RecordSource ranks{{DataType::INT32, {{1.0, {}}, {}}},
                                       1,
                                       false,
                                       nullptr,
                                       {},
                                       {},
                                       [rank = session.rank_m](unsigned, const Chunk& chunk,
                                                               ArrayOrder) -> Buffer {
                                           return std::vector<std::int32_t>(chunk.extent[0], rank);
                                       }};
                    data.info_m.records.emplace("ippl_rank", ranks.info_m);
                    data.sources_m.emplace("ippl_rank", std::move(ranks));
                }
                snapshot.particles_m.push_back(std::move(data));
            }
            for (const auto& [name, source] : self.fields_m)
                snapshot.fields_m.push_back({source.info_m(), source.owned_m(), source.values_m});
            self.backend_m->validate(snapshot);
        });
        try {
            self.backend_m->write(snapshot);
            self.lastStep_m = step.step;
        } catch (...) {
            self.closed_m = true;  // Backend failure makes the series unusable.
            self.backend_m.reset();
            self.session_m.release();
            throw;
        }
    }
    void IOWriter::flush() {
        require(bool(impl_m->backend_m) && !impl_m->closed_m, "flush() requires an open writer");
        impl_m->backend_m->flush();
    }
    void IOWriter::close() {
        if (impl_m->closed_m)
            return;
        if (impl_m->backend_m) {
            impl_m->backend_m->close();
            impl_m->backend_m.reset();
        }
        impl_m->session_m.release();
        impl_m->closed_m = true;
    }

    struct IOReader::Impl {
        std::filesystem::path directory_m;
        MPI_Comm requested_m;
        Session session_m;
        std::unique_ptr<ReaderBackend> backend_m;
        bool closed_m = false;
    };
    IOReader::IOReader(std::filesystem::path directory, MPI_Comm communicator)
        : impl_m(std::make_unique<Impl>()) {
        impl_m->directory_m = std::move(directory);
        impl_m->requested_m = communicator;
    }
    IOReader::~IOReader() {
        try {
            close();
        } catch (...) {
        }
    }
    void IOReader::open() {
        auto& self = *impl_m;
        require(!self.backend_m && !self.closed_m, "Reader cannot be reopened");
        self.session_m.start(self.requested_m);
        try {
            agree(self.session_m, self.directory_m.string(), "Input directory");
            std::string descriptor;
            collective(self.session_m, [&] {
                std::ifstream file(self.directory_m / "ippl-series.txt");
                std::getline(file, descriptor);
                require(bool(file) && descriptor.size() < 4096, "Invalid series descriptor");
            });
            agree(self.session_m, descriptor, "Input series descriptor");
            collective(self.session_m, [&] {
#ifdef IPPL_ENABLE_OPENPMD
                if (descriptor.starts_with("IPPL_OPENPMD "))
                    self.backend_m = openPMDReader(self.directory_m, self.session_m);
#endif
#ifdef IPPL_ENABLE_H5HUT
                if (descriptor == "IPPL_IO 1 H5HUT")
                    self.backend_m = h5hutReader(self.directory_m, self.session_m);
#endif
                require(bool(self.backend_m), "Invalid series descriptor or unavailable backend");
            });
            self.backend_m->open();
        } catch (...) {
            self.backend_m.reset();
            self.session_m.release();
            self.closed_m = true;
            throw;
        }
    }
    IOSeriesInfo IOReader::info() const {
        require(bool(impl_m->backend_m), "info() requires an open reader");
        return impl_m->backend_m->info_m;
    }
    IterationInfo IOReader::inspect(std::uint64_t step) const {
        require(bool(impl_m->backend_m), "inspect() requires an open reader");
        const auto it = impl_m->backend_m->iterations_m.find(step);
        require(it != impl_m->backend_m->iterations_m.end(), "Unknown iteration");
        return it->second;
    }
    Buffer IOReader::read(std::uint64_t step, std::string_view species, std::string_view record,
                          std::string_view component, const Chunk& region, DataType type,
                          bool field) {
        auto& self = *impl_m;
        require(bool(self.backend_m), "read requires an open reader");
        auto name = stream();
        name << step << ' ' << field << ' ' << std::quoted(std::string(species)) << ' '
             << std::quoted(std::string(record)) << ' ' << std::quoted(std::string(component))
             << ' ' << static_cast<int>(type);
        agree(self.session_m, name.str(), "Component read");
        const auto key = component.empty() ? SCALAR_COMPONENT : std::string(component);
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
        });
        return self.backend_m->read(step, species, record, key, region, type, field);
    }
    void IOReader::close() {
        if (impl_m->closed_m)
            return;
        if (impl_m->backend_m) {
            impl_m->backend_m->close();
            impl_m->backend_m.reset();
        }
        impl_m->session_m.release();
        impl_m->closed_m = true;
    }
}  // namespace ippl
