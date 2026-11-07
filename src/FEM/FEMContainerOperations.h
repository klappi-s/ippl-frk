// Owned entity iteration shared by FEMContainer reductions and value operations.
#ifndef IPPL_FEM_CONTAINER_OPERATIONS_H
#define IPPL_FEM_CONTAINER_OPERATIONS_H

#include "FEM/FEMFieldAccess.h"
#include "Types/Vector.h"

namespace ippl::detail {
    template <unsigned Dim, size_t Count>
    struct FEMOwnedEntityRanges {
        Kokkos::Array<Vector<size_t, Dim>, Count> extents;
        Kokkos::Array<size_t, Count + 1> offsets{};
        size_t ghosts;

        template <typename View>
        KOKKOS_INLINE_FUNCTION Vector<size_t, Dim> index(size_t entity, size_t flat) const {
            Vector<size_t, Dim> result;
            // Respect each backend's contiguous view dimension when flattening.
            for (unsigned axis = 0; axis < Dim; ++axis) {
                const unsigned d = std::is_same_v<typename View::array_layout, Kokkos::LayoutLeft>
                                       ? axis : Dim - 1 - axis;
                result[d] = ghosts + flat % extents[entity][d];
                flat /= extents[entity][d];
            }
            return result;
        }
    };

    template <typename Container, typename Views>
    auto makeFEMOwnedEntityRanges(const Container& field, const Views& views) {
        FEMOwnedEntityRanges<Container::dim, Container::NEntitys> ranges;
        ranges.ghosts = field.getNghost();
        [&]<size_t... Is>(std::index_sequence<Is...>) {
            ([&] {
                size_t count = 1;
                for (unsigned d = 0; d < Container::dim; ++d) {
                    ranges.extents[Is][d] = ippl::get<Is>(views).extent(d) - 2 * ranges.ghosts;
                    count *= ranges.extents[Is][d];
                }
                ranges.offsets[Is + 1] = ranges.offsets[Is] + count;
            }(), ...);
        }(std::make_index_sequence<Container::NEntitys>{});
        return ranges;
    }

    template <typename T, typename DOFNums, typename Views, typename Ranges, bool Norm>
    struct FEMContainerReduction {
        Views a, b;
        Ranges ranges;
        int p;

        template <size_t I = 0>
        KOKKOS_INLINE_FUNCTION void add(size_t flat, T& sum) const {
            if constexpr (I < std::tuple_size_v<DOFNums>) {
                if (flat < ranges.offsets[I + 1]) {
                    const auto& view = ippl::get<I>(a);
                    using View = std::remove_cvref_t<decltype(view)>;
                    const auto index = ranges.template index<View>(I, flat - ranges.offsets[I]);
                    const auto& left = apply(view, index);
                    constexpr size_t NUM_DOFS = std::tuple_element_t<I, DOFNums>::value;
                    for (size_t dof = 0; dof < NUM_DOFS; ++dof) {
                        if constexpr (Norm) sum += Kokkos::pow(Kokkos::abs(left[dof]), p);
                        else sum += left[dof] * apply(ippl::get<I>(b), index)[dof];
                    }
                } else add<I + 1>(flat, sum);
            }
        }
        KOKKOS_INLINE_FUNCTION void operator()(size_t flat, T& sum) const { add(flat, sum); }
    };

    template <bool Norm, typename DOFNums, typename Container>
    auto reduceFEMOwnedValues(const Container& a, const Container& b, int p = 2) {
        const auto left = make_entity_view_tuple<Container, decltype(Container::getEntityTypes())>(a);
        const auto right = make_entity_view_tuple<Container, decltype(Container::getEntityTypes())>(b);
        const auto ranges = makeFEMOwnedEntityRanges(a, left);
        using View = std::remove_cvref_t<decltype(ippl::get<0>(left))>;
        using Exec = typename View::execution_space;
        using T = typename Container::value_type;
        using Policy = Kokkos::RangePolicy<Exec, Kokkos::IndexType<size_t>>;
        using Functor = FEMContainerReduction<T, DOFNums, decltype(left), decltype(ranges), Norm>;
        T local = 0;
        // This is a concatenation of differently centered field ranges, rather
        // than one multidimensional field range. It has one host result sync.
        Kokkos::parallel_reduce(Norm ? "FEMContainer owned norm" : "FEMContainer owned innerProduct",
            Policy(0, ranges.offsets[Container::NEntitys]), Functor{left, right, ranges, p},
            Kokkos::Sum<T>(local));
        return local;
    }

    template <typename T, typename DOFNums, typename Views, typename Ranges, bool LinearCombination>
    struct FEMContainerValueOperation {
        Views result, x, y;
        Ranges ranges;
        T alpha, beta;

        template <size_t I = 0>
        KOKKOS_INLINE_FUNCTION void update(size_t flat) const {
            if constexpr (I < std::tuple_size_v<DOFNums>) {
                if (flat < ranges.offsets[I + 1]) {
                    const auto& view = ippl::get<I>(result);
                    using View = std::remove_cvref_t<decltype(view)>;
                    const auto index = ranges.template index<View>(I, flat - ranges.offsets[I]);
                    auto& output = apply(view, index);
                    constexpr size_t NUM_DOFS = std::tuple_element_t<I, DOFNums>::value;
                    for (size_t dof = 0; dof < NUM_DOFS; ++dof) {
                        if constexpr (LinearCombination) {
                            // Load both inputs before storing, including either alias.
                            const T left = apply(ippl::get<I>(x), index)[dof];
                            const T right = apply(ippl::get<I>(y), index)[dof];
                            output[dof] = alpha * left + beta * right;
                        } else output[dof] -= alpha;
                    }
                } else update<I + 1>(flat);
            }
        }
        KOKKOS_INLINE_FUNCTION void operator()(size_t flat) const { update(flat); }
    };

    template <bool LinearCombination, typename DOFNums, typename Container>
    void updateFEMOwnedValues(Container& result, const Container& x, const Container& y,
                              typename Container::value_type alpha,
                              typename Container::value_type beta = 0) {
        using Entities = decltype(Container::getEntityTypes());
        const auto output = make_entity_view_tuple<Container, Entities>(result);
        const auto left = make_entity_view_tuple<Container, Entities>(x);
        const auto right = make_entity_view_tuple<Container, Entities>(y);
        const auto ranges = makeFEMOwnedEntityRanges(result, output);
        using View = std::remove_cvref_t<decltype(ippl::get<0>(output))>;
        using Exec = typename View::execution_space;
        using T = typename Container::value_type;
        using Policy = Kokkos::RangePolicy<Exec, Kokkos::IndexType<size_t>>;
        using Functor = FEMContainerValueOperation<T, DOFNums, decltype(output), decltype(ranges), LinearCombination>;
        Kokkos::parallel_for(LinearCombination ? "FEMContainer owned linear combination"
                                              : "FEMContainer owned scalar subtraction",
            Policy(0, ranges.offsets[Container::NEntitys]),
            Functor{output, left, right, ranges, alpha, beta});
    }

}
#endif
