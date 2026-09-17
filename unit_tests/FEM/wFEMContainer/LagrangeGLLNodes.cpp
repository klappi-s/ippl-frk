#include "Ippl.h"

#include "FEM/DOFLocations.h"
#include "FEM/LagrangeSpace_wFEMContainer.h"
#include "FEM/Quadrature/SelectableQuadrature.h"
#include "PoissonSolvers/FEMPoissonSolver_wFEMContainer.h"
#include "TestUtils.h"
#include "gtest/gtest.h"

using T = double;

TEST(LagrangeGLL, FillNodes1DEndpointsAndOrdering) {
    constexpr unsigned Order = 3;
    ippl::LagrangeDOFLocations<T, 1, Order> eqLocs;
    eqLocs.setFromFamily(ippl::LagrangeNodeFamily::Equispaced);
    ippl::LagrangeDOFLocations<T, 1, Order> gllLocs;
    gllLocs.setFromFamily(ippl::LagrangeNodeFamily::GLL);

    const auto& eq  = eqLocs.nodes1d_m;
    const auto& gll = gllLocs.nodes1d_m;
    EXPECT_DOUBLE_EQ(eq[0], 0.0);
    EXPECT_DOUBLE_EQ(eq[3], 1.0);
    EXPECT_DOUBLE_EQ(gll[0], 0.0);
    EXPECT_DOUBLE_EQ(gll[3], 1.0);
    EXPECT_NEAR(eq[1], 1.0 / 3.0, 1e-14);
    EXPECT_NEAR(eq[2], 2.0 / 3.0, 1e-14);
    // GLL interior clustered toward ends vs equispaced.
    EXPECT_LT(gll[1], eq[1]);
    EXPECT_GT(gll[2], eq[2]);
}

TEST(LagrangeGLL, DOFLocationsKroneckerP2_1D) {
    constexpr unsigned Order = 2;
    ippl::LagrangeDOFLocations<T, 1, Order> locs;
    locs.setFromFamily(ippl::LagrangeNodeFamily::GLL);

    for (unsigned i = 0; i < Order + 1; ++i) {
        for (unsigned j = 0; j < Order + 1; ++j) {
            T val = T(1);
            for (unsigned k = 0; k <= Order; ++k) {
                const T nk = locs.nodes1d_m[k];
                if (Kokkos::abs(locs.nodes1d_m[i] - nk) < 1e-10) {
                    continue;
                }
                val *= (locs.nodes1d_m[j] - nk) / (locs.nodes1d_m[i] - nk);
            }
            if (i == j) {
                EXPECT_NEAR(val, 1.0, 1e-12);
            } else {
                EXPECT_NEAR(val, 0.0, 1e-12);
            }
        }
    }
}

TEST(LagrangeGLL, SelectableQuadratureFamilies) {
    ippl::EdgeElement<T> ref;
    ippl::SelectableQuadrature<T, 5, ippl::EdgeElement<T>> q(ref);

    q.setFamily(ippl::QuadratureNodeFamily::GaussLegendre);
    auto w_gl = q.getWeights1D(0.0, 1.0);
    T sum_gl  = 0;
    for (unsigned i = 0; i < 5; ++i) {
        sum_gl += w_gl[i];
    }
    EXPECT_NEAR(sum_gl, 1.0, 1e-12);

    q.setFamily(ippl::QuadratureNodeFamily::GaussLobatto);
    auto w_gll = q.getWeights1D(0.0, 1.0);
    auto x_gll = q.getIntegrationNodes1D(0.0, 1.0);
    T sum_gll  = 0;
    for (unsigned i = 0; i < 5; ++i) {
        sum_gll += w_gll[i];
    }
    EXPECT_NEAR(sum_gll, 1.0, 1e-12);
    EXPECT_NEAR(x_gll[0], 0.0, 1e-14);
    EXPECT_NEAR(x_gll[4], 1.0, 1e-14);

    q.setFamily(ippl::QuadratureNodeFamily::Midpoint);
    auto w_m = q.getWeights1D(0.0, 1.0);
    T sum_m  = 0;
    for (unsigned i = 0; i < 5; ++i) {
        sum_m += w_m[i];
    }
    EXPECT_NEAR(sum_m, 1.0, 1e-12);
}

