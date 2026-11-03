#ifndef IPPL_VECTOR_COMPONENT_H
#define IPPL_VECTOR_COMPONENT_H

#include "Types/VectorView.h"

namespace ippl {
    namespace detail {
        // Keep the native AoS owner alive and access members without aliasing Vector[] as T[].
        template <typename View>
        class AoSComponent {
        public:
            static constexpr unsigned rank = View::rank;
            using memory_space             = typename View::memory_space;
            using execution_space          = typename View::execution_space;
            using size_type                = typename View::size_type;
            using non_const_value_type     = typename View::non_const_value_type::value_type;
            using value_type = std::conditional_t<std::is_const_v<typename View::value_type>,
                                                  const non_const_value_type, non_const_value_type>;

            AoSComponent(const View& view, unsigned component)
                : view_m(view)
                , component_m(component) {}
            KOKKOS_DEFAULTED_FUNCTION AoSComponent(const AoSComponent&)            = default;
            KOKKOS_DEFAULTED_FUNCTION AoSComponent& operator=(const AoSComponent&) = default;

            template <typename... Indices>
            KOKKOS_INLINE_FUNCTION decltype(auto) operator()(Indices... indices) const {
                return view_m(indices...)[component_m];
            }
            KOKKOS_INLINE_FUNCTION size_type extent(unsigned d) const { return view_m.extent(d); }
            KOKKOS_INLINE_FUNCTION int extent_int(unsigned d) const { return view_m.extent_int(d); }
            KOKKOS_INLINE_FUNCTION size_type size() const { return view_m.size(); }

        private:
            View view_m;
            unsigned component_m;
        };
    }  // namespace detail

    // Host-side view construction; returned accessors can be captured by device kernels.
    template <typename View>
        requires(is_vector_v<typename View::non_const_value_type>)
    auto componentView(const View& view, unsigned component) {
        if (component >= View::non_const_value_type::dim)
            throw std::out_of_range("Vector component index out of range");
        if constexpr (requires { view.component(component); }) {
            return view.component(component);  // native scalar Kokkos subview, no copy
        } else {
            return detail::AoSComponent<View>(view, component);
        }
    }
}  // namespace ippl
#endif
