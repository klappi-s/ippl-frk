#ifndef IPPL_IO_METADATA_H
#define IPPL_IO_METADATA_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ippl {
    enum class IOBackend {
        OpenPMD,
        H5hut
    };
    enum class OpenPMDBackend {
        HDF5,
        BP5
    };
    enum class H5hutTransferMode {
        Collective,
        Independent
    };
    struct OpenPMDSettings {
        OpenPMDBackend backend    = OpenPMDBackend::HDF5;
        std::string backendConfig = "{}";
    };
    struct H5hutSettings {
        H5hutTransferMode transferMode = H5hutTransferMode::Collective;
    };
    struct IOOptions {
        std::filesystem::path directory;
        IOBackend backend        = IOBackend::OpenPMD;
        double timeUnitSI        = 1.0;
        std::size_t stagingBytes = 64 * 1024 * 1024;
        OpenPMDSettings openPMD;
        H5hutSettings h5hut;
    };
    struct StepInfo {
        std::uint64_t step;
        double time;
        double dt;
    };
    struct Units {
        double unitSI;
        // SI exponents: length, mass, time, current, temperature, amount, luminosity.
        std::array<double, 7> dimension;
    };
    struct TimeOffset {
        double constant         = 0.0;
        double timestepFraction = 0.0;
    };
    struct RecordMetadata {
        Units units;
        std::vector<std::string> components;
        TimeOffset timeOffset = {};
    };
    struct PositionMetadata {
        double lengthUnitSI;
        TimeOffset timeOffset = {};
    };
    struct FieldMetadata {
        RecordMetadata record;
        double gridUnitSI;
        std::vector<std::string> axisLabels;
        std::vector<double> samplePosition;
    };
    enum class DataType {
        INT8,
        UINT8,
        INT16,
        UINT16,
        INT32,
        UINT32,
        INT64,
        UINT64,
        FLOAT32,
        FLOAT64
    };
    struct Chunk {
        std::vector<std::uint64_t> offset;
        std::vector<std::uint64_t> extent;
    };
    struct RecordInfo {
        DataType dataType;
        RecordMetadata metadata;
    };
    struct ParticleInfo {
        std::string name;
        std::uint64_t count;
        std::map<std::string, RecordInfo> records;
    };
    struct FieldInfo {
        std::string name;
        DataType dataType;
        FieldMetadata metadata;
        std::vector<std::int64_t> indexBegin;
        std::vector<std::uint64_t> extent;
        std::vector<double> meshOrigin;
        std::vector<double> spacing;
        std::vector<double> gridGlobalOffset;
    };
    struct IterationInfo {
        StepInfo step;
        std::vector<ParticleInfo> particles;
        std::vector<FieldInfo> fields;
    };
    struct IOSeriesInfo {
        IOBackend backend;
        std::optional<OpenPMDBackend> openPMDBackend;
        double timeUnitSI;
        std::vector<std::uint64_t> iterations;
    };
}  // namespace ippl
#endif
