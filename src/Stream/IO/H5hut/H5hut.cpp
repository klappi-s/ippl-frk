#include <H5hut.h>
#include <charconv>
#include <fstream>

#include "Stream/IO/H5hut/Metadata.h"

namespace ippl::detail::io {
    namespace {
        using namespace h5hut;
        void check(h5_err_t result, const char* operation) {
            require(result >= 0, std::string("H5hut failed: ") + operation);
        }
        h5_int64_t nativeType(DataType type) {
            switch (type) {
                case DataType::FLOAT32:
                    return H5_FLOAT32_T;
                case DataType::FLOAT64:
                    return H5_FLOAT64_T;
                case DataType::INT32:
                    return H5_INT32_T;
                case DataType::INT64:
                    return H5_INT64_T;
                default:
                    throw IpplException("H5hut",
                                        "H5hut supports float32/64 and signed int32/64 datasets");
            }
        }
        struct File {
            h5_file_t handle_m = 0;
            const Session& session_m;
            File(const std::filesystem::path& path, bool write, const Session& session,
                 H5hutTransferMode mode = H5hutTransferMode::Collective)
                : session_m(session) {
                h5_prop_t properties = H5CreateFileProp();
                MPI_Comm comm        = session.comm_m;
                try {
                    collective(session, [&] {
                        // Reject missing payloads before H5hut enters its native error path.
                        // Some installations abort or crash instead of returning an error.
                        if (!write)
                            require(std::filesystem::is_regular_file(path),
                                    "H5hut payload is missing or not a regular file: "
                                        + path.string());
                        require(properties != static_cast<h5_prop_t>(H5_ERR),
                                "Cannot create H5hut file properties");
                        check(mode == H5hutTransferMode::Collective
                                  ? H5SetPropFileMPIOCollective(properties, &comm)
                                  : H5SetPropFileMPIOIndependent(properties, &comm),
                              "MPI file properties");
                    });
                    handle_m =
                        H5OpenFile(path.c_str(), write ? H5_O_WRONLY : H5_O_RDONLY, properties);
                    collective(session, [&] {
                        require(handle_m != (h5_file_t)H5_ERR && handle_m != 0,
                                "Cannot open H5hut file: " + path.string());
                    });
                    check(H5CloseProp(properties), "close properties");
                } catch (...) {
                    if (properties != static_cast<h5_prop_t>(H5_ERR))
                        H5CloseProp(properties);
                    throw;
                }
            }
            File(const File&)            = delete;
            File& operator=(const File&) = delete;
            void step(std::uint64_t index) {
                collective(session_m, [&] {
                    check(H5SetStep(handle_m, index), "select step");
                });
            }
            void close() {
                if (!handle_m || handle_m == (h5_file_t)H5_ERR)
                    return;
                auto file = handle_m;
                handle_m  = 0;
                collective(session_m, [&] {
                    check(H5CloseFile(file), "close file");
                });
            }
            ~File() {
                try {
                    close();
                } catch (...) {
                }
            }
        };
        std::filesystem::path particlePath(const std::filesystem::path& dir,
                                           const std::string& name, std::uint64_t step) {
            return dir / ("particles_" + name + "_" + std::to_string(step) + ".h5part");
        }
        std::filesystem::path fieldPath(const std::filesystem::path& dir, const std::string& name,
                                        std::uint64_t step) {
            return dir / ("field_" + name + "_" + std::to_string(step) + ".h5");
        }
        std::string particleName(const ParticleInfo& info, const std::string& record,
                                 unsigned component) {
            if (record == "position")
                return std::array<std::string, 3>{"x", "y", "z"}.at(component);
            if (record == "ippl_id")
                return "id";
            if (record == "ippl_rank")
                return "rank";
            // Short physical names avoid H5hut's 64-byte truncation. The sorted logical
            // record table, stored in metadata, defines this collision-free mapping.
            unsigned index = 0;
            for (const auto& [name, values] : info.records) {
                if (name == "positionOffset")
                    continue;
                if (name == record)
                    return "record_" + std::to_string(index) + "_" + std::to_string(component);
                ++index;
            }
            throw IpplException("H5hut", "Unknown particle record");
        }
        std::string fieldName(unsigned component) {
            return "component_" + std::to_string(component);
        }
        void transfer(h5_file_t file, const std::string& name, Buffer& data, bool write,
                      bool field) {
            std::visit(
                [&](auto& values) {
                    using T         = typename std::decay_t<decltype(values)>::value_type;
                    h5_err_t result = H5_ERR;
#define IPPL_H5_TRANSFER(TYPE, TAG)                                                            \
    if constexpr (std::is_same_v<T, TYPE>) {                                                   \
        if (field)                                                                             \
            result = write ? H5Block3dWriteScalarField##TAG(file, name.c_str(), values.data()) \
                           : H5Block3dReadScalarField##TAG(file, name.c_str(), values.data()); \
        else                                                                                   \
            result = write ? H5PartWriteData##TAG(file, name.c_str(), values.data())           \
                           : H5PartReadData##TAG(file, name.c_str(), values.data());           \
    }
                    IPPL_H5_TRANSFER(float, Float32)
                    IPPL_H5_TRANSFER(double, Float64)
                    IPPL_H5_TRANSFER(std::int32_t, Int32)
                    IPPL_H5_TRANSFER(std::int64_t, Int64)
#undef IPPL_H5_TRANSFER
                    check(result, write ? "write dataset" : "read dataset");
                },
                data);
        }
        void setParticleView(h5_file_t file, std::uint64_t offset, std::uint64_t count,
                             std::uint64_t total) {
            if (!total)
                return;  // SetNumParticles(0) supplies a valid empty global dataspace.
            if (count)
                check(H5PartSetView(file, offset, offset + count - 1), "particle view");
            else {
                h5_size_t dummy = 0;
                check(H5PartSetViewIndices(file, &dummy, 0), "empty particle view");
            }
        }
        void setFieldView(h5_file_t file, const Chunk& region) {
            std::array<h5_size_t, 3> start{0, 0, 0}, end{0, 0, 0};
            for (std::size_t d = 0; d < region.extent.size(); ++d) {
                start[d] = region.offset[d];
                end[d]   = start[d] + region.extent[d] - 1;
            }
            check(H5Block3dSetView(file, start[0], end[0], start[1], end[1], start[2], end[2]),
                  "field view");
        }
        void activeSession(const Session& parent, bool active, Session& subset) {
            MPI_Comm_split(parent.comm_m, active ? 0 : MPI_UNDEFINED, parent.rank_m,
                           &subset.comm_m);
            if (active) {
                MPI_Comm_rank(subset.comm_m, &subset.rank_m);
                MPI_Comm_size(subset.comm_m, &subset.size_m);
            }
        }
        void writeMetadata(File& file, const Snapshot& snapshot, double timeUnitSI,
                           const std::string& text) {
            collective(file.session_m, [&] {
                check(H5WriteFileAttribString(file.handle_m, "ippl_metadata", text.c_str()),
                      "metadata");
                check(H5WriteStepAttribFloat64(file.handle_m, "time", &snapshot.step_m.time, 1),
                      "time");
                check(H5WriteStepAttribFloat64(file.handle_m, "dt", &snapshot.step_m.dt, 1), "dt");
                check(H5WriteStepAttribFloat64(file.handle_m, "timeUnitSI", &timeUnitSI, 1),
                      "time units");
            });
        }
        void verifyMetadata(File& file, const std::string& text) {
            collective(file.session_m, [&] {
                h5_int64_t type;
                h5_size_t length;
                check(H5GetFileAttribInfoByName(file.handle_m, "ippl_metadata", &type, &length),
                      "metadata info");
                require(type == H5_STRING_T && length > 0 && length <= MAX_METADATA_BYTES,
                        "Invalid native metadata attribute");
                std::vector<char> buffer(length + 1, 0);
                check(H5ReadFileAttribString(file.handle_m, "ippl_metadata", buffer.data()),
                      "read metadata");
                require(std::string(buffer.data()) == text,
                        "H5hut file metadata differs from completed step");
            });
        }
        void writeText(const std::filesystem::path& path, const std::string& text) {
            std::ofstream file(path);
            file << text;
            file.close();
            require(!file.fail(), "Cannot write " + path.string());
        }
        std::string readText(const std::filesystem::path& path) {
            require(std::filesystem::file_size(path) < MAX_METADATA_BYTES,
                    "Oversized I/O descriptor");
            std::ifstream file(path);
            require(bool(file), "Cannot read " + path.string());
            return std::string(std::istreambuf_iterator<char>(file), {});
        }

