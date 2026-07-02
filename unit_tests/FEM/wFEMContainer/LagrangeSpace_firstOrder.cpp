

#include "Ippl.h"

#include <functional>

#include "TestUtils.h"
#include "gtest/gtest.h"

// LagrangeSpace_wFEMContainer unit tests — first order (P1) only (Track B).
//
// Split from LagrangeSpace_wFEMContainer.cpp. Polynomial order 1 only; all cases are
// active and expected to pass under mpiexec.
//
// Typed over (precision, order=1, dim=1..3): 6 instantiations. Assembly tests use
// decomposition-aware oracles (global mesh indices via ldom[d].first()) and
// FEMContainer::norm() (MPI allreduce).
//
// | Test                         | MPI | Notes |
// |------------------------------|-----|-------|
// | evaluateRefElementShapeFunction | yes | Reference element only |
// | evaluateRefElementShapeFunctionGradient | yes | Reference element only |
// | evaluateAx                   | yes | Hand oracle on biggerMesh ([0,1]^d, 5^d nodes) |
// | evaluateLoadVector           | yes | Hand oracle on symmetricMesh ([-1,1]^d) |
//
// evaluateAx pipeline per case: constant x -> evaluateLoadVector(x) -> evaluateAx(x,eval)
// (historical test design). Oracles are hand-computed for biggerMesh (5^d nodes) or
// symmetricMesh (evaluateLoadVector).

template <typename>
class LagrangeSpaceFirstOrderTest;

template <typename Tlhs, unsigned Dim, unsigned numElemDOFs>
struct EvalFunctor {
    const ippl::Vector<Tlhs, Dim> DPhiInvT;
    const Tlhs absDetDPhi;

    EvalFunctor(ippl::Vector<Tlhs, Dim> DPhiInvT, Tlhs absDetDPhi)
        : DPhiInvT(DPhiInvT)
        , absDetDPhi(absDetDPhi) {}

    KOKKOS_FUNCTION auto operator()(const size_t& i, const size_t& j,
                    const ippl::QuadratureData<Tlhs, ippl::Vector<Tlhs, Dim>, numElemDOFs>& qd) const {
        return dot((DPhiInvT * qd.deriv_q[j]), (DPhiInvT * qd.deriv_q[i])).apply() * absDetDPhi;
    }
};

template <typename T, typename ExecSpace, unsigned Order, unsigned Dim>
class LagrangeSpaceFirstOrderTest<Parameters<T, ExecSpace, Rank<Order>, Rank<Dim>>> : public ::testing::Test {
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

