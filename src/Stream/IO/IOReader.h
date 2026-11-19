#ifndef IPPL_IO_READER_H
#define IPPL_IO_READER_H
#if !defined(IPPL_ENABLE_OPENPMD) && !defined(IPPL_ENABLE_H5HUT)
#error "Enable openPMD or H5hut in IPPL to use IOReader"
#endif

#include <memory>
#include <mpi.h>
#include <string_view>

#include "Stream/IO/detail/Sources.h"

namespace ippl {
    /**
     * Read IPPL file output into owned host arrays. open/read/close are collective;
     * info/inspect use cached metadata and can be called locally after open(). All ranks
     * read the same component but may request different regions, including empty ones.
     */
    class IOReader {
    public:
        IOReader(std::filesystem::path directory, MPI_Comm communicator);
        ~IOReader();
        IOReader(const IOReader&)            = delete;
        IOReader& operator=(const IOReader&) = delete;
        void open();
        IOSeriesInfo info() const;
        IterationInfo inspect(std::uint64_t step) const;
        template <class T>
        std::vector<T> readParticleComponent(std::uint64_t step, std::string_view species,
                                             std::string_view record, std::string_view component,
                                             std::uint64_t offset, std::uint64_t count) {
            return std::get<std::vector<T>>(read(step, species, record, component,
                                                 {{offset}, {count}}, detail::io::dataType<T>(),
                                                 false));
        }
        template <class T>
        std::vector<T> readFieldComponent(std::uint64_t step, std::string_view record,
                                          std::string_view component, const Chunk& region) {
            return std::get<std::vector<T>>(
                read(step, {}, record, component, region, detail::io::dataType<T>(), true));
        }
        void close();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_m;
        detail::io::Buffer read(std::uint64_t step, std::string_view species,
                                std::string_view record, std::string_view component,
                                const Chunk& region, DataType type, bool field);
    };
}  // namespace ippl
#endif
