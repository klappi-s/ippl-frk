#ifndef IPPL_IO_BACKEND_H
#define IPPL_IO_BACKEND_H
#include <memory>

#include "Stream/IO/detail/Common.h"
namespace ippl::detail::io {
    struct ParticleData {
        ParticleInfo info_m;
        std::uint64_t local_m, offset_m;
        std::map<std::string, RecordSource> sources_m;
    };
    struct FieldData {
        FieldInfo info_m;
        Chunk owned_m;
        RecordSource source_m;
    };
    struct Snapshot {
        StepInfo step_m;
        std::vector<ParticleData> particles_m;
        std::vector<FieldData> fields_m;
    };
    class WriterBackend {
    public:
        virtual ~WriterBackend()                     = default;
        virtual void validate(const Snapshot&) const = 0;
        virtual void open()                          = 0;
        virtual void write(const Snapshot&)          = 0;
        virtual void flush()                         = 0;
        virtual void close()                         = 0;
    };
    class ReaderBackend {
    public:
        IOSeriesInfo info_m;
        std::map<std::uint64_t, IterationInfo> iterations_m;
        virtual ~ReaderBackend() = default;
        virtual void open()      = 0;
        virtual Buffer read(std::uint64_t step, std::string_view species, std::string_view record,
                            std::string_view component, const Chunk&, DataType, bool field) = 0;
        virtual void close()                                                                = 0;
    };
#ifdef IPPL_ENABLE_OPENPMD
    std::unique_ptr<WriterBackend> openPMDWriter(IOOptions&, const Session&);
    std::unique_ptr<ReaderBackend> openPMDReader(const std::filesystem::path&, const Session&);
#endif
#ifdef IPPL_ENABLE_H5HUT
    std::unique_ptr<WriterBackend> h5hutWriter(IOOptions&, const Session&);
    std::unique_ptr<ReaderBackend> h5hutReader(const std::filesystem::path&, const Session&);
#endif
}  // namespace ippl::detail::io
#endif