    LagrangeSpaceFirstOrderTest()
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
using Orders     = TestParams::Ranks<1>;
using Dimensions = TestParams::Ranks<1, 2, 3>;
using Combos     = CreateCombinations<Precisions, Spaces, Orders, Dimensions>::type;
using Tests      = TestForTypes<Combos>::type;
TYPED_TEST_CASE(LagrangeSpaceFirstOrderTest, Tests);


TYPED_TEST(LagrangeSpaceFirstOrderTest, evaluateRefElementShapeFunction) {
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

    // Test 3: Specific known values for low orders
    {
        // Order 1: Linear basis functions
        if (dim == 1) {
            // φ0(x) = 1-x,  φ1(x) = x
            // Test at x = 0.25
            T x = 0.25;
            ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(0, x), 0.75, tolerance)
                << "Order 1, 1D: φ0(0.25) should be 0.75";
            ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(1, x), 0.25, tolerance)
                << "Order 1, 1D: φ1(0.25) should be 0.25";

            // Test at x = 0.5 (midpoint)
            x = 0.5;
            ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(0, x), 0.5, tolerance)
                << "Order 1, 1D: φ0(0.5) should be 0.5";
            ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(1, x), 0.5, tolerance)
                << "Order 1, 1D: φ1(0.5) should be 0.5";
        } else if (dim == 2) {
            // Bilinear basis functions on quad
            // Test at center point (0.5, 0.5) - all should be 0.25
            ippl::Vector<T, dim> center = {0.5, 0.5};
            for (size_t i = 0; i < 4; ++i) {
                ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(i, center), 0.25, tolerance)
                    << "Order 1, 2D: φ" << i << "(0.5, 0.5) should be 0.25";
            }

            // Test at (0.25, 0.75)
            ippl::Vector<T, dim> point = {0.25, 0.75};
            // φ0 = (1-x)(1-y) = 0.75 * 0.25 = 0.1875
            ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(0, point), 0.1875, tolerance);
            // φ1 = x(1-y) = 0.25 * 0.25 = 0.0625
            ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(1, point), 0.0625, tolerance);
            // φ2 = xy = 0.25 * 0.75 = 0.1875
            ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(2, point), 0.1875, tolerance);
            // φ3 = (1-x)y = 0.75 * 0.75 = 0.5625
            ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(3, point), 0.5625, tolerance);
        } else if (dim == 3) {
            // Trilinear basis functions on hexahedron
            // Test at center point (0.5, 0.5, 0.5) - all should be 0.125
            ippl::Vector<T, dim> center = {0.5, 0.5, 0.5};
            for (size_t i = 0; i < 8; ++i) {
                ASSERT_NEAR(lagrangeSpace.evaluateRefElementShapeFunction(i, center), 0.125, tolerance)
                    << "Order 1, 3D: φ" << i << "(0.5, 0.5, 0.5) should be 0.125";
            }
        }
    }
}

