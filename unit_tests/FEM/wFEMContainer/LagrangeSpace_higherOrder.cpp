

#include "Ippl.h"

#include <functional>

#include "TestUtils.h"
#include "gtest/gtest.h"

// LagrangeSpace_wFEMContainer unit tests — higher order (P2+) (Track B).
//
// Split from LagrangeSpace_wFEMContainer.cpp.
//
// ACTIVE: P2 (order 2), 1D only — reference-element checks plus evaluateAx /
// evaluateLoadVector with hand-tabulated oracles.
//
// INACTIVE: P2 2D/3D, P3, and other assembly cases are kept as commented reference
// at the end of this file. Oracles for those cases are not set up yet; completing
// this file is an explicit TODO in TASK3_FEMContainer_REPORT.md.
//
// Typed over (precision, order=2, dim=1): 2 instantiations.
//
// | Test                         | MPI | Notes |
// |------------------------------|-----|-------|
// | evaluateRefElementShapeFunction | yes | Kronecker / partition of unity + P2 1D spots |
// | evaluateRefElementShapeFunctionGradient | yes | P2 1D analytic gradients |
// | evaluateAx                   | yes | P2 1D hand oracle on biggerMesh |
// | evaluateLoadVector           | yes | P2 1D hand oracle on symmetricMesh |
//
// evaluateAx pipeline per case: constant x -> evaluateLoadVector(x) -> evaluateAx(x,eval)
// (historical test design). Oracles are hand-computed for biggerMesh (5^d nodes) or
// symmetricMesh (evaluateLoadVector).

template <typename>
class LagrangeSpaceHigherOrderTest;

template <typename Tlhs, unsigned Dim, unsigned numElemDOFs>
struct EvalFunctor {
    const ippl::Vector<Tlhs, Dim> DPhiInvT;
    const Tlhs absDetDPhi;

    EvalFunctor(ippl::Vector<Tlhs, Dim> DPhiInvT, Tlhs absDetDPhi)
        : DPhiInvT(DPhiInvT)
        , absDetDPhi(absDetDPhi) {}

    KOKKOS_FUNCTION auto operator()(const size_t& i, const size_t& j,
                    const ippl::Vector<ippl::Vector<Tlhs, Dim>, numElemDOFs>& grad_b_q_k) const {
        return dot((DPhiInvT * grad_b_q_k[j]), (DPhiInvT * grad_b_q_k[i])).apply() * absDetDPhi;
    }
};

template <typename T, typename ExecSpace, unsigned Order, unsigned Dim>
class LagrangeSpaceHigherOrderTest<Parameters<T, ExecSpace, Rank<Order>, Rank<Dim>>> : public ::testing::Test {
protected:
    void SetUp() override {}

public:
    using value_t = T;
    static constexpr unsigned dim = Dim;

    static_assert(Dim == 1 || Dim == 2 || Dim == 3, "Dim must be 1, 2 or 3");

    using MeshType    = ippl::UniformCartesian<T, Dim>;
    using ElementType = std::conditional_t<
        Dim == 1, ippl::EdgeElement<T>,
        std::conditional_t<Dim == 2, ippl::QuadrilateralElement<T>, ippl::HexahedralElement<T>>>;

    using QuadratureType       = ippl::MidpointQuadrature<T, 1, ElementType>;
    using QuadratureType2      = ippl::MidpointQuadrature<T, 2, ElementType>;
    using QuadratureType3      = ippl::MidpointQuadrature<T, 3, ElementType>;
    using BetterQuadratureType = ippl::GaussLegendreQuadrature<T, 5, ElementType>;
    using DOFHandler_t         = ippl::DOFHandler<T, ippl::FiniteElementSpaceTraits<ippl::LagrangeSpaceTag, Dim, Order>>;
    using FieldType            = typename DOFHandler_t::FEMContainer_t;
    using BCType               = std::array<ippl::FieldBC, 2*Dim>;

    using LagrangeType = ippl::LagrangeSpace_wfc<T, Dim, Order, ElementType, QuadratureType, FieldType, FieldType>;
    using LagrangeType2 = ippl::LagrangeSpace_wfc<T, Dim, Order, ElementType, QuadratureType2, FieldType, FieldType>;
    using LagrangeType3 = ippl::LagrangeSpace_wfc<T, Dim, Order, ElementType, QuadratureType3, FieldType, FieldType>;
    using LagrangeTypeBetter = ippl::LagrangeSpace_wfc<T, Dim, Order, ElementType, BetterQuadratureType, FieldType, FieldType>;

    LagrangeSpaceHigherOrderTest()
        : ref_element()
        , mesh(ippl::NDIndex<Dim>(ippl::Vector<unsigned, Dim>(3)), ippl::Vector<T, Dim>(1.0),
               ippl::Vector<T, Dim>(0.0))
        , biggerMesh(ippl::NDIndex<Dim>(ippl::Vector<unsigned, Dim>(5)), ippl::Vector<T, Dim>(1.0),
                     ippl::Vector<T, Dim>(0.0))
        , symmetricMesh(ippl::NDIndex<Dim>(ippl::Vector<unsigned, Dim>(5)),
                        ippl::Vector<T, Dim>(0.5), ippl::Vector<T, Dim>(-1.0))
        , quadrature(ref_element)
        , quadrature2(ref_element)
        , quadrature3(ref_element)
        , betterQuadrature(ref_element)
        , lagrangeSpace(mesh, ref_element, quadrature,
                        ippl::FieldLayout<Dim>(MPI_COMM_WORLD,
                                               ippl::NDIndex<Dim>(ippl::Vector<unsigned, Dim>(3)),
                                               std::array<bool, Dim>{true}))
        , lagrangeSpaceBigger(
              biggerMesh, ref_element, quadrature,
              ippl::FieldLayout<Dim>(MPI_COMM_WORLD,
                                     ippl::NDIndex<Dim>(ippl::Vector<unsigned, Dim>(5)),
                                     std::array<bool, Dim>{true}))
        , lagrangeSpaceBigger2(
              biggerMesh, ref_element, quadrature2,
              ippl::FieldLayout<Dim>(MPI_COMM_WORLD,
                                     ippl::NDIndex<Dim>(ippl::Vector<unsigned, Dim>(5)),
                                     std::array<bool, Dim>{true}))
        , lagrangeSpaceBigger3(
              biggerMesh, ref_element, quadrature3,
              ippl::FieldLayout<Dim>(MPI_COMM_WORLD,
                                     ippl::NDIndex<Dim>(ippl::Vector<unsigned, Dim>(5)),
                                     std::array<bool, Dim>{true}))
        , symmetricLagrangeSpace(
              symmetricMesh, ref_element, betterQuadrature,
              ippl::FieldLayout<Dim>(MPI_COMM_WORLD,
                                     ippl::NDIndex<Dim>(ippl::Vector<unsigned, Dim>(5)),
                                     std::array<bool, Dim>{true})) {
        // fill the global reference DOFs
    }

    ElementType ref_element;
    MeshType mesh;
    MeshType biggerMesh;
    MeshType symmetricMesh;
    const QuadratureType quadrature;
    const QuadratureType2 quadrature2;
    const QuadratureType3 quadrature3;
    const BetterQuadratureType betterQuadrature;
    const LagrangeType lagrangeSpace;
    const LagrangeType lagrangeSpaceBigger;
    const LagrangeType2 lagrangeSpaceBigger2;
    const LagrangeType3 lagrangeSpaceBigger3;
    const LagrangeTypeBetter symmetricLagrangeSpace;
};

using Precisions = TestParams::Precisions;
using Spaces     = TestParams::Spaces;
using Orders     = TestParams::Ranks<2>;
using Dimensions = TestParams::Ranks<1>;
using Combos     = CreateCombinations<Precisions, Spaces, Orders, Dimensions>::type;
using Tests      = TestForTypes<Combos>::type;
TYPED_TEST_CASE(LagrangeSpaceHigherOrderTest, Tests);

