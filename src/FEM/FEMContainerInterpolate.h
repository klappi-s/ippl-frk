#ifndef IPPL_FEM_CONTAINER_INTERPOLATE_H
#define IPPL_FEM_CONTAINER_INTERPOLATE_H

#include "Ippl.h"

#include "Utility/Timer.h"

#include "FEM/FEMInterpolate.hpp"
#include "FEM/LagrangeSpace_wFEMContainer.h"

namespace ippl {
    struct FEMCouplingStatistics {
        double ownershipSeconds = 0, localSeconds = 0, haloSeconds = 0;
    };
    namespace detail {
        template <typename T, unsigned Dim>
        KOKKOS_INLINE_FUNCTION void locatePeriodicFEMParticle(const Vector<T, Dim>& spacing,
                                                              const Vector<T, Dim>& origin,
                                                              const Vector<T, Dim>& lengths,
                                                              Vector<T, Dim> x,
                                                              Vector<size_t, Dim>& cell,
                                                              Vector<T, Dim>& xi) {
            for (unsigned d = 0; d < Dim; ++d) {
                const T distance = x[d] - origin[d];
                x[d] = origin[d] + distance - Kokkos::floor(distance / lengths[d]) * lengths[d];
                if (x[d] >= origin[d] + lengths[d])
                    x[d] = origin[d];
            }
            locate_element_nd_and_xi(spacing, origin, x, cell, xi);
        }

        // Namespace-scope functor avoids extended device lambdas inside generic lambdas.
        template <bool Deposit, typename Entity, typename FieldView, typename Positions,
                  typename Values, typename DeviceSpace, typename Handler, unsigned Dim>
        struct FEMParticleCoupling {
            using T = typename Positions::value_type::value_type;
            FieldView field;
            Positions positions;
            Values values;
            DeviceSpace space;
            Handler handler;
            Vector<T, Dim> spacing, origin, lengths;
            Vector<int, Dim> first;
            int ghosts;

            KOKKOS_INLINE_FUNCTION void operator()(size_t particle) const {
                Vector<size_t, Dim> cell;
                Vector<T, Dim> xi;
                locatePeriodicFEMParticle(spacing, origin, lengths, positions(particle), cell, xi);
                Vector<size_t, Dim> local;
                for (unsigned d = 0; d < Dim; ++d)
                    local[d] = cell[d] - first[d] + ghosts;
                constexpr auto Begin = Handler::template getEntityDOFStart<Entity>();
                constexpr auto End   = Handler::template getEntityDOFEnd<Entity>();
                for (size_t i = Begin; i < End; ++i) {
                    const auto map = handler.getElementDOFMapping(i);
                    auto& coefficient =
                        apply(field, local + map.entityLocalIndex)[map.entityLocalDOF];
                    if constexpr (Deposit) {
                        Kokkos::atomic_add(
                            &coefficient,
                            values(particle) * space.evaluateRefElementShapeFunction(i, xi));
                    } else {
                        const auto gradient = space.evaluateRefElementShapeFunctionGradient(i, xi);
                        for (unsigned d = 0; d < Dim; ++d)
                            values(particle)[d] -= coefficient * gradient[d] / spacing[d];
                    }
                }
            }
        };