TYPED_TEST(LagrangeSpaceFirstOrderTest, evaluateRefElementShapeFunctionGradient) {
    // Reference-element gradients: P1 linear constants; P2 spot values at nodes/midpoints.
    auto& lagrangeSpace      = this->lagrangeSpace;
    static constexpr std::size_t dim = TestFixture::dim;
    const std::size_t& order = lagrangeSpace.order;
    using T                  = typename TestFixture::value_t;

    T tolerance = std::numeric_limits<T>::epsilon() * 10.0;

    if (order == 1) {
        if (dim == 1) {
            for (T x = 0.0; x < 1.0; x += 0.05) {
                const auto grad_0 = lagrangeSpace.evaluateRefElementShapeFunctionGradient(0, x);
                const auto grad_1 = lagrangeSpace.evaluateRefElementShapeFunctionGradient(1, x);

                ASSERT_NEAR(grad_0[0], -1.0, tolerance);
                ASSERT_NEAR(grad_1[0], 1.0, tolerance);
            }
        } else if (dim == 2) {
            ippl::Vector<T, dim> point;
            for (T x = 0.0; x < 1.0; x += 0.05) {
                point[0] = x;
                for (T y = 0.0; y < 1.0; y += 0.05) {
                    point[1] = y;

                    const auto grad_0 =
                        lagrangeSpace.evaluateRefElementShapeFunctionGradient(0, point);
                    const auto grad_1 =
                        lagrangeSpace.evaluateRefElementShapeFunctionGradient(1, point);
                    const auto grad_2 =
                        lagrangeSpace.evaluateRefElementShapeFunctionGradient(2, point);
                    const auto grad_3 =
                        lagrangeSpace.evaluateRefElementShapeFunctionGradient(3, point);

                    ASSERT_NEAR(grad_0[0], y - 1.0, tolerance);
                    ASSERT_NEAR(grad_0[1], x - 1.0, tolerance);

                    ASSERT_NEAR(grad_1[0], 1.0 - y, tolerance);
                    ASSERT_NEAR(grad_1[1], -x, tolerance);

                    ASSERT_NEAR(grad_2[0], y, tolerance);
                    ASSERT_NEAR(grad_2[1], x, tolerance);

                    ASSERT_NEAR(grad_3[0], -y, tolerance);
                    ASSERT_NEAR(grad_3[1], 1.0 - x, tolerance);
                }
            }
        } else if (dim == 3) {
            ippl::Vector<T, dim> point;
            for (T x = 0.0; x < 1.0; x += 0.05) {
                point[0] = x;
                for (T y = 0.0; y < 1.0; y += 0.05) {
                    point[1] = y;
                    for (T z = 0.0; z < 1.0; z += 0.05) {
                        point[2] = z;

                        const auto grad_0 =
                            lagrangeSpace.evaluateRefElementShapeFunctionGradient(0, point);
                        const auto grad_1 =
                            lagrangeSpace.evaluateRefElementShapeFunctionGradient(1, point);
                        const auto grad_2 =
                            lagrangeSpace.evaluateRefElementShapeFunctionGradient(2, point);
                        const auto grad_3 =
                            lagrangeSpace.evaluateRefElementShapeFunctionGradient(3, point);
                        const auto grad_4 =
                            lagrangeSpace.evaluateRefElementShapeFunctionGradient(4, point);
                        const auto grad_5 =
                            lagrangeSpace.evaluateRefElementShapeFunctionGradient(5, point);
                        const auto grad_6 =
                            lagrangeSpace.evaluateRefElementShapeFunctionGradient(6, point);
                        const auto grad_7 =
                            lagrangeSpace.evaluateRefElementShapeFunctionGradient(7, point);

                        ASSERT_NEAR(grad_0[0], -1.0 * (1.0 - y) * (1.0 - z), tolerance);
                        ASSERT_NEAR(grad_0[1], (1.0 - x) * -1.0 * (1.0 - z), tolerance);
                        ASSERT_NEAR(grad_0[2], (1.0 - x) * (1.0 - y) * -1.0, tolerance);

                        ASSERT_NEAR(grad_1[0], 1.0 * (1.0 - y) * (1.0 - z), tolerance);
                        ASSERT_NEAR(grad_1[1], x * -1.0 * (1.0 - z), tolerance);
                        ASSERT_NEAR(grad_1[2], x * (1.0 - y) * -1.0, tolerance);

                        ASSERT_NEAR(grad_2[0], 1.0 * y * (1.0 - z), tolerance);
                        ASSERT_NEAR(grad_2[1], x * 1.0 * (1.0 - z), tolerance);
                        ASSERT_NEAR(grad_2[2], x * y * -1.0, tolerance);

                        ASSERT_NEAR(grad_3[0], -1.0 * y * (1.0 - z), tolerance);
                        ASSERT_NEAR(grad_3[1], (1.0 - x) * 1.0 * (1.0 - z), tolerance);
                        ASSERT_NEAR(grad_3[2], (1.0 - x) * y * -1.0, tolerance);

                        ASSERT_NEAR(grad_4[0], -1.0 * (1.0 - y) * z, tolerance);
                        ASSERT_NEAR(grad_4[1], (1.0 - x) * -1.0 * z, tolerance);
                        ASSERT_NEAR(grad_4[2], (1.0 - x) * (1.0 - y) * 1.0, tolerance);

                        ASSERT_NEAR(grad_5[0], 1.0 * (1.0 - y) * z, tolerance);
                        ASSERT_NEAR(grad_5[1], x * -1.0 * z, tolerance);
                        ASSERT_NEAR(grad_5[2], x * (1.0 - y) * 1.0, tolerance);

                        ASSERT_NEAR(grad_6[0], 1.0 * y * z, tolerance);
                        ASSERT_NEAR(grad_6[1], x * 1.0 * z, tolerance);
                        ASSERT_NEAR(grad_6[2], x * y * 1.0, tolerance);

                        ASSERT_NEAR(grad_7[0], -1.0 * y * z, tolerance);
                        ASSERT_NEAR(grad_7[1], (1.0 - x) * 1.0 * z, tolerance);
                        ASSERT_NEAR(grad_7[2], (1.0 - x) * y * 1.0, tolerance);
                    }
                }
            }
        } else {
            FAIL();
        }
    }
}

