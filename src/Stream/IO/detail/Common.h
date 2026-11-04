#ifndef IPPL_IO_COMMON_H
#define IPPL_IO_COMMON_H
#include <algorithm>
#include <climits>
#include <cmath>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>

#include "Stream/IO/detail/Sources.h"
namespace ippl::detail::io {
    inline constexpr const char* SCALAR_COMPONENT = "\vScalar";
    struct Session {
        Session()                          = default;
        Session(const Session&)            = delete;
        Session& operator=(const Session&) = delete;
        MPI_Comm comm_m                    = MPI_COMM_NULL;
        int rank_m                         = 0;
        int size_m                         = 1;
        void start(MPI_Comm communicator) {
            require(communicator != MPI_COMM_NULL, "A valid simulation communicator is required");
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
        throw IpplException("I/O", "Rank " + std::to_string(first) + ": " + error);
    }
    inline void agree(const Session& session, const std::string& value,
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
    inline std::ostringstream stream() {
        std::ostringstream result;
        result.imbue(std::locale::classic());
        result << std::hexfloat;
        return result;
    }
    inline bool validName(const std::string& name) {
        return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                   || c == '_';
        });
    }
    inline bool reserved(const std::string& name) {
        return name == "position" || name == "positionOffset" || name == "ippl_id"
               || name == "ippl_rank" || name == "id";
    }
    inline Buffer buffer(DataType type, std::size_t count = 0) {
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
        throw IpplException("I/O", "Unsupported scalar type");
    }
    inline std::size_t scalarBytes(DataType type) {
        return std::visit(
            [](const auto& values) {
                return sizeof(typename std::decay_t<decltype(values)>::value_type);
            },
            buffer(type));
    }
    inline void validateRecord(const RecordSource& source, const std::string& name) {
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
    inline void signature(std::ostream& out, const RecordInfo& info) {
        out << static_cast<int>(info.dataType) << ' ' << info.metadata.units.unitSI << ' ';
        for (auto value : info.metadata.units.dimension)
            out << value << ' ';
        out << info.metadata.timeOffset.constant << ' ' << info.metadata.timeOffset.timestepFraction
            << ' ';
        out << info.metadata.components.size() << ' ';
        for (const auto& name : info.metadata.components)
            out << std::quoted(name) << ' ';
    }
    inline void signature(std::ostream& out, const FieldInfo& info) {
        out << std::quoted(info.name) << ' ';
        signature(out, {info.dataType, info.metadata.record});
        out << info.metadata.gridUnitSI << ' ' << info.extent.size() << ' ';
        for (std::size_t d = 0; d < info.extent.size(); ++d)
            out << info.indexBegin[d] << ' ' << info.extent[d] << ' ' << info.meshOrigin[d] << ' '
                << info.spacing[d] << ' ' << std::quoted(info.metadata.axisLabels[d]) << ' '
                << info.metadata.samplePosition[d] << ' ';
    }
    inline std::vector<std::string> componentNames(const RecordMetadata& metadata) {
        return metadata.components.empty() ? std::vector<std::string>{SCALAR_COMPONENT}
                                           : metadata.components;
    }
    inline void validateRegion(const Chunk& region, const std::vector<std::uint64_t>& shape) {
        require(region.offset.size() == shape.size() && region.extent.size() == shape.size(),
                "Chunk dimensionality mismatch");
        for (std::size_t d = 0; d < shape.size(); ++d)
            require(region.offset[d] <= shape[d] && region.extent[d] <= shape[d] - region.offset[d],
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

}  // namespace ippl::detail::io
#endif
