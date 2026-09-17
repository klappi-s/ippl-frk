#pragma once

#include "Ippl.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <vector>

#include "FEM/LagrangeSpace_wFEMContainer.h"
#include "FEM/Quadrature/SelectableQuadrature.h"
#include "FEMOracleData.h"
#include "PoissonSolvers/EvalFunctor.h"
#include "gtest/gtest.h"

namespace fem_oracle::test {

    template <typename Tag>
    constexpr bool matches(const ReferenceCase& c) {
        return c.dimension == Tag::DIM && c.order == Tag::ORDER
               && c.reference.quadPointsPerAxis == Tag::QUAD_POINTS;
    }

    template <typename Scalar, typename Tag>
    struct Context {
        using T                       = Scalar;
        static constexpr unsigned DIM = Tag::DIM, ORDER = Tag::ORDER;
        using Handler = ippl::LagrangeDOFHandler<T, DIM, ORDER>;
        using Field   = typename Handler::FEMContainer_t;
        using Point   = ippl::Vector<T, DIM>;
        using Element =
            std::tuple_element_t<DIM - 1,
                                 std::tuple<ippl::EdgeElement<T>, ippl::QuadrilateralElement<T>,
                                            ippl::HexahedralElement<T>>>;
        using Quadrature = ippl::SelectableQuadrature<T, Tag::QUAD_POINTS, Element>;
        using Space = ippl::LagrangeSpace_wfc<T, DIM, ORDER, Element, Quadrature, Field, Field>;
        static constexpr unsigned N = Handler::dofsPerElement;

        static Point point(Reals values) {
            Point result;
            for (unsigned d = 0; d < DIM; ++d)
                result[d] = static_cast<T>(values[d]);
            return result;
        }
        static ippl::NDIndex<DIM> domain(const ReferenceCase& c, bool single) {
            ippl::NDIndex<DIM> result;
            for (unsigned d = 0; d < DIM; ++d)
                result[d] = ippl::Index(single ? 2 : c.mesh.cellsPerAxis[d] + 1);
            return result;
        }
        static Point spacing(const ReferenceCase& c) {
            Point result;
            for (unsigned d = 0; d < DIM; ++d)
                result[d] =
                    static_cast<T>((c.mesh.corner[d] - c.mesh.origin[d]) / c.mesh.cellsPerAxis[d]);
            return result;
        }
        static std::array<bool, DIM> parallelAxes() {
            std::array<bool, DIM> result;
            result.fill(true);
            return result;
        }
        explicit Context(const ReferenceCase& c, bool single = false)
            : domain_m(domain(c, single))
            , layout_m(MPI_COMM_WORLD, domain_m, parallelAxes(), false)
            , mesh_m(domain_m, spacing(c), point(c.mesh.origin))
            , quadrature_m(element_m)
            , space_m(mesh_m, element_m, quadrature_m, layout_m) {
            if constexpr (DIM == 3) {
                if (ippl::Comm->size() == 4) {
                    // Four-rank acceptance must exercise a partition corner.
                    const auto owned = layout_m.getLocalNDIndex();
                    std::array<int, DIM> first, low, high;
                    for (unsigned d = 0; d < DIM; ++d)
                        first[d] = owned[d].first();
                    MPI_Allreduce(first.data(), low.data(), DIM, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
                    MPI_Allreduce(first.data(), high.data(), DIM, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
                    unsigned splitAxes = 0;
                    for (unsigned d = 0; d < DIM; ++d)
                        splitAxes += low[d] != high[d];
                    EXPECT_GE(splitAxes, 2u);
                }
            }
            space_m.setInterpolationNodes(std::string_view(c.interpolationFamily) == "gll"
                                              ? ippl::LagrangeNodeFamily::GLL
                                              : ippl::LagrangeNodeFamily::Equispaced);
        }
        ippl::NDIndex<DIM> domain_m;
        ippl::FieldLayout<DIM> layout_m;
        ippl::UniformCartesian<T, DIM> mesh_m;
        Element element_m;
        Quadrature quadrature_m;
        Space space_m;

        Field field(ippl::FieldBC type = ippl::NO_FACE, T value = T(0), int ghosts = 1) {
            Field result(mesh_m, layout_m, ghosts);
            std::array<ippl::FieldBC, 2 * DIM> types;
            types.fill(type);
            std::array<T, 2 * DIM> values;
            values.fill(value);
            std::array<T, 2 * DIM> slopes{};
            result.setFieldBC(types, values, slopes);
            result = T(0);
            return result;
        }
        auto stiffness() const {
            Point inverse;
            T determinant = 1;
            for (unsigned d = 0; d < DIM; ++d) {
                inverse[d] = T(1) / mesh_m.getMeshSpacing(d);
                determinant *= mesh_m.getMeshSpacing(d);
            }
            return ippl::EvalFunctor<T, DIM, N>(inverse, determinant);
        }
        struct Mass {
            T determinant;
            KOKKOS_FUNCTION T
            operator()(const size_t& i, const size_t& j,
                       const ippl::RefShapeFunctionData<T, Point, N>& data) const {
                return determinant * data.val_q[i] * data.val_q[j];
            }
        };
        Mass mass() const { return {stiffness().absDetDPhi}; }

        // Owned-entity adapter, independent of DOFHandler local/global tables.
        // Entity::dir gives its free axes; offsets enumerate interiors x-fastest.
        // One host mirror per entity, with explicit copies at the device boundary.
        template <typename Operation>
        void visit(Field& field, bool write, Operation operation, bool poisonGhosts = false) const {
            auto entity = [&]<typename Entity>() {
                auto view = field.template getView<Entity>();
                auto host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), view);
                if (poisonGhosts) {
                    for (size_t j = 0; j < host.size(); ++j)
                        for (unsigned k = 0;
                             k < Handler::SpaceTraits::template entityDOFCount<Entity>(); ++k)
                            host.data()[j][k] = j % 2 ? T(-731) : T(731);
                }
                const auto owned = field.template getLayout<Entity>().getLocalNDIndex();
                size_t entries   = 1;
                for (unsigned d = 0; d < DIM; ++d)
                    entries *= owned[d].length();
                for (size_t entry = 0; entry < entries; ++entry) {
                    ippl::Vector<size_t, DIM> nd;
                    size_t remaining = entry;
                    for (unsigned d = 0; d < DIM; ++d) {
                        nd[d] = remaining % owned[d].length();
                        remaining /= owned[d].length();
                    }
                    for (unsigned offset = 0;
                         offset < Handler::SpaceTraits::template entityDOFCount<Entity>();
                         ++offset) {
                        size_t global = 0, stride = 1, interior = offset;
                        for (unsigned d = 0; d < DIM; ++d) {
                            size_t axis = (nd[d] + owned[d].first()) * ORDER;
                            if constexpr (ORDER > 1) {
                                if (Entity::dir[d]) {
                                    axis += 1 + interior % (ORDER - 1);
                                    interior /= ORDER - 1;
                                }
                            }
                            global += axis * stride;
                            stride *= (domain_m[d].length() - 1) * ORDER + 1;
                        }
                        auto local = nd;
                        for (unsigned d = 0; d < DIM; ++d)
                            local[d] += field.getNghost();
                        operation(global, ippl::apply(host, local)[offset]);
                    }
                }
                if (write)
                    Kokkos::deep_copy(view, host);
            };
            [&]<typename... Entities>(std::tuple<Entities...>) {
                (entity.template operator()<Entities>(), ...);
            }(typename Handler::EntityTypes{});
        }
        void scatter(Field& field, Reals values, bool poisonGhosts = false) const {
            ASSERT_EQ(values.size(), space_m.numGlobalDOFs());
            visit(
                field, true,
                [&](size_t i, T& value) {
                    value = static_cast<T>(values[i]);
                },
                poisonGhosts);
        }
        std::vector<double> gather(Field& field) const {
            std::vector<double> values(space_m.numGlobalDOFs());
            std::vector<unsigned> seen(values.size());
            visit(field, false, [&](size_t i, T& value) {
                values[i] = value;
                ++seen[i];
            });
            // Replicate only owned values and verify unique ownership independently
            // of halo contents. Every rank enters both collectives before assertions.
            MPI_Allreduce(MPI_IN_PLACE, values.data(), static_cast<int>(values.size()), MPI_DOUBLE,
                          MPI_SUM, MPI_COMM_WORLD);
            MPI_Allreduce(MPI_IN_PLACE, seen.data(), static_cast<int>(seen.size()), MPI_UNSIGNED,
                          MPI_SUM, MPI_COMM_WORLD);
            for (auto count : seen)
                EXPECT_EQ(count, 1u);
            return values;
        }
        void fillProduction(Field& field, Reals values) {
            Kokkos::View<double*> coefficients("oracle input", values.size());
            auto host = Kokkos::create_mirror_view(coefficients);
            for (size_t i = 0; i < values.size(); ++i)
                host(i) = static_cast<T>(values[i]);
            Kokkos::deep_copy(coefficients, host);
            space_m.fillFromGlobalCoefficients(field, coefficients);
            field.fillHalo();
        }
    };

