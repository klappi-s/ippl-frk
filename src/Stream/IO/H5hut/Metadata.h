#ifndef IPPL_IO_H5HUT_METADATA_H
#define IPPL_IO_H5HUT_METADATA_H
#include "Stream/IO/detail/Backend.h"
namespace ippl::detail::io::h5hut {
    // Versioned, lossless metadata text stored both in native files and completion records.
    // Decimal max_digits10 preserves binary floating values without a JSON dependency.
    inline constexpr std::size_t MAX_METADATA_BYTES = 1024 * 1024;
    inline std::string metadata(const Snapshot& snapshot, double timeUnitSI) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(std::numeric_limits<double>::max_digits10);
        out << "IPPL_H5HUT 1 " << timeUnitSI << ' ' << snapshot.step_m.step << ' '
            << snapshot.step_m.time << ' ' << snapshot.step_m.dt << ' '
            << snapshot.particles_m.size() << ' ' << snapshot.fields_m.size() << ' ';
        auto evaluated = [&](RecordInfo info) {
            info.metadata.timeOffset = {
                info.metadata.timeOffset.constant
                    + info.metadata.timeOffset.timestepFraction * snapshot.step_m.dt,
                0};
            return info;
        };
        for (const auto& data : snapshot.particles_m) {
            out << std::quoted(data.info_m.name) << ' ' << data.info_m.count << ' '
                << data.info_m.records.size() << ' ';
            for (const auto& [name, info] : data.info_m.records) {
                out << std::quoted(name) << ' ';
                signature(out, evaluated(info));
            }
        }
        for (const auto& data : snapshot.fields_m) {
            auto info            = data.info_m;
            info.metadata.record = evaluated({info.dataType, info.metadata.record}).metadata;
            signature(out, info);
        }
        auto result = out.str();
        require(result.size() < MAX_METADATA_BYTES, "H5hut metadata exceeds 1 MiB");
        return result;
    }
    inline RecordInfo readRecord(std::istream& in) {
        RecordInfo info{};
        int type;
        std::size_t components;
        in >> type >> info.metadata.units.unitSI;
        require(bool(in) && type >= 0 && type <= static_cast<int>(DataType::FLOAT64),
                "Invalid H5hut record type");
        info.dataType = static_cast<DataType>(type);
        for (auto& value : info.metadata.units.dimension)
            in >> value;
        in >> info.metadata.timeOffset.constant >> info.metadata.timeOffset.timestepFraction
            >> components;
        require(bool(in) && components <= 3, "Invalid H5hut components");
        info.metadata.components.resize(components);
        for (auto& name : info.metadata.components)
            in >> std::quoted(name);
        require(bool(in), "Truncated H5hut record");
        RecordSource source{info,
                            static_cast<unsigned>(components ? components : 1),
                            components != 0,
                            nullptr,
                            {},
                            {},
                            {}};
        validateRecord(source, "record");
        return info;
    }
    inline IterationInfo parseMetadata(const std::string& text, double& timeUnitSI) {
        require(text.size() < MAX_METADATA_BYTES, "H5hut metadata exceeds 1 MiB");
        std::istringstream in(text);
        in.imbue(std::locale::classic());
        IterationInfo result{};
        std::string header;
        unsigned version;
        std::size_t particles, fields;
        in >> header >> version >> timeUnitSI >> result.step.step >> result.step.time
            >> result.step.dt >> particles >> fields;
        require(bool(in) && header == "IPPL_H5HUT" && version == 1 && particles <= 4096
                    && fields <= 4096 && std::isfinite(timeUnitSI) && timeUnitSI > 0
                    && std::isfinite(result.step.time) && std::isfinite(result.step.dt)
                    && result.step.dt >= 0,
                "Invalid H5hut metadata header");
        std::set<std::string> speciesNames, fieldNames;
        for (std::size_t i = 0; i < particles; ++i) {
            ParticleInfo info{};
            std::size_t records;
            in >> std::quoted(info.name) >> info.count >> records;
            require(bool(in) && validName(info.name) && speciesNames.insert(info.name).second
                        && records <= 4096 && info.count <= INT64_MAX,
                    "Invalid H5hut particle metadata");
            for (std::size_t j = 0; j < records; ++j) {
                std::string name;
                in >> std::quoted(name);
                require(validName(name) && info.records.emplace(name, readRecord(in)).second,
                        "Invalid/duplicate H5hut particle record");
            }
            require(info.records.contains("position") && !info.records.contains("positionOffset"),
                    "Invalid H5hut position metadata");
            const auto& position = info.records.at("position");
            require(!position.metadata.components.empty(), "Missing position components");
            info.records.emplace("positionOffset", position);
            result.particles.push_back(std::move(info));
        }
        for (std::size_t i = 0; i < fields; ++i) {
            FieldInfo info{};
            std::size_t dim;
            in >> std::quoted(info.name);
            require(validName(info.name) && fieldNames.insert(info.name).second,
                    "Invalid H5hut field name");
            auto record          = readRecord(in);
            info.dataType        = record.dataType;
            info.metadata.record = record.metadata;
            in >> info.metadata.gridUnitSI >> dim;
            require(bool(in) && dim >= 1 && dim <= 3 && std::isfinite(info.metadata.gridUnitSI)
                        && info.metadata.gridUnitSI > 0,
                    "Invalid H5hut grid metadata");
            info.indexBegin.resize(dim);
            info.extent.resize(dim);
            info.meshOrigin.resize(dim);
            info.spacing.resize(dim);
            info.gridGlobalOffset.resize(dim);
            info.metadata.axisLabels.resize(dim);
            info.metadata.samplePosition.resize(dim);
            std::set<std::string> axes;
            for (std::size_t d = 0; d < dim; ++d) {
                in >> info.indexBegin[d] >> info.extent[d] >> info.meshOrigin[d] >> info.spacing[d]
                    >> std::quoted(info.metadata.axisLabels[d]) >> info.metadata.samplePosition[d];
                info.gridGlobalOffset[d] =
                    info.meshOrigin[d] + info.indexBegin[d] * info.spacing[d];
                require(bool(in) && info.extent[d] > 0 && info.extent[d] <= INT_MAX
                            && std::isfinite(info.meshOrigin[d])
                            && std::isfinite(info.gridGlobalOffset[d])
                            && std::isfinite(info.spacing[d]) && info.spacing[d] > 0
                            && std::isfinite(info.metadata.samplePosition[d])
                            && info.metadata.samplePosition[d] >= 0
                            && info.metadata.samplePosition[d] < 1
                            && validName(info.metadata.axisLabels[d])
                            && axes.insert(info.metadata.axisLabels[d]).second,
                        "Invalid H5hut field geometry");
            }
            (void)volume(info.extent);
            result.fields.push_back(std::move(info));
        }
        in >> std::ws;
        require(in.eof(), "Trailing H5hut metadata");
        require(!result.particles.empty() || !result.fields.empty(), "Empty H5hut schema");
        return result;
    }
}  // namespace ippl::detail::io::h5hut
#endif