/*

// TODO is this test and function even needed?
TYPED_TEST(LagrangeSpaceHigherOrderTest, numGlobalDOFs) {
    const auto& lagrangeSpace = this->lagrangeSpace;
    const std::size_t& dim    = lagrangeSpace.dim;
    const std::size_t& order  = lagrangeSpace.order;

    ASSERT_EQ(lagrangeSpace.numGlobalDOFs(), static_cast<std::size_t>(pow(3.0 * order, dim)));
}

// TODO is this test and function even needed?
TYPED_TEST(LagrangeSpaceHigherOrderTest, getLocalDOFIndex) {
    const auto& lagrangeSpace = this->lagrangeSpace;
    const std::size_t& dim    = lagrangeSpace.dim;
    const std::size_t& order  = lagrangeSpace.order;

    std::size_t localDOFIndex     = static_cast<unsigned>(-1);
    const std::size_t numElements = (1 << dim);
    const std::size_t numDOFs     = static_cast<unsigned>(pow(3, dim));

    std::vector<std::vector<unsigned>> globalElementDOFs;

    if (dim == 1) {
        globalElementDOFs = {// Element 0
                             {0, 1},
                             // Element 1
                             {1, 2}};
    } else if (dim == 2) {
        globalElementDOFs = {// Element 0
                             {0, 1, 4, 3},
                             // Element 1
                             {1, 2, 5, 4},
                             // Element 2
                             {3, 4, 7, 6},
                             // Element 3
                             {4, 5, 8, 7}};
    } else if (dim == 3) {
        globalElementDOFs = {// Element 0
                             {0, 1, 4, 3, 9, 10, 13, 12},
                             // Element 1
                             {1, 2, 5, 4, 10, 11, 14, 13},
                             // Element 2
                             {3, 4, 7, 6, 12, 13, 16, 15},
                             // Element 3
                             {4, 5, 8, 7, 13, 14, 17, 16},
                             // Element 4
                             {9, 10, 13, 12, 18, 19, 22, 21},
                             // Element 5
                             {10, 11, 14, 13, 19, 20, 23, 22},
                             // Element 6
                             {12, 13, 16, 15, 21, 22, 25, 24},
                             // Element 7
                             {13, 14, 17, 16, 22, 23, 26, 25}};
    } else {
        // This dimension was not handled
        FAIL();
    }

    if (order == 1) {
        for (std::size_t el_i = 0; el_i < numElements; el_i++) {
            for (std::size_t dof_i = 0; dof_i < numDOFs; dof_i++) {
                const auto it = std::find(globalElementDOFs[el_i].begin(),
                                          globalElementDOFs[el_i].end(), dof_i);

                const std::size_t index = it - globalElementDOFs[el_i].begin();

                try {
                    localDOFIndex = lagrangeSpace.getLocalDOFIndex(el_i, dof_i);
                } catch (std::exception& e) {
                    ASSERT_EQ(it, globalElementDOFs[el_i].end());
                }

                if (it != globalElementDOFs[el_i].end()) {
                    ASSERT_EQ(localDOFIndex, index);
                }
            }
        }
    } else {
        // This order was not handled
        FAIL();
    }
}

// TODO is this test and function even needed?
TYPED_TEST(LagrangeSpaceHigherOrderTest, getGlobalDOFIndex) {
    auto& lagrangeSpace      = this->lagrangeSpace;
    const std::size_t& dim   = lagrangeSpace.dim;
    const std::size_t& order = lagrangeSpace.order;

    if (order == 1) {
        if (dim == 1) {
            // start element
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(0, 0), 0);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(0, 1), 1);

            // end element
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(1, 0), 1);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(1, 1), 2);

        } else if (dim == 2) {
            // lower left element
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(0, 0), 0);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(0, 1), 1);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(0, 2), 4);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(0, 3), 3);

            // lower right element
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(1, 0), 1);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(1, 1), 2);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(1, 2), 5);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(1, 3), 4);

            // upper left element
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(2, 0), 3);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(2, 1), 4);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(2, 2), 7);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(2, 3), 6);

            // upper right element
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(3, 0), 4);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(3, 1), 5);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(3, 2), 8);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(3, 3), 7);
        } else if (dim == 3) {
            // lower left front element
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(0, 0), 0);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(0, 1), 1);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(0, 2), 4);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(0, 3), 3);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(0, 4), 9);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(0, 5), 10);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(0, 6), 13);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(0, 7), 12);

            // lower right front element
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(1, 0), 1);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(1, 1), 2);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(1, 2), 5);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(1, 3), 4);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(1, 4), 10);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(1, 5), 11);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(1, 6), 14);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(1, 7), 13);

            // upper left front element
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(2, 0), 3);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(2, 1), 4);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(2, 2), 7);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(2, 3), 6);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(2, 4), 12);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(2, 5), 13);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(2, 6), 16);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(2, 7), 15);

            // upper right front element
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(3, 0), 4);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(3, 1), 5);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(3, 2), 8);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(3, 3), 7);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(3, 4), 13);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(3, 5), 14);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(3, 6), 17);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(3, 7), 16);

            // lower left back element
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(4, 0), 9);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(4, 1), 10);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(4, 2), 13);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(4, 3), 12);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(4, 4), 18);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(4, 5), 19);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(4, 6), 22);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(4, 7), 21);

            // lower right back element
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(5, 0), 10);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(5, 1), 11);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(5, 2), 14);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(5, 3), 13);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(5, 4), 19);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(5, 5), 20);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(5, 6), 23);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(5, 7), 22);

            // upper left back element
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(6, 0), 12);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(6, 1), 13);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(6, 2), 16);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(6, 3), 15);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(6, 4), 21);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(6, 5), 22);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(6, 6), 25);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(6, 7), 24);

            // upper right back element
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(7, 0), 13);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(7, 1), 14);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(7, 2), 17);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(7, 3), 16);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(7, 4), 22);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(7, 5), 23);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(7, 6), 26);
            ASSERT_EQ(lagrangeSpace.getGlobalDOFIndex(7, 7), 25);
        } else {
            FAIL();
        }
    } else {
        FAIL();
    }
}

// TODO is this test and function even needed?
TYPED_TEST(LagrangeSpaceHigherOrderTest, getLocalDOFIndices) {
    const auto& lagrangeSpace = this->lagrangeSpace;
    // const auto& dim = lagrangeSpace.dim;
    // const auto& order = lagrangeSpace.order;
    const auto& numElementDOFs = lagrangeSpace.numElementDOFs;

    auto local_dof_indices = lagrangeSpace.getLocalDOFIndices();

    ASSERT_EQ(local_dof_indices.dim, numElementDOFs);
    for (unsigned i = 0; i < numElementDOFs; i++) {
        ASSERT_EQ(local_dof_indices[i], i);
    }
}

// TODO is this test and function even needed?
TYPED_TEST(LagrangeSpaceHigherOrderTest, getGlobalDOFIndices) {
    auto& lagrangeSpace      = this->lagrangeSpace;
    const std::size_t& dim   = lagrangeSpace.dim;
    const std::size_t& order = lagrangeSpace.order;

    if (dim == 1) {
        auto globalDOFIndices = lagrangeSpace.getGlobalDOFIndices(1);
        if (order == 1) {
            ASSERT_EQ(globalDOFIndices.dim, 2);
            ASSERT_EQ(globalDOFIndices[0], 1);
            ASSERT_EQ(globalDOFIndices[1], 2);
        } else if (order == 2) {
            ASSERT_EQ(globalDOFIndices[0], 3);
            ASSERT_EQ(globalDOFIndices[1], 5);
            ASSERT_EQ(globalDOFIndices[2], 4);
        } else {
            FAIL();
        }
    } else if (dim == 2) {
        auto globalDOFIndices = lagrangeSpace.getGlobalDOFIndices(3);

        if (order == 1) {
            ASSERT_EQ(globalDOFIndices.dim, 4);
            ASSERT_EQ(globalDOFIndices[0], 4);
            ASSERT_EQ(globalDOFIndices[1], 5);
            ASSERT_EQ(globalDOFIndices[2], 8);
            ASSERT_EQ(globalDOFIndices[3], 7);
        } else if (order == 2) {
            ASSERT_EQ(globalDOFIndices[0], 12);
            ASSERT_EQ(globalDOFIndices[1], 14);
            ASSERT_EQ(globalDOFIndices[2], 24);
            ASSERT_EQ(globalDOFIndices[3], 22);
        } else {
            FAIL();
        }
    } else if (dim == 3) {
        auto globalDOFIndices = lagrangeSpace.getGlobalDOFIndices(7);

        if (order == 1) {
            ASSERT_EQ(globalDOFIndices.dim, 8);
            ASSERT_EQ(globalDOFIndices[0], 13);
            ASSERT_EQ(globalDOFIndices[1], 14);
            ASSERT_EQ(globalDOFIndices[2], 17);
            ASSERT_EQ(globalDOFIndices[3], 16);
            ASSERT_EQ(globalDOFIndices[4], 22);
            ASSERT_EQ(globalDOFIndices[5], 23);
            ASSERT_EQ(globalDOFIndices[6], 26);
            ASSERT_EQ(globalDOFIndices[7], 25);
        } else if (order == 2) {
            ASSERT_EQ(globalDOFIndices[0], 48);
            ASSERT_EQ(globalDOFIndices[1], 50);
            ASSERT_EQ(globalDOFIndices[2], 56);
            ASSERT_EQ(globalDOFIndices[3], 54);
            ASSERT_EQ(globalDOFIndices[4], 72);
            ASSERT_EQ(globalDOFIndices[5], 74);
            ASSERT_EQ(globalDOFIndices[6], 80);
            ASSERT_EQ(globalDOFIndices[7], 78);
        } else {
            FAIL();
        }

    } else {
        FAIL();
    }
}

*/

