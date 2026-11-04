#ifndef IPPL_IO_SOURCES_H
#define IPPL_IO_SOURCES_H

#include <functional>
#include <limits>
#include <mpi.h>
#include <type_traits>
#include <variant>

#include "Types/Vector.h"
#include "Types/VectorComponent.h"

#include "Utility/IpplException.h"
#include "Utility/ParallelDispatch.h"

#include "Expression/IpplOperations.h"

#include "Stream/IO/Metadata.h"

namespace ippl::detail::io {
    enum class ArrayOrder {
        C,
        FirstIndexFastest
    };

    using Buffer =
        std::variant<std::vector<std::int8_t>, std::vector<std::uint8_t>, std::vector<std::int16_t>,
                     std::vector<std::uint16_t>, std::vector<std::int32_t>,
                     std::vector<std::uint32_t>, std::vector<std::int64_t>,
                     std::vector<std::uint64_t>, std::vector<float>, std::vector<double>>;

    template <class T>
    constexpr DataType dataType() {
        if constexpr (std::is_same_v<T, std::int8_t>)
            return DataType::INT8;
        else if constexpr (std::is_same_v<T, std::uint8_t>)
            return DataType::UINT8;
        else if constexpr (std::is_same_v<T, std::int16_t>)
            return DataType::INT16;
        else if constexpr (std::is_same_v<T, std::uint16_t>)
            return DataType::UINT16;
        else if constexpr (std::is_same_v<T, std::int32_t>)
            return DataType::INT32;
        else if constexpr (std::is_same_v<T, std::uint32_t>)
            return DataType::UINT32;
        else if constexpr (std::is_same_v<T, std::int64_t>)
            return DataType::INT64;
        else if constexpr (std::is_same_v<T, std::uint64_t>)
            return DataType::UINT64;
        else if constexpr (std::is_same_v<T, float>)
            return DataType::FLOAT32;
        else if constexpr (std::is_same_v<T, double>)
            return DataType::FLOAT64;
        else
            static_assert(!sizeof(T), "I/O supports float, double and fixed-width integers");
    }

    template <class T>
    struct Components {
        using scalar_type               = T;
        static constexpr unsigned COUNT = 1;
        static constexpr bool VECTOR    = false;
        KOKKOS_INLINE_FUNCTION static T get(const T& value, unsigned) { return value; }
    };
    template <class T, unsigned Dim>
    struct Components<ippl::Vector<T, Dim>> {
        using scalar_type               = T;
        static constexpr unsigned COUNT = Dim;
        static constexpr bool VECTOR    = true;
        KOKKOS_INLINE_FUNCTION static T get(const ippl::Vector<T, Dim>& value, unsigned component) {
            return value[component];
        }
    };

    // Resolve storage on the host; device kernels access one scalar component.
    // SoA returns a native scalar view, while AoS retains the vector view owner.
    template <class View>
    auto scalarComponent(const View& view, unsigned component) {
        if constexpr (ippl::is_vector_v<typename View::non_const_value_type>)
            return ippl::componentView(view, component);
        else
            return view;
    }

    inline void require(bool condition, const std::string& message) {
        if (!condition)
            throw IpplException("I/O", message);
    }
    std::uint64_t volume(const std::vector<std::uint64_t>& extent);

    struct RecordSource {
        RecordInfo info_m;
        unsigned components_m;
        bool vector_m;
        const void* identity_m = nullptr;
        std::function<std::string()> name_m;
        std::function<std::uint64_t()> size_m;
        std::function<Buffer(unsigned, const Chunk&, ArrayOrder)> pack_m;
    };
    struct FieldSource {
        RecordSource values_m;
        std::function<FieldInfo()> info_m;
        std::function<Chunk()> owned_m;
        std::function<MPI_Comm()> communicator_m;
    };

