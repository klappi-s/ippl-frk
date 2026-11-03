#ifndef IPPL_VECTOR_LAYOUT_H
#define IPPL_VECTOR_LAYOUT_H

#include <Kokkos_Core.hpp>

#include <type_traits>

namespace ippl {
    // Component organization, independent of spatial layout and memory/execution space.
    struct AoS {};
    struct SoA {};

    namespace detail {
        template <typename T>
        inline constexpr bool is_vector_layout_v = std::is_same_v<T, AoS> || std::is_same_v<T, SoA>;

        template <typename... Properties>
        struct VectorStorage {
            static constexpr unsigned COUNT = (0u + ... + unsigned(is_vector_layout_v<Properties>));
            static_assert(COUNT <= 1, "Specify at most one IPPL vector storage layout");
#ifdef IPPL_ENABLE_VECTOR_SOA
            using default_layout = SoA;
#else
            using default_layout = AoS;
#endif
            using layout = std::conditional_t<
                (std::is_same_v<Properties, SoA> || ...), SoA,
                std::conditional_t<(std::is_same_v<Properties, AoS> || ...), AoS, default_layout>>;
        };

        template <typename... Properties>
        struct ViewProperties {
            template <typename Data>
            using view = Kokkos::View<Data, Properties...>;
        };

        template <typename Selected, typename... Remaining>
        struct FilterViewProperties;

        template <typename... Selected>
        struct FilterViewProperties<ViewProperties<Selected...>> {
            using type = ViewProperties<Selected...>;
        };

        template <typename... Selected, typename Next, typename... Remaining>
        struct FilterViewProperties<ViewProperties<Selected...>, Next, Remaining...>
            : FilterViewProperties<
                  std::conditional_t<is_vector_layout_v<Next>, ViewProperties<Selected...>,
                                     ViewProperties<Selected..., Next>>,
                  Remaining...> {};

        // Native storage, including scalar scratch arrays and the unchanged AoS wire buffers.
        template <typename Data, typename... Properties>
        using NativeView = typename FilterViewProperties<ViewProperties<>,
                                                         Properties...>::type ::template view<Data>;
    }  // namespace detail
}  // namespace ippl
#endif