TYPED_TEST(LagrangeSpaceHigherOrderTest, evaluateRefElementShapeFunction) {
    // Reference-element only (no mesh/MPI): Kronecker delta, partition of unity, spot checks.
    auto& lagrangeSpace      = this->lagrangeSpace;
    static constexpr std::size_t dim = TestFixture::dim;
    const std::size_t& order = lagrangeSpace.order;
    using T                  = typename TestFixture::value_t;

    T tolerance = std::numeric_limits<T>::epsilon() * 10.0;

    // Test 1: Kronecker delta property - basis function i equals 1 at node i, 0 at other nodes
    for (size_t i = 0; i < lagrangeSpace.numElementDOFs; ++i) {
        auto node_i = lagrangeSpace.getRefElementDOFLocation(i);

        for (size_t j = 0; j < lagrangeSpace.numElementDOFs; ++j) {
            T expected = (i == j) ? 1.0 : 0.0;
            T computed = lagrangeSpace.evaluateRefElementShapeFunction(j, node_i);
            ASSERT_NEAR(computed, expected, tolerance)
                << "Kronecker delta failed: Order=" << order << ", Dim=" << dim
                << ", basis " << j << " at node " << i;
        }
    }

    // Test 2: Partition of unity - sum of all basis functions equals 1
    if (dim == 1) {
        for (T x = 0.0; x <= 1.0; x += 0.05) {
            T sum = 0.0;
            for (size_t dof = 0; dof < lagrangeSpace.numElementDOFs; ++dof) {
                sum += lagrangeSpace.evaluateRefElementShapeFunction(dof, x);
            }
            ASSERT_NEAR(sum, 1.0, tolerance)
                << "Partition of unity failed at x=" << x;
        }
    } else if (dim == 2) {
        ippl::Vector<T, dim> point;
        for (T x = 0.0; x <= 1.0; x += 0.05) {
            point[0] = x;
            for (T y = 0.0; y <= 1.0; y += 0.05) {
                point[1] = y;
                T sum = 0.0;
                for (size_t dof = 0; dof < lagrangeSpace.numElementDOFs; ++dof) {
                    sum += lagrangeSpace.evaluateRefElementShapeFunction(dof, point);
                }
                ASSERT_NEAR(sum, 1.0, tolerance)
                    << "Partition of unity failed at (" << x << "," << y << ")";
            }
        }
    } else if (dim == 3) {
        ippl::Vector<T, dim> point;
        for (T x = 0.0; x <= 1.0; x += 0.05) {
            point[0] = x;
            for (T y = 0.0; y <= 1.0; y += 0.05) {
                point[1] = y;
                for (T z = 0.0; z <= 1.0; z += 0.05) {
                    point[2] = z;
                    T sum = 0.0;
                    for (size_t dof = 0; dof < lagrangeSpace.numElementDOFs; ++dof) {
                        sum += lagrangeSpace.evaluateRefElementShapeFunction(dof, point);
                    }
                    ASSERT_NEAR(sum, 1.0, tolerance)
                        << "Partition of unity failed at (" << x << "," << y << "," << z << ")";
                }
            }
        }
    } else {
        FAIL();
    }

    // Test 3: P2 1D quadratic spot checks (other orders/dims: commented reference at file end)
    if (order == 2) {
        // Order 2: Quadratic basis functions
        if (dim == 1) {
            // 3 DOFs: vertices at 0, 1 and edge DOF at 0.5
            // φ0(x) = 2x² - 3x + 1  (vertex at x=0)
            // φ1(x) = 2x² - x       (vertex at x=1)
            // φ2(x) = -4x² + 4x     (edge at x=0.5)

            T x = 0.25;
            ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(0, x),
                       2.0*x*x - 3.0*x + 1.0, tolerance)
                << "Order 2, 1D: φ0(0.25) mismatch";
            ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(1, x),
                       2.0*x*x - x, tolerance)
                << "Order 2, 1D: φ1(0.25) mismatch";
            ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(2, x),
                       -4.0*x*x + 4.0*x, tolerance)
                << "Order 2, 1D: φ2(0.25) mismatch (edge DOF)";

            // Test at x = 0.5 (edge DOF location)
            x = 0.5;
            ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(0, x), 0.0, tolerance)
                << "Order 2, 1D: φ0(0.5) should be 0";
            ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(1, x), 0.0, tolerance)
                << "Order 2, 1D: φ1(0.5) should be 0";
            ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(2, x), 1.0, tolerance)
                << "Order 2, 1D: φ2(0.5) should be 1 (Kronecker delta)";
        }
    }
}

TYPED_TEST(LagrangeSpaceHigherOrderTest, evaluateRefElementShapeFunctionGradient) {
    // Reference-element gradients: P1 linear constants; P2 spot values at nodes/midpoints.
    auto& lagrangeSpace      = this->lagrangeSpace;
    static constexpr std::size_t dim = TestFixture::dim;
    const std::size_t& order = lagrangeSpace.order;
    using T                  = typename TestFixture::value_t;

    T tolerance = std::numeric_limits<T>::epsilon() * 10.0;

    if (order == 2) {
        // Order 2: Test specific gradient values for quadratic basis functions
        if (dim == 1) {
            // φ0(x) = 2x² - 3x + 1  =>  φ0'(x) = 4x - 3
            // φ1(x) = 2x² - x       =>  φ1'(x) = 4x - 1
            // φ2(x) = -4x² + 4x     =>  φ2'(x) = -8x + 4

            for (T x = 0.0; x <= 1.0; x += 0.05) {
                const auto grad_0 = lagrangeSpace.evaluateRefElementShapeFunctionGradient(0, x);
                const auto grad_1 = lagrangeSpace.evaluateRefElementShapeFunctionGradient(1, x);
                const auto grad_2 = lagrangeSpace.evaluateRefElementShapeFunctionGradient(2, x);

                ASSERT_NEAR(grad_0[0], 4.0*x - 3.0, tolerance)
                    << "Order 2, 1D: ∇φ0(" << x << ") mismatch";
                ASSERT_NEAR(grad_1[0], 4.0*x - 1.0, tolerance)
                    << "Order 2, 1D: ∇φ1(" << x << ") mismatch";
                ASSERT_NEAR(grad_2[0], -8.0*x + 4.0, tolerance)
                    << "Order 2, 1D: ∇φ2(" << x << ") mismatch (edge DOF)";
            }
        }
    } else {
        FAIL() << "LagrangeSpace_higherOrder: only P2 is instantiated";
    }
}