TYPED_TEST(LagrangeSpaceFirstOrderTest, evaluateAx) {
    // Matrix-free stiffness apply (weak Laplacian) on a decomposed FEMContainer field.
    // Reference values are tabulated per global mesh index; valid for any MPI rank count
    // supported by the layout (after accumulateHalo on assembly).

    {
        using T         = typename TestFixture::value_t;
        using FieldType = typename TestFixture::FieldType;
        using BCType    = typename TestFixture::BCType;
        using LagrangeType = typename TestFixture::LagrangeType;
    
        const auto& refElement           = this->ref_element;
        const auto& lagrangeSpace        = this->lagrangeSpaceBigger;
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

        // P1: 1-point element quadrature on biggerMesh; oracles keyed by global vertex index.
        if constexpr (dim == 1) {
            x = 1.25;

            x.fillHalo();
            lagrangeSpace.evaluateLoadVector(x);
            x.fillHalo();

            z = lagrangeSpace.evaluateAx(x, eval);
            z.fillHalo();

            // set up for comparison
            FieldType ref_field(mesh, layout, 1);

            using VertexType = ippl::Vertex<dim>;
            auto view_ref = ref_field.template getView<VertexType>();
            auto mirror   = Kokkos::create_mirror_view(view_ref);

            auto ldom     = ref_field.template getLayout<VertexType>().getLocalNDIndex();

            nestedViewLoop(mirror, 0, [&]<typename... Idx>(const Idx... args) {
                using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
                index_type coords[dim] = {args...};

                // global coordinates
                // We don't take into account nghost as this causes
                // coords to be negative, which causes an overflow due
                // to the index type.
                // All below indices for setting the ref_field are 
                // shifted by 1 to include the ghost (applies to all tests).
                for (unsigned int d = 0; d < lagrangeSpace.dim; ++d) {
                    coords[d] += ldom[d].first();
                }

                // reference field
                if ((coords[0] == 2) || (coords[0] == 4)) {
                    mirror(args...) = 1.25;
                } else {
                    mirror(args...) = 0.0;
                }
            });
            Kokkos::fence();

            Kokkos::deep_copy(view_ref, mirror);

            // compare values with reference
            z  = z - ref_field;
            double err = z.norm();

            ASSERT_NEAR(err, 0.0, 1e-6);
        } else if constexpr (dim == 2) {
            // P1 2D: checkerboard-like pattern on 5x5 vertex grid (biggerMesh).
            x = 1.0;

            x.fillHalo();
            lagrangeSpace.evaluateLoadVector(x);
            x.fillHalo();

            z = lagrangeSpace.evaluateAx(x, eval);
            z.fillHalo();

            // set up for comparison
            FieldType ref_field(mesh, layout, 1);
            using VertexType = ippl::Vertex<dim>;
            auto view_ref = ref_field.template getView<VertexType>();
            auto mirror   = Kokkos::create_mirror_view(view_ref);

            auto ldom     = ref_field.template getLayout<VertexType>().getLocalNDIndex();

            nestedViewLoop(mirror, 0, [&]<typename... Idx>(const Idx... args) {
                using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
                index_type coords[dim] = {args...};

                // global coordinates
                for (unsigned int d = 0; d < lagrangeSpace.dim; ++d) {
                    coords[d] += ldom[d].first();
                }
                
                // reference field
                if (((coords[0] == 2) && (coords[1] == 2)) ||
                    ((coords[0] == 2) && (coords[1] == 4)) ||
                    ((coords[0] == 4) && (coords[1] == 2)) ||
                    ((coords[0] == 4) && (coords[1] == 4))) {
                    mirror(args...) = 1.5;
                } else if (((coords[0] == 2) && (coords[1] == 3)) ||
                    ((coords[0] == 3) && (coords[1] == 2)) ||
                    ((coords[0] == 3) && (coords[1] == 4)) ||
                    ((coords[0] == 4) && (coords[1] == 3))) {
                    mirror(args...) = 1.0;
                } else {
                    mirror(args...) = 0.0;
                }
            });
            Kokkos::fence();

            Kokkos::deep_copy(view_ref, mirror);

            // compare values with reference
            z  = z - ref_field;
            double err = z.norm();

            ASSERT_NEAR(err, 0.0, 1e-6);
        } else if constexpr (dim == 3) {
            // P1 3D: interior vertex stencil on 5x5x5 grid.
            x = 1.5;

            x.fillHalo();
            lagrangeSpace.evaluateLoadVector(x);
            x.fillHalo();

            z = lagrangeSpace.evaluateAx(x, eval);
            z.fillHalo();

            // set up for comparison
            FieldType ref_field(mesh, layout, 1);
            using VertexType = ippl::Vertex<dim>;
            auto view_ref = ref_field.template getView<VertexType>();
            auto mirror   = Kokkos::create_mirror_view(view_ref);

            auto ldom     = ref_field.template getLayout<VertexType>().getLocalNDIndex();

            nestedViewLoop(mirror, 0, [&]<typename... Idx>(const Idx... args) {
                using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
                index_type coords[dim] = {args...};

                // global coordinates
                for (unsigned int d = 0; d < lagrangeSpace.dim; ++d) {
                    coords[d] += ldom[d].first();
                }

                // reference field
                if (((coords[0] > 1) && (coords[0] < 5)) && 
                    ((coords[1] > 1) && (coords[1] < 5)) && 
                    ((coords[2] > 1) && (coords[2] < 5))) {
                    
                    mirror(args...) = 2.53125;
                    
                    if ((coords[0] == 3) || (coords[1] == 3) || (coords[2] == 3)) {
                        mirror(args...) = 2.25;
                    }

                    if (((coords[0] == 3) && (coords[1] == 3) && (coords[2] == 2)) ||
                        ((coords[0] == 3) && (coords[1] == 2) && (coords[2] == 3)) ||
                        ((coords[0] == 2) && (coords[1] == 3) && (coords[2] == 3)) ||
                        ((coords[0] == 4) && (coords[1] == 3) && (coords[2] == 3)) ||
                        ((coords[0] == 3) && (coords[1] == 4) && (coords[2] == 3)) ||
                        ((coords[0] == 3) && (coords[1] == 3) && (coords[2] == 4))) {
                        mirror(args...) = 1.5;
                    }
                    
                    if ((coords[0] == 3) && (coords[1] == 3) && (coords[2] == 3)) {
                        mirror(args...) = 0.0;
                    }
                } else {
                    mirror(args...) = 0.0;
                }
            });
            Kokkos::fence();

            Kokkos::deep_copy(view_ref, mirror);

            // compare values with reference
            z  = z - ref_field;
            double err = z.norm();

            ASSERT_NEAR(err, 0.0, 1e-6);
        } else {
            // only 1D, 2D, 3D supported
            FAIL();
        }
    }
}

