#ifndef IPPL_IO_WRITER_H
#define IPPL_IO_WRITER_H
#if !defined(IPPL_ENABLE_OPENPMD) && !defined(IPPL_ENABLE_H5HUT)
#error "Enable openPMD or H5hut in IPPL to use IOWriter"
#endif

#include <memory>
#include <mpi.h>
#include <string_view>

#include "Stream/IO/detail/Sources.h"
#include "Stream/ParticleAttributeSelection.h"

namespace ippl {
    class IOWriter;

    /** Typed, borrowed attributes. The owning writer fixes registration at open(). */
    class ParticleOutputBinding {
    public:
        ParticleOutputBinding(const ParticleOutputBinding&)            = delete;
        ParticleOutputBinding& operator=(const ParticleOutputBinding&) = delete;
        template <class Attribute>
        void bind(const Attribute& attribute, RecordMetadata metadata) {
            addAttribute(attribute.get_name(),
                         detail::io::attributeSource(attribute, std::move(metadata)));
        }

    private:
        friend class IOWriter;
        ParticleOutputBinding() = default;
        void addAttribute(std::string name, detail::io::RecordSource source);
        bool frozen_m = false;
        ParticleAttributeSelection selection_m;
        std::function<std::vector<std::pair<const void*, std::string>>()> attributes_m;
        std::function<std::uint64_t()> count_m;
        std::map<std::string, detail::io::RecordSource> records_m;
    };

    /**
     * Synchronous output on the supplied communicator. Bindings borrow their sources;
     * keep those objects alive at stable addresses until close(). Reallocation is allowed.
     * All ranks call open/write/flush/close in the same order, before MPI finalization.
     */
    class IOWriter {
    public:
        IOWriter(IOOptions options, MPI_Comm communicator);
        ~IOWriter();
        IOWriter(const IOWriter&)            = delete;
        IOWriter& operator=(const IOWriter&) = delete;

        template <class ParticleContainer>
        ParticleOutputBinding& addParticles(std::string speciesName,
                                            const ParticleContainer& particles,
                                            PositionMetadata positions) {
            using Vector = typename ParticleContainer::vector_type;
            static_assert(Vector::dim >= 1 && Vector::dim <= 3);
            auto binding     = std::unique_ptr<ParticleOutputBinding>(new ParticleOutputBinding);
            binding->count_m = [&particles] {
                return particles.getLocalNum();
            };
            binding->attributes_m = [&particles] {
                std::vector<std::pair<const void*, std::string>> result;
                particles.forAllAttributes([&](const auto& attributes) {
                    for (const auto* attribute : attributes)
                        if (!particles.isBuiltinAttribute(attribute))
                            result.emplace_back(dynamic_cast<const void*>(attribute),
                                                attribute->get_name());
                });
                return result;
            };
            RecordMetadata metadata{
                {positions.lengthUnitSI, {1, 0, 0, 0, 0, 0, 0}}, {}, positions.timeOffset};
            const std::array<std::string, 3> axes{"x", "y", "z"};
            metadata.components.assign(axes.begin(), axes.begin() + Vector::dim);
            binding->records_m.emplace("position",
                                       detail::io::attributeSource(particles.R, metadata));
            if constexpr (ParticleContainer::EnableIDs)
                binding->records_m.emplace(
                    "ippl_id",
                    detail::io::attributeSource(particles.ID, RecordMetadata{{1.0, {}}, {}}));
            return addParticleSource(std::move(speciesName), std::move(binding));
        }
        template <class Field>
        void addField(std::string recordName, const Field& field, FieldMetadata metadata) {
            auto source = detail::io::fieldSource(recordName, field, std::move(metadata));
            addFieldSource(std::move(recordName), std::move(source));
        }
        void setParticleAttributeSelection(
            std::string_view speciesName,
            ParticleAttributeSelection selection = ParticleAttributeSelection::all());
        void open();  // Freeze registration and create a new directory; never overwrite.
        void write(const StepInfo& step);  // Source memory can be reused when this returns.
        void flush();
        void close();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_m;
        ParticleOutputBinding& addParticleSource(std::string name,
                                                 std::unique_ptr<ParticleOutputBinding> binding);
        void addFieldSource(std::string name, detail::io::FieldSource source);
    };
}  // namespace ippl
#endif