TYPED_TEST(LagrangeSpaceHigherOrderTest, evaluateAx) {
    // P2 1D only — matrix-free stiffness apply with hand oracle (biggerMesh).
    static constexpr std::size_t order = TestFixture::DOFHandler_t::SpaceTraits::Order;
    static_assert(order == 2, "LagrangeSpace_higherOrder is P2 only");

    {
        using T         = typename TestFixture::value_t;
        using FieldType = typename TestFixture::FieldType;
        using BCType    = typename TestFixture::BCType;
        using LagrangeType = typename TestFixture::LagrangeType2;
    
        const auto& refElement           = this->ref_element;
        const auto& lagrangeSpace        = this->lagrangeSpaceBigger2;
        auto mesh                        = this->biggerMesh;
        static constexpr std::size_t dim = TestFixture::dim;
    
        // create layout
        ippl::NDIndex<dim> domain(
            ippl::Vector<unsigned, dim>(mesh.getGridsize(0)));
    
        // specifies decomposition; here all dimensions are parallel
        std::array<bool, dim> isParallel;
        isParallel.fill(true);
    
        ippl::FieldLayout<dim> layout(MPI_COMM_WORLD, domain, isParallel);
    
        FieldType x(mesh, layout, 1);
        FieldType z(mesh, layout, 1);
    
        // Define boundary conditions
        BCType bcField;
        for (unsigned int i = 0; i < 2 * dim; ++i) {
            bcField[i] = ippl::ZERO_FACE;
        }
        x.setFieldBC(bcField);
        z.setFieldBC(bcField);
    
        // 1. Define the eval function for the evaluateAx function
    
        const ippl::Vector<std::size_t, dim> zeroNdIndex =
            ippl::Vector<std::size_t, dim>(0);
    
        // Inverse Transpose Transformation Jacobian
        const ippl::Vector<T, dim> DPhiInvT =
            refElement.getInverseTransposeTransformationJacobian(
                lagrangeSpace.getElementMeshVertexPoints(zeroNdIndex));
    
        // Absolute value of det Phi_K
        const T absDetDPhi = std::abs(refElement.getDeterminantOfTransformationJacobian(
            lagrangeSpace.getElementMeshVertexPoints(zeroNdIndex)));
    
        // Poisson equation eval function (based on the weak form)
        EvalFunctor<T, dim, LagrangeType::numElementDOFs> eval(DPhiInvT, absDetDPhi);
    
        std::cout << "Inverse Transpose Jacobian: ";
        for (unsigned int d = 0; d < dim; ++d) {
            std::cout << DPhiInvT[d] << " ";
        }
        std::cout << std::endl;
        std::cout << "Absolute Determinant of Jacobian: " << absDetDPhi << std::endl;

        {
            x = 2.0;

            x.fillHalo();
            lagrangeSpace.evaluateLoadVector(x);
            x.fillHalo();

            using VertexType1 = ippl::Vertex<dim>;
            using EdgeXType1 = ippl::EdgeX<dim>;

            auto ldom_vertex1 = x.template getLayout<VertexType1>().getLocalNDIndex();
            auto ldom_edge_x1 = x.template getLayout<EdgeXType1>().getLocalNDIndex();

            z = lagrangeSpace.evaluateAx(x, eval);
            z.fillHalo();

            // Set up for comparison
            FieldType ref_field_vertex(mesh, layout, 1);
            FieldType ref_field_edge_x(mesh, layout, 1);

            using VertexType = ippl::Vertex<dim>;
            using EdgeXType = ippl::EdgeX<dim>;

            auto view_ref_vertex = ref_field_vertex.template getView<VertexType>();
            auto view_ref_edge_x = ref_field_edge_x.template getView<EdgeXType>();

            auto mirror_vertex = Kokkos::create_mirror_view(view_ref_vertex);
            auto mirror_edge_x = Kokkos::create_mirror_view(view_ref_edge_x);

            auto ldom_vertex = ref_field_vertex.template getLayout<VertexType>().getLocalNDIndex();
            auto ldom_edge_x = ref_field_edge_x.template getLayout<EdgeXType>().getLocalNDIndex();

            // Vertex DOFs
            nestedViewLoop(mirror_vertex, 0, [&]<typename... Idx>(const Idx... args) {
                using index_type = std::tuple_element_t<0, std::tuple<Idx...>>;
                index_type coords[dim] = {args...};
                for (unsigned int d = 0; d < dim; ++d) {
                    coords[d] += ldom_vertex[d].first();
                }

                if ((coords[0] > 1) && (coords[0] < 5)) {
                    mirror_vertex(args...) = -4.0;
                } else {
                    mirror_vertex(args...) = 0.0;
                }
            });

            // Edge DOFs
            nestedViewLoop(mirror_edge_x, 0, [&]<typename... Idx>(const Idx... args) {
                using index_type = std::tuple_element_t<0, std::tuple<Idx...>>;
                index_type coords[dim] = {args...};
                for (unsigned int d = 0; d < dim; ++d) {
                    coords[d] += ldom_edge_x[d].first();
                }

                // Interior edge DOFs
                if ((coords[0] > 1) && (coords[0] < 4)) {
                    mirror_edge_x(args...) = 4.0;
                // Edges next to boundary
                } else if ((coords[0] == 1) || (coords[0] == 4)) {
                    mirror_edge_x(args...) = 5.0;
                }
                // Halo edge DOFs
                else {
                    mirror_edge_x(args...) = 0.0;
                }
            });

            Kokkos::fence();
            Kokkos::deep_copy(view_ref_vertex, mirror_vertex);
            Kokkos::deep_copy(view_ref_edge_x, mirror_edge_x);

            // Compare
            z = z - ref_field_vertex - ref_field_edge_x;
            double err = z.norm();
            ASSERT_NEAR(err, 0.0, 1e-6);

        }
    }
}

TYPED_TEST(LagrangeSpaceHigherOrderTest, evaluateLoadVector) {
    // P2 1D only — load assembly on symmetricMesh with hand oracle.
    using FieldType = typename TestFixture::FieldType;
    using BCType    = typename TestFixture::BCType;

    const auto& lagrangeSpace = this->symmetricLagrangeSpace;
    auto mesh                 = this->symmetricMesh;
    static constexpr std::size_t dim = TestFixture::dim;
    static constexpr std::size_t order = TestFixture::DOFHandler_t::SpaceTraits::Order;
    static_assert(order == 2, "LagrangeSpace_higherOrder is P2 only");

        // initialize the RHS field
        ippl::NDIndex<dim> domain(
            ippl::Vector<unsigned, dim>(mesh.getGridsize(0)));

        // specifies decomposition; here all dimensions are parallel
        std::array<bool, dim> isParallel;
        isParallel.fill(true);

        ippl::FieldLayout<dim> layout(MPI_COMM_WORLD, domain, isParallel);

        FieldType rhs_field(mesh, layout, 1);
        FieldType ref_field(mesh, layout, 1);

        // Define boundary conditions
        BCType bcField;
        for (unsigned int i = 0; i < 2 * dim; ++i) {
            bcField[i] = ippl::ZERO_FACE;
        }
        rhs_field.setFieldBC(bcField);

        {
            rhs_field = 2.75;

            // call evaluateLoadVector
            rhs_field.fillHalo();
            lagrangeSpace.evaluateLoadVector(rhs_field);
            rhs_field.fillHalo();

            // For order 2, we have vertices and edge midpoints
            // Expected values computed using quadrature integration
            using VertexType = ippl::Vertex<dim>;
            using EdgeXType   = ippl::EdgeX<dim>;

            auto view_ref_vertex = ref_field.template getView<VertexType>();
            auto view_ref_edge_x = ref_field.template getView<EdgeXType>();
            auto mirror_vertex   = Kokkos::create_mirror_view(view_ref_vertex);
            auto mirror_edge_x   = Kokkos::create_mirror_view(view_ref_edge_x);

            auto ldom_vertex = ref_field.template getLayout<VertexType>().getLocalNDIndex();
            auto ldom_edge_x = ref_field.template getLayout<EdgeXType>().getLocalNDIndex();

            // Set vertex values
            nestedViewLoop(mirror_vertex, 0, [&]<typename... Idx>(const Idx... args) {
                using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
                index_type coords[dim] = {args...};

                for (unsigned int d = 0; d < dim; ++d) {
                    coords[d] += ldom_vertex[d].first();
                }

                // Boundary vertices are zero
                if (coords[0] <= 1 || coords[0] >= 5) {
                    mirror_vertex(args...) = 0.0;
                } else {
                    // Interior vertices for order 2
                    mirror_vertex(args...) = 0.458333333333;  // Computed from quadrature
                }
            });

            // Set edge values
            nestedViewLoop(mirror_edge_x, 0, [&]<typename... Idx>(const Idx... args) {
                using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
                index_type coords[dim] = {args...};

                for (unsigned int d = 0; d < dim; ++d) {
                    coords[d] += ldom_edge_x[d].first();
                }

                // Edge midpoints (interior)
                mirror_edge_x(args...) = 0.916666666667;  // Computed from quadrature
            });

            Kokkos::fence();
            Kokkos::deep_copy(view_ref_vertex, mirror_vertex);
            Kokkos::deep_copy(view_ref_edge_x, mirror_edge_x);
            // compare values with reference
            rhs_field  = rhs_field - ref_field;
            double err = rhs_field.norm();

            ASSERT_NEAR(err, 0.0, 1e-6);
        }
}

