// Shared device access to the entity fields of a FEMContainer.
#ifndef IPPL_FEM_FIELD_ACCESS_H
#define IPPL_FEM_FIELD_ACCESS_H

#include <Kokkos_Core.hpp>

#include <tuple>
#include <type_traits>
#include <utility>

#include "Expression/IpplOperations.h"
#include "Types/Tuple.h"

namespace ippl::detail {

    // Keep the established transfer helper API, with owning device-callable tuples.
    template <typename Container, typename EntityTypes>
    auto make_entity_view_tuple(Container& field) {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            return [](auto... views) {
                return ippl::makeTuple(std::move(views)...);
            }(field.template getView<std::tuple_element_t<Is, EntityTypes>>()...);
        }(std::make_index_sequence<std::tuple_size_v<EntityTypes>>{});
    }

    template <typename Container, typename EntityTypes>
    auto make_entity_view_tuple(const Container& field) {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            return [](auto... views) {
                return ippl::makeTuple(std::move(views)...);
            }(field.template getView<std::tuple_element_t<Is, EntityTypes>>()...);
        }(std::make_index_sequence<std::tuple_size_v<EntityTypes>>{});
    }

    // All entity views store the same scalar T, although their DOFArray sizes differ.
    // Select the mapped scalar without a device std::tuple or a generic device lambda.
    template <typename T, std::size_t I = 0, typename ViewTuple,
              typename Indices, typename Mapping>
    KOKKOS_INLINE_FUNCTION T* mappedDOFPointer(const ViewTuple& views,
                                              const Indices& elementNDIndex,
                                              const Mapping& map) {
        if constexpr (I < std::tuple_size_v<ViewTuple>) {
            if (I == map.entityTypeIndex)
                return &apply(ippl::get<I>(views), elementNDIndex + map.entityLocalIndex)
                             [map.entityLocalDOF];
            return mappedDOFPointer<T, I + 1>(views, elementNDIndex, map);
        } else {
            return nullptr;
        }
    }

    template <typename T, typename ViewTuple, typename Indices, typename Mapping>
    KOKKOS_INLINE_FUNCTION T gather_from_views(const ViewTuple& views,
                                               const Indices& elementNDIndex,
                                               const Mapping& map) {
        const T* value = mappedDOFPointer<T>(views, elementNDIndex, map);
        return value ? *value : T(0);
    }

    template <typename ViewTuple, typename Indices, typename Mapping, typename T>
    KOKKOS_INLINE_FUNCTION void scatter_to_views(ViewTuple& views,
                                                 const Indices& elementNDIndex,
                                                 const Mapping& map, const T value) {
        if (T* destination = mappedDOFPointer<T>(views, elementNDIndex, map))
            Kokkos::atomic_store(destination, value);
    }

    template <typename ViewTuple, typename Indices, typename Mapping, typename T>
    KOKKOS_INLINE_FUNCTION void atomicAddToViews(const ViewTuple& views,
                                                const Indices& elementNDIndex,
                                                const Mapping& map, const T value) {
        if (T* destination = mappedDOFPointer<T>(views, elementNDIndex, map))
            Kokkos::atomic_add(destination, value);
    }
}
#endif
