//
// Order-transfer operators for Track B p-multigrid (Lagrange, same mesh).
// Prolongation: evaluate coarse FE function at fine DOF nodes.
// Restriction: evaluate fine FE function at coarse DOF nodes.
//

#ifndef IPPL_FEM_ORDER_TRANSFER_H
#define IPPL_FEM_ORDER_TRANSFER_H

#include "FEM/DOFHandler.h"
#include "FEM/DOFLocations.h"
#include "FEM/FiniteElementSpaceTraits.h"
#include "Utility/IpplTimings.h"

namespace ippl {

    namespace detail {

        template <typename T, unsigned Dim, unsigned Order>
        KOKKOS_INLINE_FUNCTION T lagrange_shape_at(const size_t localDOF,
                                                   const Vector<T, Dim>& localPoint,
                                                   const LagrangeDOFLocations<T, Dim, Order>& locs) {
            const Vector<T, Dim> ref = locs.locations[localDOF];
            T product                = T(1);
            for (unsigned d = 0; d < Dim; ++d) {
                T basis_1d = T(1);
                for (unsigned k = 0; k <= Order; ++k) {
                    const T node_k = static_cast<T>(k) / static_cast<T>(Order);
                    if (Kokkos::abs(ref[d] - node_k) < T(1e-10)) {
                        continue;
                    }
                    basis_1d *= (localPoint[d] - node_k) / (ref[d] - node_k);
                }
                product *= basis_1d;
            }
            return product;
        }

        template <typename Container, typename EntityTypes>
        auto make_entity_view_tuple(Container& field) {
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                return std::make_tuple(field.template getView<std::tuple_element_t<Is, EntityTypes>>()...);
            }(std::make_index_sequence<std::tuple_size_v<EntityTypes>>{});
        }