/*
 * REFERENCE — inactive higher-order cases (not compiled / not run).
 * TODO: complete LagrangeSpace_higherOrder with valid oracles for these cases
 * (hand-tabulated or FEniCS-derived). See TASK3_FEMContainer_REPORT.md.
 */

//         } else if constexpr (dim == 2) {
//             // P2 2D evaluateAx — oracle not set up (see TASK3_FEMContainer_REPORT.md)
//         } else if constexpr (dim == 3) {
//             // P2 3D evaluateAx — oracle not set up
//         } else {
//             FAIL();
//         }
//     } else if constexpr (order == 3) {
//         // P3 evaluateAx — oracle not set up
//     } else {
//         GTEST_SKIP() << "Tests only implemented for order 1, 2, 3";
//     }
// }
// 
// 
//         } else if constexpr (dim == 2) {
//             rhs_field = 3.5;
// 
//             // call evaluateLoadVector
//             rhs_field.fillHalo();
//             lagrangeSpace.evaluateLoadVector(rhs_field);
//             rhs_field.fillHalo();
// 
//             // set up for comparison
//             using VertexType = ippl::Vertex<dim>;
//             auto view_ref = ref_field.template getView<VertexType>();
//             auto mirror   = Kokkos::create_mirror_view(view_ref);
// 
//             auto ldom     = ref_field.template getLayout<VertexType>().getLocalNDIndex();
// 
//             nestedViewLoop(mirror, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 // global coordinates
//                 for (unsigned int d = 0; d < lagrangeSpace.dim; ++d) {
//                     coords[d] += ldom[d].first();
//                 }
// 
//                 // reference field
//                 if ((coords[0] < 2) || (coords[1] < 2) || 
//                     (coords[0] > 4) || (coords[1] > 4)) {
//                     mirror(args...) = 0.0;
//                 } else {
//                     mirror(args...) = 0.875;
//                 }
//             });
//             Kokkos::fence();
// 
//             Kokkos::deep_copy(view_ref, mirror);
// 
//             // compare values with reference
//             rhs_field  = rhs_field - ref_field;
//             double err = rhs_field.norm();
// 
//             ASSERT_NEAR(err, 0.0, 1e-6);
// 
//         } else if constexpr (dim == 3) {
// 
//             rhs_field = 1.25;
// 
//             // call evaluateLoadVector
//             rhs_field.fillHalo();
//             lagrangeSpace.evaluateLoadVector(rhs_field);
//             rhs_field.fillHalo();
// 
//             // set up for comparison
//             using VertexType = ippl::Vertex<dim>;
//             auto view_ref = ref_field.template getView<VertexType>();
//             auto mirror   = Kokkos::create_mirror_view(view_ref);
// 
//             auto ldom     = ref_field.template getLayout<VertexType>().getLocalNDIndex();
// 
//             nestedViewLoop(mirror, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 // global coordinates
//                 for (unsigned int d = 0; d < lagrangeSpace.dim; ++d) {
//                     coords[d] += ldom[d].first();
//                 }
// 
//                 // reference field
//                 if ((coords[0] == 1) || (coords[1] == 1) || (coords[2] == 1) ||
//                     (coords[0] == 5) || (coords[1] == 5) || (coords[2] == 5)) {
//                     mirror(args...) = 0.0;
//                 } else {
//                     mirror(args...) = 0.15625;
//                 }
//             });
//             Kokkos::fence();
// 
//             Kokkos::deep_copy(view_ref, mirror);
// 
//             // compare values with reference
//             rhs_field  = rhs_field - ref_field;
//             double err = rhs_field.norm();
// 
//             ASSERT_NEAR(err, 0.0, 1e-6);
//         } else {
//             // only dims 1, 2, 3 supported
//             FAIL();
//         }
//     } else if constexpr (order == 2) {
//         // Higher order (Order 2) tests
// 
//         // initialize the RHS field
//         ippl::NDIndex<dim> domain(
//             ippl::Vector<unsigned, dim>(mesh.getGridsize(0)));
// 
//         // specifies decomposition; here all dimensions are parallel
//         std::array<bool, dim> isParallel;
//         isParallel.fill(true);
// 
//         ippl::FieldLayout<dim> layout(MPI_COMM_WORLD, domain, isParallel);
// 
//         FieldType rhs_field(mesh, layout, 1);
//         FieldType ref_field(mesh, layout, 1);
// 
//         // Define boundary conditions
//         BCType bcField;
//         for (unsigned int i = 0; i < 2 * dim; ++i) {
//             bcField[i] = ippl::ZERO_FACE;
//         }
//         rhs_field.setFieldBC(bcField);
// 
//         if constexpr (dim == 1) {
//             rhs_field = 2.75;
// 
//             // call evaluateLoadVector
//             rhs_field.fillHalo();
//             lagrangeSpace.evaluateLoadVector(rhs_field);
//             rhs_field.fillHalo();
// 
//             // For order 2, we have vertices and edge midpoints
//             // Expected values computed using quadrature integration
//             using VertexType = ippl::Vertex<dim>;
//             using EdgeXType   = ippl::EdgeX<dim>;
// 
//             auto view_ref_vertex = ref_field.template getView<VertexType>();
//             auto view_ref_edge_x = ref_field.template getView<EdgeXType>();
//             auto mirror_vertex   = Kokkos::create_mirror_view(view_ref_vertex);
//             auto mirror_edge_x   = Kokkos::create_mirror_view(view_ref_edge_x);
// 
//             auto ldom_vertex = ref_field.template getLayout<VertexType>().getLocalNDIndex();
//             auto ldom_edge_x = ref_field.template getLayout<EdgeXType>().getLocalNDIndex();
// 
//             // Set vertex values
//             nestedViewLoop(mirror_vertex, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_vertex[d].first();
//                 }
// 
//                 // Boundary vertices are zero
//                 if (coords[0] <= 1 || coords[0] >= 5) {
//                     mirror_vertex(args...) = 0.0;
//                 } else {
//                     // Interior vertices for order 2
//                     mirror_vertex(args...) = 0.458333333333;  // Computed from quadrature
//                 }
//             });
// 
//             // Set edge values
//             nestedViewLoop(mirror_edge_x, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_edge_x[d].first();
//                 }
// 
//                 // Edge midpoints (interior)
//                 mirror_edge_x(args...) = 0.916666666667;  // Computed from quadrature
//             });
// 
//             Kokkos::fence();
//             Kokkos::deep_copy(view_ref_vertex, mirror_vertex);
//             Kokkos::deep_copy(view_ref_edge_x, mirror_edge_x);
//             // compare values with reference
//             rhs_field  = rhs_field - ref_field;
//             double err = rhs_field.norm();
// 
//             ASSERT_NEAR(err, 0.0, 1e-6);
//         } else if constexpr (dim == 2) {
//             rhs_field = 3.5;
// 
//             // call evaluateLoadVector
//             rhs_field.fillHalo();
//             lagrangeSpace.evaluateLoadVector(rhs_field);
//             rhs_field.fillHalo();
// 
//             // For order 2 2D, we have vertices, edge midpoints and face midpoints
//             using VertexType = ippl::Vertex<dim>;
//             using EdgeXType   = ippl::EdgeX<dim>;
//             using EdgeYType   = ippl::EdgeY<dim>;
//             using FaceXYType  = ippl::FaceXY<dim>;
// 
//             auto view_ref_vertex = ref_field.template getView<VertexType>();
//             auto view_ref_edge_x = ref_field.template getView<EdgeXType>();
//             auto view_ref_edge_y = ref_field.template getView<EdgeYType>();
//             auto view_ref_face_xy= ref_field.template getView<FaceXYType>();
//             auto mirror_vertex   = Kokkos::create_mirror_view(view_ref_vertex);
//             auto mirror_edge_x   = Kokkos::create_mirror_view(view_ref_edge_x);
//             auto mirror_edge_y   = Kokkos::create_mirror_view(view_ref_edge_y);
//             auto mirror_face_xy  = Kokkos::create_mirror_view(view_ref_face_xy);
// 
//             auto ldom_vertex = ref_field.template getLayout<VertexType>().getLocalNDIndex();
//             auto ldom_edge_x = ref_field.template getLayout<EdgeXType>().getLocalNDIndex();
//             auto ldom_edge_y = ref_field.template getLayout<EdgeYType>().getLocalNDIndex();
//             auto ldom_face_xy= ref_field.template getLayout<FaceXYType>().getLocalNDIndex();
// 
//             // Set vertex values
//             nestedViewLoop(mirror_vertex, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_vertex[d].first();
//                 }
// 
//                 // Boundary vertices are zero
//                 if ((coords[0] < 2) || (coords[1] < 2) ||
//                     (coords[0] > 4) || (coords[1] > 4)) {
//                     mirror_vertex(args...) = 0.0;
//                 } else {
//                     mirror_vertex(args...) = 0.097222222222;  // Computed from quadrature
//                 }
//             });
// 
//             // Set edge values
//             nestedViewLoop(mirror_edge_x, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_edge_x[d].first();
//                 }
// 
//                 // Interior edge midpoints
//                 if ((coords[1] < 2) || (coords[1] > 4)) {
//                     mirror_edge_x(args...) = 0.0;
//                 } else {
//                     mirror_edge_x(args...) = 0.194444444444;  // Computed from quadrature
//                 }
//             });
// 
//             nestedViewLoop(mirror_edge_y, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_edge_y[d].first();
//                 }
// 
//                 // Interior edge midpoints
//                 if ((coords[0] < 2) || (coords[0] > 4)) {
//                     mirror_edge_y(args...) = 0.0;
//                 } else {
//                     // Try using the single element contribution without multipliers
//                     mirror_edge_y(args...) = 0.194444444444;  // Computed from quadrature
//                 }
//             });
// 
//             // Set face values
//             nestedViewLoop(mirror_face_xy, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_face_xy[d].first();
//                 }
//                 // Interior face midpoints
//                 mirror_face_xy(args...) = 0.388888888889;  // Computed from quadrature
//             });
// 
//             Kokkos::fence();
//             Kokkos::deep_copy(view_ref_vertex, mirror_vertex);
//             Kokkos::deep_copy(view_ref_edge_x, mirror_edge_x);
//             Kokkos::deep_copy(view_ref_edge_y, mirror_edge_y);
//             Kokkos::deep_copy(view_ref_face_xy, mirror_face_xy);
//             
//             // compare values with reference
//             rhs_field  = rhs_field - ref_field;
//             double err = rhs_field.norm();
// 
//             ASSERT_NEAR(err, 0.0, 1e-6);
//         } else if constexpr (dim == 3) {
//             rhs_field = 1.25;
// 
//             // call evaluateLoadVector
//             rhs_field.fillHalo();
//             lagrangeSpace.evaluateLoadVector(rhs_field);
//             rhs_field.fillHalo();
// 
//             // For order 2 3D, we have vertices, edge midpoints, face midpoints and hexahedron centers
//             using VertexType = ippl::Vertex<dim>;
//             using EdgeXType   = ippl::EdgeX<dim>;
//             using EdgeYType   = ippl::EdgeY<dim>;
//             using EdgeZType   = ippl::EdgeZ<dim>;
//             using FaceXYType  = ippl::FaceXY<dim>;
//             using FaceXZType  = ippl::FaceXZ<dim>;
//             using FaceYZType  = ippl::FaceYZ<dim>;
//             using HexahedronType = ippl::Hexahedron<dim>;
// 
//             auto view_ref_vertex = ref_field.template getView<VertexType>();
//             auto view_ref_edge_x = ref_field.template getView<EdgeXType>();
//             auto view_ref_edge_y = ref_field.template getView<EdgeYType>();
//             auto view_ref_edge_z = ref_field.template getView<EdgeZType>();
//             auto view_ref_face_xy= ref_field.template getView<FaceXYType>();
//             auto view_ref_face_xz= ref_field.template getView<FaceXZType>();
//             auto view_ref_face_yz= ref_field.template getView<FaceYZType>();
//             auto view_ref_hexahedron = ref_field.template getView<HexahedronType>();
//             auto mirror_vertex   = Kokkos::create_mirror_view(view_ref_vertex);
//             auto mirror_edge_x   = Kokkos::create_mirror_view(view_ref_edge_x);
//             auto mirror_edge_y   = Kokkos::create_mirror_view(view_ref_edge_y);
//             auto mirror_edge_z   = Kokkos::create_mirror_view(view_ref_edge_z);
//             auto mirror_face_xy  = Kokkos::create_mirror_view(view_ref_face_xy);
//             auto mirror_face_xz  = Kokkos::create_mirror_view(view_ref_face_xz);
//             auto mirror_face_yz  = Kokkos::create_mirror_view(view_ref_face_yz);
//             auto mirror_hexahedron = Kokkos::create_mirror_view(view_ref_hexahedron);
// 
//             auto ldom_vertex = ref_field.template getLayout<VertexType>().getLocalNDIndex();
//             auto ldom_edge_x = ref_field.template getLayout<EdgeXType>().getLocalNDIndex();
//             auto ldom_edge_y = ref_field.template getLayout<EdgeYType>().getLocalNDIndex();
//             auto ldom_edge_z = ref_field.template getLayout<EdgeZType>().getLocalNDIndex();
//             auto ldom_face_xy= ref_field.template getLayout<FaceXYType>().getLocalNDIndex();
//             auto ldom_face_xz= ref_field.template getLayout<FaceXZType>().getLocalNDIndex();
//             auto ldom_face_yz= ref_field.template getLayout<FaceYZType>().getLocalNDIndex();
//             auto ldom_hexahedron = ref_field.template getLayout<HexahedronType>().getLocalNDIndex();
// 
//             // Set vertex values
//             nestedViewLoop(mirror_vertex, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_vertex[d].first();
//                 }
// 
//                 // Boundary vertices are zero
//                 if ((coords[0] == 1) || (coords[1] == 1) || (coords[2] == 1) ||
//                     (coords[0] == 5) || (coords[1] == 5) || (coords[2] == 5)) {
//                     mirror_vertex(args...) = 0.0;
//                 } else {
//                     mirror_vertex(args...) = 0.005787037037;  // Computed from quadrature
//                 }
//             });
// 
//             // Set edge values
//             nestedViewLoop(mirror_edge_x, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_edge_x[d].first();
//                 }
// 
//                 // Interior edge midpoints
//                 if ((coords[1] == 1) || (coords[2] == 1) ||
//                     (coords[1] == 5) || (coords[2] == 5)) {
//                     mirror_edge_x(args...) = 0.0;
//                 } else {
//                     mirror_edge_x(args...) = 0.011574074074;  // Computed from quadrature
//                 }
//             });
// 
//             nestedViewLoop(mirror_edge_y, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_edge_y[d].first();
//                 }
// 
//                 // Interior edge midpoints
//                 if ((coords[0] == 1) || (coords[2] == 1) ||
//                     (coords[0] == 5) || (coords[2] == 5)) {
//                     mirror_edge_y(args...) = 0.0;
//                 } else {
//                     mirror_edge_y(args...) = 0.011574074074;  // Computed from quadrature
//                 }
//             });
// 
//             nestedViewLoop(mirror_edge_z, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_edge_z[d].first();
//                 }
// 
//                 // Interior edge midpoints
//                 if ((coords[0] == 1) || (coords[1] == 1) ||
//                     (coords[0] == 5) || (coords[1] == 5)) {
//                     mirror_edge_z(args...) = 0.0;
//                 } else {
//                     mirror_edge_z(args...) = 0.011574074074;  // Computed from quadrature
//                 }
//             });
// 
//             // Set face values
//             nestedViewLoop(mirror_face_xy, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_face_xy[d].first();
//                 }
//                 // Interior face midpoints
//                 if ((coords[2] == 1) || (coords[2] == 5)) {
//                     mirror_face_xy(args...) = 0.0;
//                 } else {
//                     mirror_face_xy(args...) = 0.023148148148;  // Computed from quadrature
//                 }
//             });
// 
//             nestedViewLoop(mirror_face_xz, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_face_xz[d].first();
//                 }
//                 // Interior face midpoints
//                 if ((coords[1] == 1) || (coords[1] == 5)) {
//                     mirror_face_xz(args...) = 0.0;
//                 } else {
//                     mirror_face_xz(args...) = 0.023148148148;  // Computed from quadrature
//                 }
//             });
// 
//             nestedViewLoop(mirror_face_yz, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_face_yz[d].first();
//                 }
//                 // Interior face midpoints
//                 if ((coords[0] == 1) || (coords[0] == 5)) {
//                     mirror_face_yz(args...) = 0.0;
//                 } else {
//                     mirror_face_yz(args...) = 0.023148148148;  // Computed from quadrature
//                 }
//             });
// 
//             // Set hexahedron center values
//             nestedViewLoop(mirror_hexahedron, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_hexahedron[d].first();
//                 }
//                 // Interior hexahedron centers
//                 mirror_hexahedron(args...) = 0.046296296296;  // Computed from quadrature
//             });
// 
//             Kokkos::fence();
//             Kokkos::deep_copy(view_ref_vertex, mirror_vertex);
//             Kokkos::deep_copy(view_ref_edge_x, mirror_edge_x);
//             Kokkos::deep_copy(view_ref_edge_y, mirror_edge_y);
//             Kokkos::deep_copy(view_ref_edge_z, mirror_edge_z);
//             Kokkos::deep_copy(view_ref_face_xy, mirror_face_xy);
//             Kokkos::deep_copy(view_ref_face_xz, mirror_face_xz);
//             Kokkos::deep_copy(view_ref_face_yz, mirror_face_yz);
//             Kokkos::deep_copy(view_ref_hexahedron, mirror_hexahedron);
// 
//             // compare values with reference
//             rhs_field  = rhs_field - ref_field;
//             double err = rhs_field.norm();
// 
//             ASSERT_NEAR(err, 0.0, 1e-6);
//         } else {
//             FAIL();
//         }
//     } else if constexpr (order == 3) {
//         // Higher order (Order 3) tests
// 
//         // initialize the RHS field
//         ippl::NDIndex<dim> domain(
//             ippl::Vector<unsigned, dim>(mesh.getGridsize(0)));
// 
//         // specifies decomposition; here all dimensions are parallel
//         std::array<bool, dim> isParallel;
//         isParallel.fill(true);
// 
//         ippl::FieldLayout<dim> layout(MPI_COMM_WORLD, domain, isParallel);
// 
//         FieldType rhs_field(mesh, layout, 1);
//         FieldType ref_field(mesh, layout, 1);
// 
//         // Define boundary conditions
//         BCType bcField;
//         for (unsigned int i = 0; i < 2 * dim; ++i) {
//             bcField[i] = ippl::ZERO_FACE;
//         }
//         rhs_field.setFieldBC(bcField);
// 
//         if constexpr (dim == 1) {
//             rhs_field = 2.75;
// 
//             // call evaluateLoadVector
//             rhs_field.fillHalo();
//             lagrangeSpace.evaluateLoadVector(rhs_field);
//             rhs_field.fillHalo();
// 
//             // For order 3 1D: vertices + 2 edge DOFs per element
//             using VertexType = ippl::Vertex<dim>;
//             using EdgeXType  = ippl::EdgeX<dim>;
// 
//             auto view_ref_vertex = ref_field.template getView<VertexType>();
//             auto view_ref_edge_x = ref_field.template getView<EdgeXType>();
//             auto mirror_vertex   = Kokkos::create_mirror_view(view_ref_vertex);
//             auto mirror_edge_x   = Kokkos::create_mirror_view(view_ref_edge_x);
// 
//             auto ldom_vertex = ref_field.template getLayout<VertexType>().getLocalNDIndex();
//             auto ldom_edge_x = ref_field.template getLayout<EdgeXType>().getLocalNDIndex();
// 
//             // Set vertex values
//             nestedViewLoop(mirror_vertex, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_vertex[d].first();
//                 }
// 
//                 // Boundary vertices are zero
//                 if (coords[0] == 1 || coords[0] == 5) {
//                     mirror_vertex(args...) = 0.0;
//                 } else {
//                     mirror_vertex(args...) = 0.343750000000;  // From calculator
//                 }
//             });
// 
//             // Set edge values - Order 3 has 2 edge DOFs per element
//             // Both edge DOFs have the same value for constant field
//             nestedViewLoop(mirror_edge_x, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_edge_x[d].first();
//                 }
// 
//                 // All edge DOFs are interior for 1D
//                 mirror_edge_x(args...) = 0.515625000000;  // From calculator
//             });
// 
//             Kokkos::fence();
//             Kokkos::deep_copy(view_ref_vertex, mirror_vertex);
//             Kokkos::deep_copy(view_ref_edge_x, mirror_edge_x);
// 
//             // compare values with reference
//             rhs_field  = rhs_field - ref_field;
//             double err = rhs_field.norm();
// 
//             ASSERT_NEAR(err, 0.0, 1e-6);
//         } else if constexpr (dim == 2) {
//             rhs_field = 3.5;
// 
//             // call evaluateLoadVector
//             rhs_field.fillHalo();
//             lagrangeSpace.evaluateLoadVector(rhs_field);
//             rhs_field.fillHalo();
// 
//             // For order 3 2D: vertices + edge DOFs (2 per edge) + face DOFs (4 per face)
//             using VertexType = ippl::Vertex<dim>;
//             using EdgeXType  = ippl::EdgeX<dim>;
//             using EdgeYType  = ippl::EdgeY<dim>;
//             using FaceXYType = ippl::FaceXY<dim>;
// 
//             auto view_ref_vertex  = ref_field.template getView<VertexType>();
//             auto view_ref_edge_x  = ref_field.template getView<EdgeXType>();
//             auto view_ref_edge_y  = ref_field.template getView<EdgeYType>();
//             auto view_ref_face_xy = ref_field.template getView<FaceXYType>();
//             auto mirror_vertex    = Kokkos::create_mirror_view(view_ref_vertex);
//             auto mirror_edge_x    = Kokkos::create_mirror_view(view_ref_edge_x);
//             auto mirror_edge_y    = Kokkos::create_mirror_view(view_ref_edge_y);
//             auto mirror_face_xy   = Kokkos::create_mirror_view(view_ref_face_xy);
// 
//             auto ldom_vertex  = ref_field.template getLayout<VertexType>().getLocalNDIndex();
//             auto ldom_edge_x  = ref_field.template getLayout<EdgeXType>().getLocalNDIndex();
//             auto ldom_edge_y  = ref_field.template getLayout<EdgeYType>().getLocalNDIndex();
//             auto ldom_face_xy = ref_field.template getLayout<FaceXYType>().getLocalNDIndex();
// 
//             // Set vertex values
//             nestedViewLoop(mirror_vertex, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_vertex[d].first();
//                 }
// 
//                 // Boundary vertices are zero
//                 if ((coords[0] < 2) || (coords[1] < 2) ||
//                     (coords[0] > 4) || (coords[1] > 4)) {
//                     mirror_vertex(args...) = 0.0;
//                 } else {
//                     mirror_vertex(args...) = 0.054687500000;  // From calculator
//                 }
//             });
// 
//             // Set EdgeX values - check only y-boundary
//             nestedViewLoop(mirror_edge_x, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_edge_x[d].first();
//                 }
// 
//                 // EdgeX extends in x-direction, so check y boundaries
//                 if ((coords[1] < 2) || (coords[1] > 4)) {
//                     mirror_edge_x(args...) = 0.0;
//                 } else {
//                     mirror_edge_x(args...) = 0.082031250000;  // From calculator
//                 }
//             });
// 
//             // Set EdgeY values - check only x-boundary
//             nestedViewLoop(mirror_edge_y, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_edge_y[d].first();
//                 }
// 
//                 // EdgeY extends in y-direction, so check x boundaries
//                 if ((coords[0] < 2) || (coords[0] > 4)) {
//                     mirror_edge_y(args...) = 0.0;
//                 } else {
//                     mirror_edge_y(args...) = 0.082031250000;  // From calculator
//                 }
//             });
// 
//             // Set FaceXY values - all interior
//             nestedViewLoop(mirror_face_xy, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_face_xy[d].first();
//                 }
// 
//                 // All face DOFs are interior for 2D
//                 mirror_face_xy(args...) = 0.123046875000;  // From calculator
//             });
// 
//             Kokkos::fence();
//             Kokkos::deep_copy(view_ref_vertex, mirror_vertex);
//             Kokkos::deep_copy(view_ref_edge_x, mirror_edge_x);
//             Kokkos::deep_copy(view_ref_edge_y, mirror_edge_y);
//             Kokkos::deep_copy(view_ref_face_xy, mirror_face_xy);
// 
//             // compare values with reference
//             rhs_field  = rhs_field - ref_field;
//             double err = rhs_field.norm();
// 
//             ASSERT_NEAR(err, 0.0, 1e-6);
//         } else if constexpr (dim == 3) {
//             rhs_field = 1.25;
// 
//             // call evaluateLoadVector
//             rhs_field.fillHalo();
//             lagrangeSpace.evaluateLoadVector(rhs_field);
//             rhs_field.fillHalo();
// 
//             // For order 3 3D: vertices + edge DOFs + face DOFs + volume DOFs
//             using VertexType      = ippl::Vertex<dim>;
//             using EdgeXType       = ippl::EdgeX<dim>;
//             using EdgeYType       = ippl::EdgeY<dim>;
//             using EdgeZType       = ippl::EdgeZ<dim>;
//             using FaceXYType      = ippl::FaceXY<dim>;
//             using FaceXZType      = ippl::FaceXZ<dim>;
//             using FaceYZType      = ippl::FaceYZ<dim>;
//             using HexahedronType  = ippl::Hexahedron<dim>;
// 
//             auto view_ref_vertex     = ref_field.template getView<VertexType>();
//             auto view_ref_edge_x     = ref_field.template getView<EdgeXType>();
//             auto view_ref_edge_y     = ref_field.template getView<EdgeYType>();
//             auto view_ref_edge_z     = ref_field.template getView<EdgeZType>();
//             auto view_ref_face_xy    = ref_field.template getView<FaceXYType>();
//             auto view_ref_face_xz    = ref_field.template getView<FaceXZType>();
//             auto view_ref_face_yz    = ref_field.template getView<FaceYZType>();
//             auto view_ref_hexahedron = ref_field.template getView<HexahedronType>();
// 
//             auto mirror_vertex     = Kokkos::create_mirror_view(view_ref_vertex);
//             auto mirror_edge_x     = Kokkos::create_mirror_view(view_ref_edge_x);
//             auto mirror_edge_y     = Kokkos::create_mirror_view(view_ref_edge_y);
//             auto mirror_edge_z     = Kokkos::create_mirror_view(view_ref_edge_z);
//             auto mirror_face_xy    = Kokkos::create_mirror_view(view_ref_face_xy);
//             auto mirror_face_xz    = Kokkos::create_mirror_view(view_ref_face_xz);
//             auto mirror_face_yz    = Kokkos::create_mirror_view(view_ref_face_yz);
//             auto mirror_hexahedron = Kokkos::create_mirror_view(view_ref_hexahedron);
// 
//             auto ldom_vertex     = ref_field.template getLayout<VertexType>().getLocalNDIndex();
//             auto ldom_edge_x     = ref_field.template getLayout<EdgeXType>().getLocalNDIndex();
//             auto ldom_edge_y     = ref_field.template getLayout<EdgeYType>().getLocalNDIndex();
//             auto ldom_edge_z     = ref_field.template getLayout<EdgeZType>().getLocalNDIndex();
//             auto ldom_face_xy    = ref_field.template getLayout<FaceXYType>().getLocalNDIndex();
//             auto ldom_face_xz    = ref_field.template getLayout<FaceXZType>().getLocalNDIndex();
//             auto ldom_face_yz    = ref_field.template getLayout<FaceYZType>().getLocalNDIndex();
//             auto ldom_hexahedron = ref_field.template getLayout<HexahedronType>().getLocalNDIndex();
// 
//             // Set vertex values
//             nestedViewLoop(mirror_vertex, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_vertex[d].first();
//                 }
// 
//                 // Boundary vertices are zero
//                 if ((coords[0] == 1) || (coords[1] == 1) || (coords[2] == 1) ||
//                     (coords[0] == 5) || (coords[1] == 5) || (coords[2] == 5)) {
//                     mirror_vertex(args...) = 0.0;
//                 } else {
//                     mirror_vertex(args...) = 0.002441406250;  // From calculator
//                 }
//             });
// 
//             // Set EdgeX values - check y,z boundaries
//             nestedViewLoop(mirror_edge_x, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_edge_x[d].first();
//                 }
// 
//                 if ((coords[1] == 1) || (coords[2] == 1) ||
//                     (coords[1] == 5) || (coords[2] == 5)) {
//                     mirror_edge_x(args...) = 0.0;
//                 } else {
//                     mirror_edge_x(args...) = 0.003662109375;  // From calculator
//                 }
//             });
// 
//             // Set EdgeY values - check x,z boundaries
//             nestedViewLoop(mirror_edge_y, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_edge_y[d].first();
//                 }
// 
//                 if ((coords[0] == 1) || (coords[2] == 1) ||
//                     (coords[0] == 5) || (coords[2] == 5)) {
//                     mirror_edge_y(args...) = 0.0;
//                 } else {
//                     mirror_edge_y(args...) = 0.003662109375;  // From calculator
//                 }
//             });
// 
//             // Set EdgeZ values - check x,y boundaries
//             nestedViewLoop(mirror_edge_z, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_edge_z[d].first();
//                 }
// 
//                 if ((coords[0] == 1) || (coords[1] == 1) ||
//                     (coords[0] == 5) || (coords[1] == 5)) {
//                     mirror_edge_z(args...) = 0.0;
//                 } else {
//                     mirror_edge_z(args...) = 0.003662109375;  // From calculator
//                 }
//             });
// 
//             // Set FaceXY values - check z boundary
//             nestedViewLoop(mirror_face_xy, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_face_xy[d].first();
//                 }
// 
//                 if ((coords[2] == 1) || (coords[2] == 5)) {
//                     mirror_face_xy(args...) = 0.0;
//                 } else {
//                     mirror_face_xy(args...) = 0.005493164062;  // From calculator
//                 }
//             });
// 
//             // Set FaceXZ values - check y boundary
//             nestedViewLoop(mirror_face_xz, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_face_xz[d].first();
//                 }
// 
//                 if ((coords[1] == 1) || (coords[1] == 5)) {
//                     mirror_face_xz(args...) = 0.0;
//                 } else {
//                     mirror_face_xz(args...) = 0.005493164062;  // From calculator
//                 }
//             });
// 
//             // Set FaceYZ values - check x boundary
//             nestedViewLoop(mirror_face_yz, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_face_yz[d].first();
//                 }
// 
//                 if ((coords[0] == 1) || (coords[0] == 5)) {
//                     mirror_face_yz(args...) = 0.0;
//                 } else {
//                     mirror_face_yz(args...) = 0.005493164062;  // From calculator
//                 }
//             });
// 
//             // Set hexahedron (volume) values - all interior
//             nestedViewLoop(mirror_hexahedron, 0, [&]<typename... Idx>(const Idx... args) {
//                 using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
//                 index_type coords[dim] = {args...};
// 
//                 for (unsigned int d = 0; d < dim; ++d) {
//                     coords[d] += ldom_hexahedron[d].first();
//                 }
// 
//                 // All volume DOFs are interior
//                 mirror_hexahedron(args...) = 0.008239746094;  // From calculator
//             });
// 
//             Kokkos::fence();
//             Kokkos::deep_copy(view_ref_vertex, mirror_vertex);
//             Kokkos::deep_copy(view_ref_edge_x, mirror_edge_x);
//             Kokkos::deep_copy(view_ref_edge_y, mirror_edge_y);
//             Kokkos::deep_copy(view_ref_edge_z, mirror_edge_z);
//             Kokkos::deep_copy(view_ref_face_xy, mirror_face_xy);
//             Kokkos::deep_copy(view_ref_face_xz, mirror_face_xz);
//             Kokkos::deep_copy(view_ref_face_yz, mirror_face_yz);
//             Kokkos::deep_copy(view_ref_hexahedron, mirror_hexahedron);
//
//             // compare values with reference
//             rhs_field  = rhs_field - ref_field;
//             double err = rhs_field.norm();
//
//             ASSERT_NEAR(err, 0.0, 1e-6);
//         } else {
//             FAIL();
//         }
//     } else {
//         GTEST_SKIP() << "Tests only implemented for order 1, 2, 3";
//     }
// }

int main(int argc, char* argv[]) {
    int success = 1;
    ippl::initialize(argc, argv);
    {
        ::testing::InitGoogleTest(&argc, argv);
        success = RUN_ALL_TESTS();
    }
    ippl::finalize();
    return success;
}
