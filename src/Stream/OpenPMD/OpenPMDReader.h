#ifndef IPPL_OPENPMD_READER_H
#define IPPL_OPENPMD_READER_H
#ifndef IPPL_ENABLE_OPENPMD
#error "Configure IPPL with IPPL_ENABLE_OPENPMD=ON to use OpenPMDReader"
#endif

#include <memory>
#include <mpi.h>
#include <string_view>

#include "Stream/OpenPMD/Storage.h"

namespace ippl {
    /**
     * Read IPPL openPMD output into owned host arrays. open/read/close are collective;
     * info/inspect use cached metadata and can be called locally after open(). All ranks
     * read the same component but may request different regions, including empty ones.
     */
    class OpenPMDReader {
    public:
        OpenPMDReader(std::filesystem::path directory, MPI_Comm communicator);
        ~OpenPMDReader();
        OpenPMDReader(const OpenPMDReader&)            = delete;
        OpenPMDReader& operator=(const OpenPMDReader&) = delete;
        void open();
        SeriesInfo info() const;
        IterationInfo inspect(std::uint64_t step) const;
        template <class T>
        std::vector<T> readParticleComponent(std::uint64_t step, std::string_view species,
                                             std::string_view record, std::string_view component,
                                             std::uint64_t offset, std::uint64_t count) {
            return std::get<std::vector<T>>(read(step, species, record, component,
                                                 {{offset}, {count}},
                                                 detail::openpmd::dataType<T>(), false));
        }
        template <class T>
        std::vector<T> readFieldComponent(std::uint64_t step, std::string_view record,
                                          std::string_view component, const Chunk& region) {
            return std::get<std::vector<T>>(
                read(step, {}, record, component, region, detail::openpmd::dataType<T>(), true));
        }
        void close();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_m;
        detail::openpmd::Buffer read(std::uint64_t step, std::string_view species,
                                     std::string_view record, std::string_view component,
                                     const Chunk& region, DataType type, bool field);
    };
}  // namespace ippl
#endif
