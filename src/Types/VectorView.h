#ifndef IPPL_VECTOR_VIEW_H
#define IPPL_VECTOR_VIEW_H

#include <Kokkos_Core.hpp>

#include <stdexcept>

#include "Types/ViewTypes.h"

#include "Types/Vector.h"

#include "Utility/ParallelDispatch.h"

#include "Expression/IpplOperations.h"

namespace ippl {
    /** A reference to one vector in component-separated storage.
     * Copies of this object alias the same element. Convert to Vector to take a snapshot.
     * Assignment copies values, including when both operands are VectorReference objects.
     */
    template <typename T, unsigned Components>
    class VectorReference
        : public detail::Expression<VectorReference<T, Components>, sizeof(T*) + sizeof(size_t)> {
    public:
        using value_type              = std::remove_const_t<T>;
        using vector_type             = Vector<value_type, Components>;
        static constexpr unsigned dim = Components;

        KOKKOS_INLINE_FUNCTION VectorReference(T* first, size_t stride)
            : first_m(first)
            , stride_m(stride) {}
        KOKKOS_DEFAULTED_FUNCTION VectorReference(const VectorReference&) = default;

        KOKKOS_INLINE_FUNCTION T& operator[](unsigned d) const { return first_m[d * stride_m]; }
        KOKKOS_INLINE_FUNCTION T& operator()(unsigned d) const { return (*this)[d]; }

        KOKKOS_INLINE_FUNCTION operator vector_type() const {
            vector_type result;
            for (unsigned d = 0; d < Components; ++d)
                result[d] = (*this)[d];
            return result;
        }

        KOKKOS_INLINE_FUNCTION VectorReference& operator=(const VectorReference& rhs)
            requires(!std::is_const_v<T>)
        {
            return assign(rhs);
        }

        KOKKOS_INLINE_FUNCTION VectorReference& operator=(std::initializer_list<value_type> rhs)
            requires(!std::is_const_v<T>)
        {
            return assign(vector_type(rhs));
        }

        template <typename E>
        KOKKOS_INLINE_FUNCTION VectorReference& operator=(const E& rhs)
            requires(!std::is_const_v<T>)
        {
            return assign(rhs);
        }

#define IPPL_VECTOR_REFERENCE_COMPOUND(OP)                            \
    template <typename E>                                             \
    KOKKOS_INLINE_FUNCTION VectorReference& operator OP(const E& rhs) \
        requires(!std::is_const_v<T>)                                 \
    {                                                                 \
        vector_type value = *this;                                    \
        value OP rhs;                                                 \
        return assign(value);                                         \
    }
        IPPL_VECTOR_REFERENCE_COMPOUND(+=)
        IPPL_VECTOR_REFERENCE_COMPOUND(-=)
        IPPL_VECTOR_REFERENCE_COMPOUND(*=)
        IPPL_VECTOR_REFERENCE_COMPOUND(/=)
#undef IPPL_VECTOR_REFERENCE_COMPOUND

        KOKKOS_INLINE_FUNCTION value_type dot(const vector_type& rhs) const {
            const vector_type value = *this;
            return value.dot(rhs);
        }
        KOKKOS_INLINE_FUNCTION value_type Pnorm(int p = 2) const {
            const vector_type value = *this;
            return value.Pnorm(p);
        }

        friend std::ostream& operator<<(std::ostream& stream, const VectorReference& value) {
            const vector_type snapshot = value;
            return stream << snapshot;
        }

    private:
        template <typename E>
        KOKKOS_INLINE_FUNCTION VectorReference& assign(const E& rhs) {
            // Snapshot before writing: expressions may read other components of this element.
            const vector_type value = rhs;
            for (unsigned d = 0; d < Components; ++d)
                (*this)[d] = value[d];
            return *this;
        }
        T* first_m;
        size_t stride_m;
    };

    namespace detail {
        template <typename T, unsigned C>
        struct isExpression<VectorReference<T, C>> : std::true_type {};
    }  // namespace detail

    /** Vector-valued facade over one scalar Kokkos allocation.
     * The component dimension is slowest; the remaining dimensions keep their spatial layout.
     * It is intentionally not a Kokkos::View. component(d) returns a native scalar view.
     */
    template <typename ScalarView, unsigned Components, bool ComponentsFirst>
    class VectorView {
    public:
        static_assert(Components > 0);
        static constexpr unsigned rank         = ScalarView::rank - 1;
        static constexpr unsigned components   = Components;
        static constexpr bool components_first = ComponentsFirst;
        using scalar_view_type                 = ScalarView;
        using scalar_type                      = typename ScalarView::value_type;
        using non_const_value_type = Vector<typename ScalarView::non_const_value_type, Components>;
        using value_type           = non_const_value_type;
        using reference_type       = VectorReference<scalar_type, Components>;
        using memory_space         = typename ScalarView::memory_space;
        using execution_space      = typename ScalarView::execution_space;
        using device_type          = typename ScalarView::device_type;
        using array_layout         = typename ScalarView::array_layout;
        using memory_traits        = typename ScalarView::memory_traits;
        using size_type            = typename ScalarView::size_type;
        using data_type            = typename detail::NPtr<value_type, rank>::type;
        using host_mirror_type =
            VectorView<typename ScalarView::host_mirror_type, Components, ComponentsFirst>;
        using HostMirror = host_mirror_type;
        using uniform_type =
            VectorView<typename ScalarView::uniform_type, Components, ComponentsFirst>;

