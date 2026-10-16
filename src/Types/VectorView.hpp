#ifndef IPPL_VECTOR_VIEW_HPP
#define IPPL_VECTOR_VIEW_HPP

#include "Types/VectorView.h"

namespace ippl::detail {
    template <typename Dst, typename Src>
    bool vectorStorageCanMemcpy(const Dst& dst, const Src& src) {
        if constexpr (std::is_same_v<typename Dst::array_layout, typename Src::array_layout>
                      && std::is_same_v<typename Dst::value_type,
                                        typename Src::non_const_value_type>) {
            if (!dst.span_is_contiguous() || !src.span_is_contiguous())
                return false;
            for (unsigned d = 0; d < Dst::rank; ++d) {
                if (dst.stride(d) != src.stride(d))
                    return false;
            }
            return true;
        }
        return false;
    }

    template <typename Dst, typename Src>
    void copyVectorComponent(const Dst& dst, const Src& src) {
        if constexpr (std::is_same_v<typename Dst::memory_space, typename Src::memory_space>) {
            bool identical =
                static_cast<const void*>(dst.data()) == static_cast<const void*>(src.data());
            for (unsigned d = 0; d < Dst::rank; ++d) {
                identical = identical && dst.stride(d) == src.stride(d);
            }
            if (identical)
                return;
        }
        constexpr bool CommonSpace =
            Kokkos::SpaceAccessibility<typename Dst::execution_space,
                                       typename Src::memory_space>::accessible
            || Kokkos::SpaceAccessibility<typename Src::execution_space,
                                          typename Dst::memory_space>::accessible;
        if constexpr (CommonSpace) {
            Kokkos::deep_copy(dst, src);
        } else {
            if (vectorStorageCanMemcpy(dst, src)) {
                Kokkos::deep_copy(dst, src);
                return;
            }
            // A cropped field cannot be copied directly between inaccessible memory spaces.
            // Pack on the source device, transfer a contiguous mirror, then remap locally.
            using Packed =
                Kokkos::View<typename NPtr<typename Src::non_const_value_type, Src::rank>::type,
                             Kokkos::LayoutLeft, typename Src::memory_space>;
            auto packed = [&]<size_t... I>(std::index_sequence<I...>) {
                return Packed("VectorView::copy staging", src.extent(I)...);
            }(std::make_index_sequence<Src::rank>{});
            Kokkos::deep_copy(packed, src);
            auto local = Kokkos::create_mirror_view_and_copy(typename Dst::memory_space{}, packed);
            Kokkos::deep_copy(dst, local);
        }
    }
}  // namespace ippl::detail

// Compatibility overloads for IPPL's view facade. Native scalar views always use Kokkos itself.
// Keep these visible before consumers define templates with qualified Kokkos calls.
namespace Kokkos {
    template <typename S, unsigned C, bool F, typename... Args>
    auto subview(const ippl::VectorView<S, C, F>& view, Args... args) {
        static_assert(sizeof...(Args) == S::rank - 1);
        auto scalar = [&] {
            if constexpr (F)
                return Kokkos::subview(view.scalarView(), Kokkos::ALL, args...);
            else
                return Kokkos::subview(view.scalarView(), args..., Kokkos::ALL);
        }();
        return ippl::VectorView<decltype(scalar), C, F>(scalar);
    }

    template <typename S, unsigned C, bool F>
    auto create_mirror(const ippl::VectorView<S, C, F>& view) {
        auto scalar = Kokkos::create_mirror(view.scalarView());
        return ippl::VectorView<decltype(scalar), C, F>(scalar);
    }

    template <typename Space, typename S, unsigned C, bool F>
    auto create_mirror(const Space& space, const ippl::VectorView<S, C, F>& view) {
        auto scalar = Kokkos::create_mirror(space, view.scalarView());
        return ippl::VectorView<decltype(scalar), C, F>(scalar);
    }

    template <typename S, unsigned C, bool F>
    auto create_mirror_view(const ippl::VectorView<S, C, F>& view) {
        auto scalar = Kokkos::create_mirror_view(view.scalarView());
        return ippl::VectorView<decltype(scalar), C, F>(scalar);
    }

    template <typename Space, typename S, unsigned C, bool F>
    auto create_mirror_view(const Space& space, const ippl::VectorView<S, C, F>& view) {
        auto scalar = Kokkos::create_mirror_view(space, view.scalarView());
        return ippl::VectorView<decltype(scalar), C, F>(scalar);
    }