        class H5hutOutput final : public WriterBackend {
            IOOptions options_m;
            const Session& session_m;

        public:
            H5hutOutput(const IOOptions& options, const Session& session)
                : options_m(options)
                , session_m(session) {
                require(options.h5hut.transferMode == H5hutTransferMode::Collective
                            || options.h5hut.transferMode == H5hutTransferMode::Independent,
                        "Invalid H5hut transfer mode");
            }
            void validate(const Snapshot& snapshot) const override {
                require(snapshot.step_m.step <= INT64_MAX, "H5hut step exceeds int64 range");
                for (const auto& particle : snapshot.particles_m) {
                    require(
                        particle.info_m.name.size() <= 128 && particle.info_m.count <= INT64_MAX,
                        "H5hut species name/count too large");
                    for (const auto& [name, info] : particle.info_m.records)
                        (void)nativeType(info.dataType);
                }
                for (const auto& field : snapshot.fields_m) {
                    (void)nativeType(field.info_m.dataType);
                    require(field.info_m.name.size() <= 128, "H5hut field name too long");
                    const auto count = volume(field.owned_m.extent);
                    const auto bytes = 3 * scalarBytes(field.info_m.dataType);
                    require(count <= options_m.stagingBytes / bytes,
                            "H5hut field '" + field.info_m.name + "' needs stagingBytes >= "
                                + (count <= UINT64_MAX / bytes ? std::to_string(count * bytes)
                                                               : std::string("uint64 overflow"))
                                + " for one local component; increase the staging budget");
                }
                (void)metadata(snapshot, options_m.timeUnitSI);
            }
            void open() override {
                collective(session_m, [&] {
                    if (session_m.rank_m)
                        return;
                    writeText(options_m.directory / "ippl-series.txt", "IPPL_IO 1 H5HUT\n");
                    writeText(options_m.directory / "ippl-backend.json",
                              options_m.h5hut.transferMode == H5hutTransferMode::Collective
                                  ? "{\"backend\":\"h5hut\",\"transferMode\":\"collective\"}\n"
                                  : "{\"backend\":\"h5hut\",\"transferMode\":\"independent\"}\n");
                });
            }
            void write(const Snapshot& snapshot) override {
                const auto text = metadata(snapshot, options_m.timeUnitSI);
                const auto step = snapshot.step_m.step;
                for (const auto& particle : snapshot.particles_m) {
                    File file(particlePath(options_m.directory, particle.info_m.name, step), true,
                              session_m, options_m.h5hut.transferMode);
                    file.step(step);
                    writeMetadata(file, snapshot, options_m.timeUnitSI, text);
                    collective(session_m, [&] {
                        check(H5PartSetNumParticles(file.handle_m, particle.local_m),
                              "particle count");
                    });
                    for (const auto& [record, source] : particle.sources_m) {
                        const auto capacity =
                            options_m.stagingBytes / (3 * scalarBytes(source.info_m.dataType));
                        const auto chunks =
                            particle.local_m / capacity + (particle.local_m % capacity != 0);
                        std::uint64_t rounds;
                        MPI_Allreduce(&chunks, &rounds, 1, MPI_UINT64_T, MPI_MAX, session_m.comm_m);
                        rounds = std::max<std::uint64_t>(
                            1, rounds);  // Create datasets for a globally empty species.
                        for (unsigned c = 0; c < source.components_m; ++c) {
                            const auto name = particleName(particle.info_m, record, c);
                            for (std::uint64_t round = 0; round < rounds; ++round) {
                                const auto start =
                                    round < chunks ? round * capacity : particle.local_m;
                                const auto count = std::min(capacity, particle.local_m - start);
                                Buffer data;
                                collective(session_m, [&] {
                                    data = count
                                               ? source.pack_m(c, {{start}, {count}}, ArrayOrder::C)
                                               : buffer(source.info_m.dataType, 1);
                                    setParticleView(file.handle_m, particle.offset_m + start, count,
                                                    particle.info_m.count);
                                });
                                collective(session_m, [&] {
                                    transfer(file.handle_m, name, data, true, false);
                                });
                            }
                        }
                    }
                    file.close();
                }
                for (const auto& field : snapshot.fields_m) {
                    Session active;
                    activeSession(session_m, volume(field.owned_m.extent) != 0, active);
                    collective(session_m, [&] {
                        if (active.comm_m == MPI_COMM_NULL)
                            return;
                        File file(fieldPath(options_m.directory, field.info_m.name, step), true,
                                  active, options_m.h5hut.transferMode);
                        file.step(step);
                        writeMetadata(file, snapshot, options_m.timeUnitSI, text);
                        collective(active, [&] {
                            setFieldView(file.handle_m, field.owned_m);
                        });
                        const Chunk local{
                            std::vector<std::uint64_t>(field.owned_m.extent.size(), 0),
                            field.owned_m.extent};
                        for (unsigned c = 0; c < field.source_m.components_m; ++c) {
                            Buffer data;
                            collective(active, [&] {
                                data =
                                    field.source_m.pack_m(c, local, ArrayOrder::FirstIndexFastest);
                            });
                            const auto name = fieldName(c);
                            collective(active, [&] {
                                transfer(file.handle_m, name, data, true, true);
                                std::array<double, 3> origin{0, 0, 0}, spacing{1, 1, 1};
                                for (std::size_t d = 0; d < field.info_m.extent.size(); ++d) {
                                    spacing[d] = field.info_m.spacing[d];
                                    origin[d] =
                                        field.info_m.gridGlobalOffset[d]
                                        + field.info_m.metadata.samplePosition[d] * spacing[d];
                                }
                                check(H5Block3dSetFieldOrigin(file.handle_m, name.c_str(),
                                                              origin[0], origin[1], origin[2]),
                                      "field origin");
                                check(H5Block3dSetFieldSpacing(file.handle_m, name.c_str(),
                                                               spacing[0], spacing[1], spacing[2]),
                                      "field spacing");
                            });
                        }
                        file.close();
                    });
                }
                // Publish only after all native files have closed successfully on all ranks.
                collective(session_m, [&] {
                    if (session_m.rank_m)
                        return;
                    auto target =
                        options_m.directory / ("step_" + std::to_string(step) + ".complete");
                    auto temporary = target;
                    temporary += ".tmp";
                    writeText(temporary, text);
                    std::filesystem::rename(temporary, target);
                });
            }
            void flush() override {}  // Each write closes its files before returning.
            void close() override {}
        };