        template <typename Container, typename EntityTypes>
        auto make_entity_view_tuple(const Container& field) {
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                return std::make_tuple(field.template getView<std::tuple_element_t<Is, EntityTypes>>()...);
            }(std::make_index_sequence<std::tuple_size_v<EntityTypes>>{});
        }

        template <typename T, typename ViewTuple, typename Indices, typename Mapping>
        KOKKOS_INLINE_FUNCTION T gather_from_views(const ViewTuple& views,
                                                   const Indices& elementNDIndex,
                                                   const Mapping& map) {
            T val = T(0);
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                ((Is == map.entityTypeIndex
                      ? (val = apply(std::get<Is>(views),
                                     elementNDIndex + map.entityLocalIndex)[map.entityLocalDOF],
                         void())
                      : void()),
                 ...);
            }(std::make_index_sequence<std::tuple_size_v<ViewTuple>>{});
            return val;
        }

        template <typename ViewTuple, typename Indices, typename Mapping, typename T>
        KOKKOS_INLINE_FUNCTION void scatter_to_views(ViewTuple& views, const Indices& elementNDIndex,
                                                     const Mapping& map, const T value) {
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                ((Is == map.entityTypeIndex
                      ? (Kokkos::atomic_store(
                             &apply(std::get<Is>(views),
                                    elementNDIndex + map.entityLocalIndex)[map.entityLocalDOF],
                             value),
                         void())
                      : void()),
                 ...);
            }(std::make_index_sequence<std::tuple_size_v<ViewTuple>>{});
        }

    }  // namespace detail

    /**
     * @brief Same-mesh Lagrange order transfer for p-multigrid.
     */
    template <typename T, unsigned Dim, unsigned OrderFine, unsigned OrderCoarse>
    class FEMOrderTransfer {
        static_assert(OrderFine > OrderCoarse, "OrderFine must exceed OrderCoarse");
        static_assert(OrderCoarse >= 1, "OrderCoarse must be >= 1");

    public:
        using TraitsFine      = FiniteElementSpaceTraits<LagrangeSpaceTag, Dim, OrderFine>;
        using TraitsCoarse    = FiniteElementSpaceTraits<LagrangeSpaceTag, Dim, OrderCoarse>;
        using HandlerFine     = DOFHandler<T, TraitsFine>;
        using HandlerCoarse   = DOFHandler<T, TraitsCoarse>;
        using ContainerFine   = typename HandlerFine::FEMContainer_t;
        using ContainerCoarse = typename HandlerCoarse::FEMContainer_t;
        using indices_t       = typename HandlerFine::indices_t;

        static constexpr unsigned NF = TraitsFine::dofsPerElement;
        static constexpr unsigned NC = TraitsCoarse::dofsPerElement;

        FEMOrderTransfer() {
            P_m = Kokkos::View<T**, Kokkos::LayoutRight>("FEMOrderTransfer::P", NF, NC);
            R_m = Kokkos::View<T**, Kokkos::LayoutRight>("FEMOrderTransfer::R", NC, NF);

            LagrangeDOFLocations<T, Dim, OrderFine> fineLocs;
            LagrangeDOFLocations<T, Dim, OrderCoarse> coarseLocs;

            auto Ph = Kokkos::create_mirror_view(P_m);
            auto Rh = Kokkos::create_mirror_view(R_m);
            for (unsigned i = 0; i < NF; ++i) {
                for (unsigned j = 0; j < NC; ++j) {
                    Ph(i, j) = detail::lagrange_shape_at<T, Dim, OrderCoarse>(
                        j, fineLocs.locations[i], coarseLocs);
                }
            }
            for (unsigned j = 0; j < NC; ++j) {
                for (unsigned i = 0; i < NF; ++i) {
                    // Injection: evaluate fine FE function at coarse DOF nodes
                    Rh(j, i) = detail::lagrange_shape_at<T, Dim, OrderFine>(
                        i, coarseLocs.locations[j], fineLocs);
                }
            }
            Kokkos::deep_copy(P_m, Ph);
            Kokkos::deep_copy(R_m, Rh);
        }

        void prolong(const ContainerCoarse& uc, ContainerFine& uf, const HandlerFine& handlerFine,
                     const HandlerCoarse& handlerCoarse,
                     const Kokkos::View<const size_t*>& elementIndices) const {
            IpplTimings::TimerRef timer = IpplTimings::getTimer("fem_pmg_prolong");
            IpplTimings::startTimer(timer);

            uf               = T(0);
            const int nghost = uf.getNghost();
            auto P           = P_m;
            auto dhF         = handlerFine;
            auto dhC         = handlerCoarse;
            auto elems       = elementIndices;
            auto viewsC      = detail::make_entity_view_tuple<ContainerCoarse,
                                                         typename HandlerCoarse::EntityTypes>(uc);
            auto viewsF =
                detail::make_entity_view_tuple<ContainerFine, typename HandlerFine::EntityTypes>(uf);

            using exec_space  = typename Kokkos::View<const size_t*>::execution_space;
            using policy_type = Kokkos::RangePolicy<exec_space>;

            Kokkos::parallel_for(
                "FEMOrderTransfer::prolong", policy_type(0, elems.extent(0)),
                KOKKOS_LAMBDA(const size_t index) {
                    const size_t elementIndex = elems(index);
                    const indices_t eF        = dhF.getLocalElementNDIndex(elementIndex, nghost);
                    const indices_t eC        = dhC.getLocalElementNDIndex(elementIndex, nghost);

                    T uc_loc[NC];
                    for (unsigned j = 0; j < NC; ++j) {
                        uc_loc[j] = detail::gather_from_views<T>(viewsC, eC,
                                                                  dhC.getElementDOFMapping(j));
                    }
                    for (unsigned i = 0; i < NF; ++i) {
                        T val = T(0);
                        for (unsigned j = 0; j < NC; ++j) {
                            val += P(i, j) * uc_loc[j];
                        }
                        detail::scatter_to_views(viewsF, eF, dhF.getElementDOFMapping(i), val);
                    }
                });
            Kokkos::fence();
            uf.fillHalo();

            IpplTimings::stopTimer(timer);
        }

        void restrict(const ContainerFine& uf, ContainerCoarse& uc, const HandlerFine& handlerFine,
                      const HandlerCoarse& handlerCoarse,
                      const Kokkos::View<const size_t*>& elementIndices) const {
            IpplTimings::TimerRef timer = IpplTimings::getTimer("fem_pmg_restrict");
            IpplTimings::startTimer(timer);

            uc               = T(0);
            const int nghost = uf.getNghost();
            auto R           = R_m;
            auto dhF         = handlerFine;
            auto dhC         = handlerCoarse;
            auto elems       = elementIndices;
            auto viewsF      = detail::make_entity_view_tuple<ContainerFine,
                                                         typename HandlerFine::EntityTypes>(uf);
            auto viewsC =
                detail::make_entity_view_tuple<ContainerCoarse, typename HandlerCoarse::EntityTypes>(
                    uc);

            using exec_space  = typename Kokkos::View<const size_t*>::execution_space;
            using policy_type = Kokkos::RangePolicy<exec_space>;

            Kokkos::parallel_for(
                "FEMOrderTransfer::restrict", policy_type(0, elems.extent(0)),
                KOKKOS_LAMBDA(const size_t index) {
                    const size_t elementIndex = elems(index);
                    const indices_t eF        = dhF.getLocalElementNDIndex(elementIndex, nghost);
                    const indices_t eC        = dhC.getLocalElementNDIndex(elementIndex, nghost);

                    T uf_loc[NF];
                    for (unsigned i = 0; i < NF; ++i) {
                        uf_loc[i] = detail::gather_from_views<T>(viewsF, eF,
                                                                  dhF.getElementDOFMapping(i));
                    }
                    for (unsigned j = 0; j < NC; ++j) {
                        T val = T(0);
                        for (unsigned i = 0; i < NF; ++i) {
                            val += R(j, i) * uf_loc[i];
                        }
                        detail::scatter_to_views(viewsC, eC, dhC.getElementDOFMapping(j), val);
                    }
                });
            Kokkos::fence();
            uc.fillHalo();

            IpplTimings::stopTimer(timer);
        }

    private:
        Kokkos::View<T**, Kokkos::LayoutRight> P_m;
        Kokkos::View<T**, Kokkos::LayoutRight> R_m;
    };

}  // namespace ippl

#endif  // IPPL_FEM_ORDER_TRANSFER_H