TYPED_TEST(LagrangeSpaceFirstOrderTest, evaluateLoadVector) {
    // Weak-form load assembly on symmetricMesh ([-1,1]^d, 5^d nodes) with GL-5 quadrature.
    // Constant coefficient field; per-vertex oracle by global mesh index (MPI-safe).
    using FieldType = typename TestFixture::FieldType;
    using BCType    = typename TestFixture::BCType;

    const auto& lagrangeSpace = this->symmetricLagrangeSpace;
    auto mesh                 = this->symmetricMesh;
    static constexpr std::size_t dim = TestFixture::dim;
    {

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

        if constexpr (dim == 1) {
            rhs_field = 2.75;

            // call evaluateLoadVector
            rhs_field.fillHalo();
            lagrangeSpace.evaluateLoadVector(rhs_field);
            rhs_field.fillHalo();

            std::cout << "RHS Field after evaluateLoadVector (1D):" << std::endl;

            // set up for comparison
            using VertexType = ippl::Vertex<dim>;
            auto view_ref = ref_field.template getView<VertexType>();
            auto mirror   = Kokkos::create_mirror_view(view_ref);

            auto ldom     = ref_field.template getLayout<VertexType>().getLocalNDIndex();

            nestedViewLoop(mirror, 0, [&]<typename... Idx>(const Idx... args) {
                using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
                index_type coords[dim] = {args...};

                // global coordinates
                for (unsigned int d = 0; d < lagrangeSpace.dim; ++d) {
                    coords[d] += ldom[d].first();
                }

                // reference field
                switch (coords[0]) {
                    case 1:
                        mirror(args...) = 0.0;
                        break;
                    case 2:
                        mirror(args...) = 1.375;
                        break;
                    case 3:
                        mirror(args...) = 1.375;
                        break;
                    case 4:
                        mirror(args...) = 1.375;
                        break;
                    case 5:
                        mirror(args...) = 0.0;
                        break;
                    default:
                        mirror(args...) = 0.0;
                }
            });
            Kokkos::fence();

            Kokkos::deep_copy(view_ref, mirror);

            // compare values with reference
            rhs_field  = rhs_field - ref_field;
            double err = rhs_field.norm();

            ASSERT_NEAR(err, 0.0, 1e-6);
        } else if constexpr (dim == 2) {
            rhs_field = 3.5;

            // call evaluateLoadVector
            rhs_field.fillHalo();
            lagrangeSpace.evaluateLoadVector(rhs_field);
            rhs_field.fillHalo();

            // set up for comparison
            using VertexType = ippl::Vertex<dim>;
            auto view_ref = ref_field.template getView<VertexType>();
            auto mirror   = Kokkos::create_mirror_view(view_ref);

            auto ldom     = ref_field.template getLayout<VertexType>().getLocalNDIndex();

            nestedViewLoop(mirror, 0, [&]<typename... Idx>(const Idx... args) {
                using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
                index_type coords[dim] = {args...};

                // global coordinates
                for (unsigned int d = 0; d < lagrangeSpace.dim; ++d) {
                    coords[d] += ldom[d].first();
                }

                // reference field
                if ((coords[0] < 2) || (coords[1] < 2) || 
                    (coords[0] > 4) || (coords[1] > 4)) {
                    mirror(args...) = 0.0;
                } else {
                    mirror(args...) = 0.875;
                }
            });
            Kokkos::fence();

            Kokkos::deep_copy(view_ref, mirror);

            // compare values with reference
            rhs_field  = rhs_field - ref_field;
            double err = rhs_field.norm();

            ASSERT_NEAR(err, 0.0, 1e-6);

        } else if constexpr (dim == 3) {

            rhs_field = 1.25;

            // call evaluateLoadVector
            rhs_field.fillHalo();
            lagrangeSpace.evaluateLoadVector(rhs_field);
            rhs_field.fillHalo();

            // set up for comparison
            using VertexType = ippl::Vertex<dim>;
            auto view_ref = ref_field.template getView<VertexType>();
            auto mirror   = Kokkos::create_mirror_view(view_ref);

            auto ldom     = ref_field.template getLayout<VertexType>().getLocalNDIndex();

            nestedViewLoop(mirror, 0, [&]<typename... Idx>(const Idx... args) {
                using index_type       = std::tuple_element_t<0, std::tuple<Idx...>>;
                index_type coords[dim] = {args...};

                // global coordinates
                for (unsigned int d = 0; d < lagrangeSpace.dim; ++d) {
                    coords[d] += ldom[d].first();
                }

                // reference field
                if ((coords[0] == 1) || (coords[1] == 1) || (coords[2] == 1) ||
                    (coords[0] == 5) || (coords[1] == 5) || (coords[2] == 5)) {
                    mirror(args...) = 0.0;
                } else {
                    mirror(args...) = 0.15625;
                }
            });
            Kokkos::fence();

            Kokkos::deep_copy(view_ref, mirror);

            // compare values with reference
            rhs_field  = rhs_field - ref_field;
            double err = rhs_field.norm();

            ASSERT_NEAR(err, 0.0, 1e-6);
        } else {
            // only dims 1, 2, 3 supported
            FAIL();
        }
    }
}

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