    inline std::vector<double> apply(const SparseMatrix& matrix, Reals x, bool absolute = false) {
        std::vector<double> result(matrix.rows);
        for (size_t row = 0; row < matrix.rows; ++row)
            for (size_t j = matrix.rowOffsets[row]; j < matrix.rowOffsets[row + 1]; ++j) {
                const double term = matrix.values[j] * x[matrix.columns[j]];
                result[row] += absolute ? std::abs(term) : term;
            }
        return result;
    }

    // Summation allowance uses the magnitude of the terms, not a relative error at zero.
    template <typename T, unsigned Q>
    void compare(Reals actual, Reals expected, Reals scale = {}) {
        ASSERT_EQ(actual.size(), expected.size());
        constexpr double REL =
            std::is_same_v<T, float> ? 4 * std::numeric_limits<float>::epsilon() * Q : 1e-10;
        constexpr double ABS = std::is_same_v<T, float> ? 1e-6 : 2e-12;
        for (size_t i = 0; i < actual.size(); ++i) {
            SCOPED_TRACE("global/local DOF " + std::to_string(i));
            EXPECT_NEAR(actual[i], expected[i],
                        ABS + REL * (scale.empty() ? std::abs(expected[i]) : scale[i]));
        }
    }

    template <typename T, unsigned Dim>
    struct Exact {
        T operator()(const ippl::Vector<T, Dim>& x) const {
            T value = 1;
            for (unsigned d = 0; d < Dim; ++d)
                value += T(d + 1) * x[d] * (T(1) - x[d]) * (T(1) + T(2) * x[d]);
            return value;
        }
    };
    template <typename T, unsigned Dim>
    struct Zero {
        T operator()(const ippl::Vector<T, Dim>&) const { return 0; }
    };

}  // namespace fem_oracle::test