    template <typename D, typename S, unsigned C, bool DF, bool SF>
    void deep_copy(const ippl::VectorView<D, C, DF>& dst, const ippl::VectorView<S, C, SF>& src) {
        ippl::detail::checkVectorViewExtents(dst, src);
        constexpr bool CommonSpace =
            Kokkos::SpaceAccessibility<typename D::execution_space,
                                       typename S::memory_space>::accessible
            || Kokkos::SpaceAccessibility<typename S::execution_space,
                                          typename D::memory_space>::accessible;
        if constexpr (DF == SF) {
            if constexpr (std::is_same_v<typename D::memory_space, typename S::memory_space>) {
                bool identical =
                    static_cast<const void*>(dst.data()) == static_cast<const void*>(src.data());
                for (unsigned d = 0; d < D::rank; ++d) {
                    identical =
                        identical && dst.scalarView().stride(d) == src.scalarView().stride(d);
                }
                if (identical)
                    return;
            }
            if (CommonSpace
                || ippl::detail::vectorStorageCanMemcpy(dst.scalarView(), src.scalarView())) {
                Kokkos::deep_copy(dst.scalarView(), src.scalarView());
                return;
            }
        }
        for (unsigned d = 0; d < C; ++d)
            ippl::detail::copyVectorComponent(dst.component(d), src.component(d));
    }

    template <typename Space, typename S, unsigned C, bool F>
    auto create_mirror_view_and_copy(const Space& space, const ippl::VectorView<S, C, F>& view) {
        auto mirror = Kokkos::create_mirror_view(space, view);
        if constexpr (!std::is_same_v<typename decltype(mirror)::memory_space,
                                      typename S::memory_space>) {
            Kokkos::deep_copy(mirror, view);
        } else {
            // Same-space mirrors alias the source, which may be const.
            Kokkos::fence("VectorView::mirror input ready");
        }
        return mirror;
    }

    // Explicit boundary conversions. If spaces differ, stage once using native Kokkos mirrors.
    template <typename S, unsigned C, bool F, typename Data, typename... Props>
    void deep_copy(const ippl::VectorView<S, C, F>& dst, const Kokkos::View<Data, Props...>& src) {
        static_assert(ippl::is_vector_v<typename Kokkos::View<Data, Props...>::value_type>);
        ippl::detail::checkVectorViewExtents(dst, src);
        auto local = Kokkos::create_mirror_view(typename S::memory_space{}, src);
        if constexpr (!std::is_same_v<typename decltype(local)::memory_space,
                                      typename Kokkos::View<Data, Props...>::memory_space>) {
            ippl::detail::copyVectorComponent(local, src);
        }
        ippl::detail::copyVectorElements(dst, local);
    }

    template <typename Data, typename... Props, typename S, unsigned C, bool F>
    void deep_copy(const Kokkos::View<Data, Props...>& dst, const ippl::VectorView<S, C, F>& src) {
        static_assert(ippl::is_vector_v<typename Kokkos::View<Data, Props...>::value_type>);
        ippl::detail::checkVectorViewExtents(dst, src);
        auto local = Kokkos::create_mirror_view_and_copy(
            typename Kokkos::View<Data, Props...>::memory_space{}, src);
        ippl::detail::copyVectorElements(dst, local);
    }

    template <typename S, unsigned C, bool F>
    void deep_copy(const ippl::VectorView<S, C, F>& dst,
                   const typename ippl::VectorView<S, C, F>::value_type& value) {
        for (unsigned d = 0; d < C; ++d)
            Kokkos::deep_copy(dst.component(d), value[d]);
    }

    template <typename S, unsigned C, bool F, typename... Extents>
    void resize(ippl::VectorView<S, C, F>& view, Extents... extents) {
        static_assert(sizeof...(Extents) == S::rank - 1);
        if constexpr (F)
            Kokkos::resize(view.scalarView(), C, extents...);
        else
            Kokkos::resize(view.scalarView(), extents..., C);
    }

    template <typename S, unsigned C, bool F, typename... Extents>
    void realloc(ippl::VectorView<S, C, F>& view, Extents... extents) {
        static_assert(sizeof...(Extents) == S::rank - 1);
        if constexpr (F)
            Kokkos::realloc(view.scalarView(), C, extents...);
        else
            Kokkos::realloc(view.scalarView(), extents..., C);
    }
}  // namespace Kokkos

#endif