        template <bool Deposit, typename Field, typename Space, typename Positions, typename Values>
        FEMCouplingStatistics coupleFEMParticles(Field& field, const Space& space,
                                                 Positions positions, Values values, size_t count) {
            FEMCouplingStatistics statistics;
            Timer timer;
            Kokkos::fence();
            timer.start();
            constexpr unsigned Dim = Field::dim;
            using T                = typename Field::value_type;
            if (!field.getLayout().isAllPeriodic_m)
                throw IpplException("coupleFEMParticles", "Requires periodic FEM ownership.");
            if (count > positions.extent(0) || count > values.extent(0))
                throw IpplException("coupleFEMParticles", "Particle view is too small.");
            const auto spacing = field.get_mesh().getMeshSpacing();
            const auto origin  = field.get_mesh().getOrigin();
            Vector<T, Dim> lengths;
            Vector<int, Dim> first, last;
            const auto owned = space.getDOFHandler().getLocalElementDomain();
            for (unsigned d = 0; d < Dim; ++d) {
                lengths[d] = spacing[d] * (field.get_mesh().getGridsize()[d] - 1);
                first[d]   = owned[d].first();
                last[d]    = owned[d].last();
            }
            using Exec       = typename Positions::execution_space;
            using Policy     = Kokkos::RangePolicy<Exec>;
            unsigned invalid = 0;
            Kokkos::parallel_reduce(
                "FEM particle ownership", Policy(0, count),
                KOKKOS_LAMBDA(size_t p, unsigned& errors) {
                    for (unsigned d = 0; d < Dim; ++d)
                        if (!Kokkos::isfinite(positions(p)[d])) {
                            ++errors;
                            return;
                        }
                    Vector<size_t, Dim> cell;
                    Vector<T, Dim> xi;
                    detail::locatePeriodicFEMParticle(spacing, origin, lengths, positions(p), cell,
                                                      xi);
                    for (unsigned d = 0; d < Dim; ++d)
                        if (cell[d] < static_cast<size_t>(first[d])
                            || cell[d] > static_cast<size_t>(last[d])) {
                            ++errors;
                            break;
                        }
                },
                invalid);
            unsigned globalInvalid = 0;
            Comm->allreduce(invalid, globalInvalid, 1, std::plus<unsigned>());
            if (globalInvalid)
                throw IpplException("coupleFEMParticles",
                                    "Migrate particles to their element owners first.");
            Kokkos::fence();
            timer.stop();
            statistics.ownershipSeconds = timer.elapsed();
            timer.clear();
            timer.start();
            if constexpr (Deposit)
                field = T(0);
            else {
                field.fillHalo();
                Kokkos::fence();
                timer.stop();
                statistics.haloSeconds = timer.elapsed();
                timer.clear();
                timer.start();
                Kokkos::parallel_for(
                    "Clear FEM particle field", Policy(0, count),
                    KOKKOS_LAMBDA(size_t p) { values(p) = Vector<T, Dim>(0); });
            }
            auto process = [&]<typename Entity>() {
                auto view    = field.template getView<Entity>();
                auto device  = space.getDeviceMirror();
                auto handler = space.getDOFHandler();
                using Functor =
                    FEMParticleCoupling<Deposit, Entity, decltype(view), Positions, Values,
                                        decltype(device), decltype(handler), Dim>;
                Kokkos::parallel_for(Deposit ? "FEM particle deposition" : "FEM particle gradient",
                                     Policy(0, count),
                                     Functor{view, positions, values, device, handler, spacing,
                                             origin, lengths, first, field.getNghost()});
            };
            [&]<typename... Entities>(std::tuple<Entities...>) {
                (process.template operator()<Entities>(), ...);
            }(Field::getEntityTypes());
            Kokkos::fence();
            timer.stop();
            statistics.localSeconds = timer.elapsed();
            if constexpr (Deposit) {
                timer.clear();
                timer.start();
                field.accumulateHalo();
                Kokkos::fence();
                timer.stop();
                statistics.haloSeconds = timer.elapsed();
            }
            return statistics;
        }
    }  // namespace detail

    // Produces the assembled weak load sum(q_p * phi_i(x_p)), without mass division.
    template <typename Field, typename Space, typename Positions, typename Charges>
    FEMCouplingStatistics depositFEMLoad(Field& load, const Space& space, Positions positions,
                                         Charges charges, size_t count) {
        return detail::coupleFEMParticles<true>(load, space, positions, charges, count);
    }

    // Gathers E=-grad(phi_h), using the same higher-order basis as deposition.
    template <typename Field, typename Space, typename Positions, typename ElectricField>
    FEMCouplingStatistics gatherFEMElectricField(Field& potential, const Space& space,
                                                 Positions positions, ElectricField electric,
                                                 size_t count) {
        return detail::coupleFEMParticles<false>(potential, space, positions, electric, count);
    }
}  // namespace ippl
#endif