        VectorView()                                                       = default;
        KOKKOS_DEFAULTED_FUNCTION VectorView(const VectorView&)            = default;
        KOKKOS_DEFAULTED_FUNCTION VectorView& operator=(const VectorView&) = default;

        KOKKOS_INLINE_FUNCTION explicit VectorView(const ScalarView& storage)
            : storage_m(storage) {}

        template <typename OtherView>
            requires(std::is_constructible_v<ScalarView, OtherView>)
        KOKKOS_INLINE_FUNCTION VectorView(
            const VectorView<OtherView, Components, ComponentsFirst>& other)
            : storage_m(other.scalarView()) {}

        template <typename Label, typename... Extents>
            requires(sizeof...(Extents) == rank)
        explicit VectorView(const Label& label, Extents... extents)
            : storage_m(allocate(label, extents...)) {}

        template <typename... Indices>
        KOKKOS_INLINE_FUNCTION reference_type operator()(Indices... indices) const {
            static_assert(sizeof...(Indices) == rank);
            static_assert(!memory_traits::is_atomic,
                          "Use scalar component views for atomic vector storage access");
            if constexpr (ComponentsFirst) {
                return {&storage_m(0, indices...), storage_m.stride(0)};
            } else {
                return {&storage_m(indices..., 0), storage_m.stride(rank)};
            }
        }

        KOKKOS_INLINE_FUNCTION size_type extent(unsigned d) const {
            return d < rank ? storage_m.extent(d + (ComponentsFirst ? 1 : 0)) : 1;
        }
        KOKKOS_INLINE_FUNCTION int extent_int(unsigned d) const { return extent(d); }
        KOKKOS_INLINE_FUNCTION size_type size() const { return storage_m.size() / Components; }
        KOKKOS_INLINE_FUNCTION bool is_allocated() const { return storage_m.is_allocated(); }
        // Raw scalar storage, never a Vector*. Use component(d).data() for component export.
        KOKKOS_INLINE_FUNCTION auto data() const { return storage_m.data(); }
        KOKKOS_INLINE_FUNCTION const ScalarView& scalarView() const { return storage_m; }
        KOKKOS_INLINE_FUNCTION ScalarView& scalarView() { return storage_m; }
        std::string label() const { return storage_m.label(); }

        auto component(unsigned d) const {
            return [&]<size_t... I>(std::index_sequence<I...>) {
                if constexpr (ComponentsFirst) {
                    return Kokkos::subview(storage_m, d, (static_cast<void>(I), Kokkos::ALL)...);
                } else {
                    return Kokkos::subview(storage_m, (static_cast<void>(I), Kokkos::ALL)..., d);
                }
            }(std::make_index_sequence<rank>{});
        }

    private:
        template <typename Label, typename... Extents>
        static ScalarView allocate(const Label& label, Extents... extents) {
            static_assert(!std::is_same_v<array_layout, Kokkos::LayoutStride>,
                          "Allocate vector storage with LayoutLeft or LayoutRight, then subview");
            if constexpr (ComponentsFirst) {
                return ScalarView(label, Components, extents...);
            } else {
                return ScalarView(label, extents..., Components);
            }
        }
        ScalarView storage_m;
    };

    namespace detail {
#ifdef IPPL_ENABLE_VECTOR_SOA
        template <typename T, unsigned Components, unsigned Rank, typename... Properties>
            requires(!is_vector_v<T>)
        struct ViewType<Vector<T, Components>, Rank, Properties...> {
            // Nested vectors represent tensors (e.g. FFT Hessians), outside this vector adapter.
            using scalar_view = Kokkos::View<typename NPtr<T, Rank + 1>::type, Properties...>;
            using view_type =
                VectorView<scalar_view, Components,
                           std::is_same_v<typename scalar_view::array_layout, Kokkos::LayoutRight>>;
        };
#endif

        template <typename Dst, typename Src>
        void checkVectorViewExtents(const Dst& dst, const Src& src) {
            static_assert(Dst::rank == Src::rank);
            for (unsigned d = 0; d < Dst::rank; ++d) {
                if (dst.extent(d) != src.extent(d)) {
                    throw std::invalid_argument("VectorView::deep_copy: extent mismatch");
                }
            }
        }

        template <typename Dst, typename Src>
        void copyVectorElements(const Dst& dst, const Src& src) {
            Kokkos::fence("VectorView::convert input ready");
            using Index =
                typename RangePolicy<Dst::rank, typename Dst::execution_space>::index_array_type;
            ippl::parallel_for(
                "VectorView::convert", getRangePolicy(dst),
                KOKKOS_LAMBDA(const Index& index) { apply(dst, index) = apply(src, index); });
            typename Dst::execution_space{}.fence("VectorView::convert complete");
        }
    }  // namespace detail
}  // namespace ippl

#include "Types/VectorView.hpp"

#endif