    template <class View>
    Buffer hostBuffer(const View& packed) {
        using Scalar    = typename View::non_const_value_type;
        const auto host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), packed);
        std::vector<Scalar> result(host.extent(0));
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = host(i);
        return result;
    }

    template <class Attribute>
    RecordSource attributeSource(const Attribute& attribute, RecordMetadata metadata) {
        using Value  = typename Attribute::value_type;
        using Traits = Components<Value>;
        using Scalar = typename Traits::scalar_type;
        using Exec   = typename Attribute::view_type::execution_space;
        static_assert(Traits::COUNT >= 1 && Traits::COUNT <= 3);
        return {{dataType<Scalar>(), std::move(metadata)},
                Traits::COUNT,
                Traits::VECTOR,
                static_cast<const void*>(&attribute),
                [&attribute] {
                    return attribute.get_name();
                },
                [&attribute] {
                    return attribute.getView().extent(0);
                },
                [&attribute](unsigned component, const Chunk& chunk, ArrayOrder order) -> Buffer {
                    (void)order;
                    const auto view  = scalarComponent(attribute.getView(), component);
                    const auto start = chunk.offset[0];
                    const auto count = chunk.extent[0];
                    Kokkos::View<Scalar*, typename Exec::memory_space> packed("I/O particle",
                                                                              count);
                    Kokkos::parallel_for(
                        "I/O pack particle",
                        Kokkos::RangePolicy<Exec, Kokkos::IndexType<std::uint64_t>>(0, count),
                        KOKKOS_LAMBDA(std::uint64_t i) {
                            packed(i) = view(start + i);
                        });
                    Exec().fence("I/O particle staging");
                    return hostBuffer(packed);
                }};
    }

    template <class Field>
    FieldSource fieldSource(std::string name, const Field& field, FieldMetadata metadata) {
        using Traits           = Components<typename Field::value_type>;
        using Scalar           = typename Traits::scalar_type;
        using Exec             = typename Field::execution_space;
        constexpr unsigned DIM = Field::dim;
        static_assert(DIM >= 1 && DIM <= 3 && Traits::COUNT <= 3);
        FieldSource result;
        result.values_m.info_m       = {dataType<Scalar>(), metadata.record};
        result.values_m.components_m = Traits::COUNT;
        result.values_m.vector_m     = Traits::VECTOR;
        result.info_m                = [name, &field, metadata] {
            FieldInfo info{name, dataType<Scalar>(), metadata, {}, {}, {}, {}, {}};
            const auto& domain = field.getDomain();
            const auto& mesh   = field.get_mesh();
            for (unsigned d = 0; d < DIM; ++d) {
                require(domain[d].stride() == 1, name + ": non-unit index stride");
                info.indexBegin.push_back(domain[d].first());
                info.extent.push_back(domain[d].length());
                info.meshOrigin.push_back(mesh.getOrigin()[d]);
                info.spacing.push_back(mesh.getMeshSpacing(d));
                info.gridGlobalOffset.push_back(mesh.getOrigin()[d]
                                                + domain[d].first() * mesh.getMeshSpacing(d));
            }
            return info;
        };
        result.owned_m = [&field] {
            Chunk chunk;
            for (unsigned d = 0; d < DIM; ++d) {
                const auto& owned  = field.getOwned()[d];
                const auto& global = field.getDomain()[d];
                require(owned.first() >= global.first(),
                        "Field owned region precedes global domain");
                chunk.offset.push_back(owned.first() - global.first());
                chunk.extent.push_back(owned.length());
            }
            return chunk;
        };
        result.communicator_m = [&field] {
            return field.getCommunicator().getCommunicator();
        };
        result.values_m.pack_m = [&field](unsigned component, const Chunk& chunk,
                                          ArrayOrder order) -> Buffer {
            const auto view = scalarComponent(field.getView(), component);
            using Index     = typename ippl::RangePolicy<DIM, Exec>::index_type;
            using Coords    = typename ippl::RangePolicy<DIM, Exec>::index_array_type;
            Kokkos::Array<Index, DIM> begin, end;
            for (unsigned d = 0; d < DIM; ++d) {
                begin[d] = field.getNghost() + chunk.offset[d];
                end[d]   = begin[d] + chunk.extent[d];
            }
            Kokkos::View<Scalar*, typename Exec::memory_space> packed("I/O field",
                                                                      volume(chunk.extent));
            // Pack directly in the backend file order using the source execution space.
            ippl::parallel_for(
                "I/O pack field", ippl::createRangePolicy<DIM, Exec>(begin, end),
                KOKKOS_LAMBDA(const Coords& index) {
                    std::uint64_t linear = 0;
                    for (unsigned axis = 0; axis < DIM; ++axis) {
                        const unsigned d = order == ArrayOrder::C ? axis : DIM - 1 - axis;
                        linear           = linear * (end[d] - begin[d]) + index[d] - begin[d];
                    }
                    packed(linear) = ippl::apply(view, index);
                });
            Exec().fence("I/O field staging");
            return hostBuffer(packed);
        };
        return result;
    }
}  // namespace ippl::detail::io
#endif
