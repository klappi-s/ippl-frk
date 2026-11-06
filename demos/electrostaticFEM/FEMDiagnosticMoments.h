#pragma once

#include "FEM/FEMContainerInterpolate.h"
#include "FEM/FEMFieldAccess.h"

namespace electrostatic_fem {
    struct FEMDiagnosticValues {
        double energy = 0, real = 0, imaginary = 0;
    };

    namespace detail {
        template <typename Handler, typename Views, typename Entities, unsigned Dim>
        struct FEMDiagnosticReduction {
            using value_type = FEMDiagnosticValues;
            using Point = ippl::Vector<double, Dim>;
            using Index = ippl::Vector<size_t, Dim>;
            Handler handler;
            Views views;
            Kokkos::View<Point*> points;
            Kokkos::View<double*> weights;
            Kokkos::View<Point**> gradients;
            Index first, extent;
            Point spacing;
            int ghosts;
            double determinant, wave;

            KOKKOS_INLINE_FUNCTION void init(value_type& v) const { v = value_type{}; }
            KOKKOS_INLINE_FUNCTION void join(value_type& v, const value_type& w) const {
                v.energy += w.energy;
                v.real += w.real;
                v.imaginary += w.imaginary;
            }
            template <size_t I = 0>
            KOKKOS_INLINE_FUNCTION void addEntity(const Index& local, size_t q, Point& electric) const {
                if constexpr (I < std::tuple_size_v<Entities>) {
                    using Entity = std::tuple_element_t<I, Entities>;
                    constexpr size_t DOF_BEGIN = Handler::template getEntityDOFStart<Entity>();
                    constexpr size_t DOF_END = Handler::template getEntityDOFEnd<Entity>();
                    for (size_t i = DOF_BEGIN; i < DOF_END; ++i) {
                        const auto map = handler.getElementDOFMapping(i);
                        const auto coefficient = ippl::apply(ippl::get<I>(views),
                            local + map.entityLocalIndex)[map.entityLocalDOF];
                        for (unsigned d = 0; d < Dim; ++d)
                            electric[d] -= coefficient * gradients(i, q)[d] / spacing[d];
                    }
                    addEntity<I + 1>(local, q, electric);
                }
            }
            KOKKOS_INLINE_FUNCTION void operator()(size_t index, value_type& sum) const {
                Index cell;
                for (unsigned d = 0; d < Dim; ++d) {
                    cell[d] = first[d] + index % extent[d];
                    index /= extent[d];
                }
                const auto local = cell - first + Index(ghosts);
                for (size_t q = 0; q < points.extent(0); ++q) {
                    Point electric(0);
                    addEntity(local, q, electric);
                    const double weight = determinant * weights(q);
                    for (unsigned d = 0; d < Dim; ++d)
                        sum.energy += 0.5 * weight * electric[d] * electric[d];
                    // Retain the original diagnostic coordinate convention (zero mesh origin).
                    const double x = (cell[0] + points(q)[0]) * spacing[0];
                    sum.real += weight * electric[0] * Kokkos::cos(wave * x);
                    sum.imaginary += weight * electric[0] * Kokkos::sin(wave * x);
                }
            }
        };
    }

    // Cache only reference-cell data. No storage scales with the number of mesh cells.
    template <unsigned Dim>
    class FEMDiagnosticMoments {
        using Point = ippl::Vector<double, Dim>;
        Kokkos::View<Point*> points_m;
        Kokkos::View<double*> weights_m;
        Kokkos::View<Point**> gradients_m;

    public:
        template <typename Space, typename Quadrature>
        void initialize(const Space& space, const Quadrature& quadrature) {
            const auto points = quadrature.getIntegrationNodesForRefElement();
            const auto weights = quadrature.getWeightsForRefElement();
            // Endpoint gathering selects the adjacent element. The demo uses interior GL
            // rules; reject other rules rather than silently changing its field definition.
            for (unsigned q = 0; q < points.dim; ++q)
                for (unsigned d = 0; d < Dim; ++d)
                    if (!(points[q][d] > 0 && points[q][d] < 1))
                        throw IpplException("FEMDiagnosticMoments", "Requires interior quadrature nodes.");
            using Handler = std::remove_cvref_t<decltype(space.getDOFHandler())>;
            points_m = Kokkos::View<Point*>("FEM diagnostic reference points", points.dim);
            weights_m = Kokkos::View<double*>("FEM diagnostic reference weights", points.dim);
            gradients_m = Kokkos::View<Point**>("FEM diagnostic reference gradients",
                                               Handler::dofsPerElement, points.dim);
            auto hostPoints = Kokkos::create_mirror_view(points_m);
            auto hostWeights = Kokkos::create_mirror_view(weights_m);
            for (unsigned q = 0; q < points.dim; ++q) {
                hostPoints(q) = points[q];
                hostWeights(q) = weights[q];
            }
            Kokkos::deep_copy(points_m, hostPoints);
            Kokkos::deep_copy(weights_m, hostWeights);
            const auto device = space.getDeviceMirror();
            const auto devicePoints = points_m;
            const auto gradients = gradients_m;
            using Exec = typename decltype(points_m)::execution_space;
            Kokkos::parallel_for("Cache FEM diagnostic reference gradients",
                Kokkos::RangePolicy<Exec>(0, Handler::dofsPerElement),
                KOKKOS_LAMBDA(size_t i) {
                    for (size_t q = 0; q < devicePoints.extent(0); ++q)
                        gradients(i, q) = device.evaluateRefElementShapeFunctionGradient(i, devicePoints(q));
                });
        }

        template <typename Field, typename Space>
        FEMDiagnosticValues evaluate(Field& potential, const Space& space, double wave) const {
            if (!points_m.extent(0))
                throw IpplException("FEMDiagnosticMoments", "Initialize reference quadrature first.");
            if (!potential.getLayout().isAllPeriodic_m)
                throw IpplException("FEMDiagnosticMoments", "Requires periodic FEM ownership.");
            potential.fillHalo();
            const auto handler = space.getDOFHandler().getDeviceMirror();
            using Entities = decltype(Field::getEntityTypes());
            const auto views = ippl::detail::make_entity_view_tuple<Field, Entities>(potential);
            using Functor = detail::FEMDiagnosticReduction<decltype(handler), decltype(views), Entities, Dim>;
            ippl::Vector<size_t, Dim> first, extent;
            const auto owned = space.getDOFHandler().getLocalElementDomain();
            size_t count = 1;
            double determinant = 1;
            const auto spacing = potential.get_mesh().getMeshSpacing();
            for (unsigned d = 0; d < Dim; ++d) {
                first[d] = owned[d].first();
                extent[d] = owned[d].length();
                count *= extent[d];
                determinant *= spacing[d];
            }
            using Exec = typename decltype(points_m)::execution_space;
            using Policy = Kokkos::RangePolicy<Exec, Kokkos::IndexType<size_t>>;
            FEMDiagnosticValues local;
            Kokkos::parallel_reduce("FEM diagnostic moments", Policy(0, count),
                Functor{handler, views, points_m, weights_m, gradients_m, first, extent,
                        spacing, potential.getNghost(), determinant, wave}, local);
            const double send[3] = {local.energy, local.real, local.imaginary};
            double receive[3] = {};
            ippl::Comm->allreduce<double>(send, receive, 3, std::plus<double>());
            return {receive[0], receive[1], receive[2]};
        }
    };
}