TEST(LagrangeGLL, SpaceShapeKroneckerWithGLL) {
    constexpr unsigned Dim   = 1;
    constexpr unsigned Order = 2;
    using MeshType           = ippl::UniformCartesian<T, Dim>;
    using ElementType        = ippl::EdgeElement<T>;
    using QuadType = ippl::SelectableQuadrature<T, 3, ElementType>;
    using DOFHandler_t =
        ippl::DOFHandler<T, ippl::FiniteElementSpaceTraits<ippl::LagrangeSpaceTag, Dim, Order>>;
    using FieldType = typename DOFHandler_t::FEMContainer_t;
    using Space =
        ippl::LagrangeSpace_wfc<T, Dim, Order, ElementType, QuadType, FieldType, FieldType>;

    ippl::NDIndex<Dim> domain(ippl::Index(5));
    ippl::FieldLayout<Dim> layout(MPI_COMM_WORLD, domain, {true}, {false});
    MeshType mesh(domain, ippl::Vector<T, Dim>(1.0), ippl::Vector<T, Dim>(0.0));
    ElementType ref;
    QuadType quad(ref, ippl::QuadratureNodeFamily::GaussLobatto);
    Space space(mesh, ref, quad, layout);
    space.setInterpolationNodes(ippl::LagrangeNodeFamily::GLL);

    const auto& locs = space.getDOFLocations();
    for (unsigned i = 0; i < Order + 1; ++i) {
        for (unsigned j = 0; j < Order + 1; ++j) {
            const T phi = space.evaluateRefElementShapeFunction(i, locs[j]);
            if (i == j) {
                EXPECT_NEAR(phi, 1.0, 1e-11) << "i=" << i << " j=" << j;
            } else {
                EXPECT_NEAR(phi, 0.0, 1e-11) << "i=" << i << " j=" << j;
            }
        }
    }
}

// Deferred until mass/reaction assembly exists (see FEMPoissonSolver_wFEMContainer.h).
// TEST(LagrangeGLL, SolverParameterListMassShortcutFlag) {
//     constexpr unsigned Dim   = 1;
//     constexpr unsigned Order = 2;
//     using FieldType = typename ippl::DOFHandler<
//         T, ippl::FiniteElementSpaceTraits<ippl::LagrangeSpaceTag, Dim, Order>>::FEMContainer_t;
//     using Solver = ippl::FEMPoissonSolver_wFEMContainer<FieldType, FieldType, Order, Order + 1>;
//
//     ippl::NDIndex<Dim> domain(ippl::Index(4));
//     ippl::FieldLayout<Dim> layout(MPI_COMM_WORLD, domain, {true}, {false});
//     ippl::UniformCartesian<T, Dim> mesh(domain, ippl::Vector<T, Dim>(1.0),
//                                         ippl::Vector<T, Dim>(0.0));
//     FieldType lhs(mesh, layout), rhs(mesh, layout);
//
//     Solver solver(lhs, rhs);
//     EXPECT_FALSE(solver.usesGllMassShortcut());
//
//     ippl::ParameterList params;
//     params.add("interpolation_nodes", std::string("gll"));
//     params.add("quadrature_nodes", std::string("gll"));
//     solver.mergeParameters(params);
//     solver.configureNodeFamilies();
//     EXPECT_TRUE(solver.usesGllMassShortcut());
//
//     ippl::ParameterList back;
//     back.add("interpolation_nodes", std::string("equispaced"));
//     back.add("quadrature_nodes", std::string("gauss_legendre"));
//     solver.mergeParameters(back);
//     solver.configureNodeFamilies();
//     EXPECT_FALSE(solver.usesGllMassShortcut());
// }

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