        class H5hutInput final : public ReaderBackend {
            std::filesystem::path directory_m;
            const Session& session_m;

        public:
            H5hutInput(std::filesystem::path directory, const Session& session)
                : directory_m(std::move(directory))
                , session_m(session) {
                info_m = {IOBackend::H5hut, {}, 1.0, {}};
            }
            void open() override {
                std::map<std::uint64_t, std::string> completed;
                collective(session_m, [&] {
                    require(readText(directory_m / "ippl-series.txt") == "IPPL_IO 1 H5HUT\n",
                            "Invalid H5hut series descriptor");
                    for (const auto& entry : std::filesystem::directory_iterator(directory_m)) {
                        const auto name = entry.path().filename().string();
                        if (!name.starts_with("step_") || !name.ends_with(".complete"))
                            continue;
                        auto index         = name.substr(5, name.size() - 5 - 9);
                        std::uint64_t step = 0;
                        auto parsed =
                            std::from_chars(index.data(), index.data() + index.size(), step);
                        require(parsed.ec == std::errc{}
                                    && parsed.ptr == index.data() + index.size()
                                    && index == std::to_string(step) && step <= INT64_MAX,
                                "Invalid completed-step filename");
                        completed.emplace(step, readText(entry.path()));
                    }
                });
                auto list = stream();
                for (const auto& [step, text] : completed)
                    list << step << ' ' << std::quoted(text) << ' ';
                agree(session_m, list.str(), "Completed H5hut steps");
                for (const auto& [step, text] : completed) {
                    IterationInfo info;
                    double timeUnit;
                    collective(session_m, [&] {
                        info = parseMetadata(text, timeUnit);
                        require(info.step.step == step, "Completed-step index mismatch");
                        if (iterations_m.empty())
                            info_m.timeUnitSI = timeUnit;
                        require(info_m.timeUnitSI == timeUnit,
                                "Changing time units in H5hut series");
                        for (const auto& particle : info.particles)
                            for (const auto& [name, record] : particle.records)
                                (void)nativeType(record.dataType);
                        for (const auto& field : info.fields)
                            (void)nativeType(field.dataType);
                    });
                    for (const auto& particle : info.particles) {
                        File file(particlePath(directory_m, particle.name, step), false, session_m);
                        file.step(step);
                        verifyMetadata(file, text);
                        collective(session_m, [&] {
                            for (const auto& [record, values] : particle.records) {
                                if (record == "positionOffset")
                                    continue;
                                for (unsigned c = 0; c < componentNames(values.metadata).size();
                                     ++c) {
                                    h5_int64_t type;
                                    h5_size_t count;
                                    check(H5PartGetDatasetInfoByName(
                                              file.handle_m,
                                              particleName(particle, record, c).c_str(), &type,
                                              &count),
                                          "dataset info");
                                    require(type == nativeType(values.dataType)
                                                && count == particle.count,
                                            "H5hut dataset type/count mismatch");
                                }
                            }
                        });
                        file.close();
                    }
                    for (const auto& field : info.fields) {
                        File file(fieldPath(directory_m, field.name, step), false, session_m);
                        file.step(step);
                        verifyMetadata(file, text);
                        collective(session_m, [&] {
                            for (unsigned c = 0; c < componentNames(field.metadata.record).size();
                                 ++c) {
                                h5_size_t rank, dims[3], elementRank;
                                h5_int64_t type;
                                check(H5BlockGetFieldInfoByName(file.handle_m, fieldName(c).c_str(),
                                                                &rank, dims, &elementRank, &type),
                                      "field info");
                                require(rank == 3 && elementRank == 1
                                            && type == nativeType(field.dataType),
                                        "H5hut field type/rank mismatch");
                                for (unsigned d = 0; d < 3; ++d)
                                    require(
                                        dims[d] == (d < field.extent.size() ? field.extent[d] : 1),
                                        "H5hut field extent mismatch");
                            }
                        });
                        file.close();
                    }
                    iterations_m.emplace(step, std::move(info));
                    info_m.iterations.push_back(step);
                }
            }
            Buffer read(std::uint64_t step, std::string_view species, std::string_view record,
                        std::string_view component, const Chunk& region, DataType type,
                        bool field) override {
                Buffer result;
                const auto count = volume(region.extent);
                collective(session_m, [&] {
                    result = buffer(type, std::max<std::uint64_t>(1, count));
                });
                const auto& info = iterations_m.at(step);
                if (!field) {
                    const auto& particle = *std::find_if(info.particles.begin(),
                                                         info.particles.end(), [&](const auto& p) {
                                                             return p.name == species;
                                                         });
                    const auto names =
                        componentNames(particle.records.at(std::string(record)).metadata);
                    auto c = static_cast<unsigned>(std::find(names.begin(), names.end(), component)
                                                   - names.begin());
                    if (record != "positionOffset" && particle.count) {
                        File file(particlePath(directory_m, particle.name, step), false, session_m);
                        file.step(step);
                        collective(session_m, [&] {
                            setParticleView(file.handle_m, region.offset[0], count, particle.count);
                        });
                        collective(session_m, [&] {
                            transfer(file.handle_m, particleName(particle, std::string(record), c),
                                     result, false, false);
                        });
                        file.close();
                    }
                } else {
                    const auto& item =
                        *std::find_if(info.fields.begin(), info.fields.end(), [&](const auto& f) {
                            return f.name == record;
                        });
                    const auto names = componentNames(item.metadata.record);
                    auto c = static_cast<unsigned>(std::find(names.begin(), names.end(), component)
                                                   - names.begin());
                    Session active;
                    activeSession(session_m, count != 0, active);
                    collective(session_m, [&] {
                        if (active.comm_m == MPI_COMM_NULL)
                            return;
                        File file(fieldPath(directory_m, item.name, step), false, active);
                        file.step(step);
                        collective(active, [&] {
                            setFieldView(file.handle_m, region);
                        });
                        collective(active, [&] {
                            transfer(file.handle_m, fieldName(c), result, false, true);
                        });
                        file.close();
                    });
                    // Public readers retain last-index-fastest ordering for both backends.
                    collective(session_m, [&] {
                        std::visit(
                            [&](auto& values) {
                                auto ordered = values;
                                for (std::uint64_t i = 0; i < count; ++i) {
                                    auto remainder       = i;
                                    std::uint64_t source = 0;
                                    for (std::size_t d = region.extent.size(); d-- > 0;) {
                                        source = source * region.extent[d]
                                                 + remainder % region.extent[d];
                                        remainder /= region.extent[d];
                                    }
                                    ordered[i] = values[source];
                                }
                                values = std::move(ordered);
                            },
                            result);
                    });
                }
                if (!count)
                    std::visit(
                        [](auto& values) {
                            values.clear();
                        },
                        result);
                return result;
            }
            void close() override {}
        };
    }  // namespace
    std::unique_ptr<WriterBackend> h5hutWriter(IOOptions& options, const Session& session) {
        return std::make_unique<H5hutOutput>(options, session);
    }
    std::unique_ptr<ReaderBackend> h5hutReader(const std::filesystem::path& directory,
                                               const Session& session) {
        return std::make_unique<H5hutInput>(directory, session);
    }
}  // namespace ippl::detail::io
