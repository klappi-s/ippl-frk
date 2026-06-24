// Unit tests for DOFHandler class
// Tests DOF mapping functionality for Lagrange finite element spaces

#include "Ippl.h"

#include "TestUtils.h"
#include "gtest/gtest.h"

template <typename>
class DOFHandlerTest;

template <typename T, typename ExecSpace, unsigned Dim, unsigned Order>
class DOFHandlerTest<Parameters<T, ExecSpace, Rank<Dim>, Rank<Order>>> : public ::testing::Test {
protected:
    void SetUp() override {}

public:
    using value_type = T;
    using exec_space = ExecSpace;
    static constexpr unsigned dim = Dim;
    static constexpr unsigned order = Order;

    static_assert(Dim >= 1 && Dim <= 3, "Dim must be 1, 2 or 3");
    static_assert(Order >= 1 && Order <= 4, "Order must be 1, 2, 3, or 4");

    using mesh_type = ippl::UniformCartesian<T, Dim>;
    using layout_type = ippl::FieldLayout<Dim>;
    using DOFHandler_type = ippl::LagrangeDOFHandler<T, Dim, Order>;
    using SpaceTraits = typename DOFHandler_type::SpaceTraits;

    DOFHandlerTest()
        : nPoints(getGridSizes<Dim>()) {
        for (unsigned d = 0; d < Dim; d++) {
            domain[d] = nPoints[d] / 32.;
        }

        std::array<ippl::Index, Dim> indices;
        for (unsigned d = 0; d < Dim; d++) {
            indices[d] = ippl::Index(nPoints[d]);
        }
        auto owned = std::make_from_tuple<ippl::NDIndex<Dim>>(indices);

        ippl::Vector<T, Dim> hx;
        ippl::Vector<T, Dim> origin;

        std::array<bool, Dim> isParallel;
        isParallel.fill(true);

        for (unsigned d = 0; d < Dim; d++) {
            hx[d]     = domain[d] / nPoints[d];
            origin[d] = 0;
        }

        layout = std::make_shared<layout_type>(MPI_COMM_WORLD, owned, isParallel);
        mesh   = std::make_shared<mesh_type>(owned, hx, origin);
        dofHandler = std::make_shared<DOFHandler_type>(*mesh, *layout);
    }

    std::array<size_t, Dim> nPoints;
    std::array<T, Dim> domain;
    std::shared_ptr<mesh_type> mesh;
    std::shared_ptr<layout_type> layout;
    std::shared_ptr<DOFHandler_type> dofHandler;
};

// Test configurations: double precision, 1D/2D/3D, Order 1-4
// Each Rank<> represents a template parameter: Rank<Dim>, Rank<Order>
using AllTests = ::testing::Types<
    // 1D tests
    Parameters<double, Kokkos::DefaultExecutionSpace, Rank<1>, Rank<1>>,
    Parameters<double, Kokkos::DefaultExecutionSpace, Rank<1>, Rank<2>>,
    Parameters<double, Kokkos::DefaultExecutionSpace, Rank<1>, Rank<3>>,
    Parameters<double, Kokkos::DefaultExecutionSpace, Rank<1>, Rank<4>>,
    // 2D tests
    Parameters<double, Kokkos::DefaultExecutionSpace, Rank<2>, Rank<1>>,
    Parameters<double, Kokkos::DefaultExecutionSpace, Rank<2>, Rank<2>>,
    Parameters<double, Kokkos::DefaultExecutionSpace, Rank<2>, Rank<3>>,
    Parameters<double, Kokkos::DefaultExecutionSpace, Rank<2>, Rank<4>>,
    // 3D tests
    Parameters<double, Kokkos::DefaultExecutionSpace, Rank<3>, Rank<1>>,
    Parameters<double, Kokkos::DefaultExecutionSpace, Rank<3>, Rank<2>>,
    Parameters<double, Kokkos::DefaultExecutionSpace, Rank<3>, Rank<3>>,
    Parameters<double, Kokkos::DefaultExecutionSpace, Rank<3>, Rank<4>>
>;

TYPED_TEST_CASE(DOFHandlerTest, AllTests);

// Test that dofsPerElement is calculated correctly
TYPED_TEST(DOFHandlerTest, DOFsPerElement) {
    constexpr unsigned dim = TestFixture::dim;
    constexpr unsigned order = TestFixture::order;
    constexpr unsigned dofsPerElement = TestFixture::DOFHandler_type::dofsPerElement;

    // For Lagrange elements:
    // Order 1: 2^Dim vertices only
    // Order 2+: vertices + edges + faces + volumes with (Order-1) DOFs per entity dimension

    if constexpr (order == 1) {
        // Only vertices: 2^Dim
        constexpr unsigned expected = (1 << dim);
        EXPECT_EQ(dofsPerElement, expected);
    } else if constexpr (dim == 1 && order == 2) {
        // 2 vertices + 1 edge with 1 DOF
        EXPECT_EQ(dofsPerElement, 3u);
    } else if constexpr (dim == 1 && order == 3) {
        // 2 vertices + 1 edge with 2 DOFs
        EXPECT_EQ(dofsPerElement, 4u);
    } else if constexpr (dim == 1 && order == 4) {
        // 2 vertices + 1 edge with 3 DOFs
        EXPECT_EQ(dofsPerElement, 5u);
    } else if constexpr (dim == 2 && order == 2) {
        // 4 vertices + 4 edges with 1 DOF each + 1 face with 1 DOF
        EXPECT_EQ(dofsPerElement, 9u);
    } else if constexpr (dim == 2 && order == 3) {
        // 4 vertices + 4 edges with 2 DOFs each + 1 face with 4 DOFs
        EXPECT_EQ(dofsPerElement, 16u);
    } else if constexpr (dim == 2 && order == 4) {
        // 4 vertices + 4 edges with 3 DOFs each + 1 face with 9 DOFs
        EXPECT_EQ(dofsPerElement, 25u);
    } else if constexpr (dim == 3 && order == 2) {
        // 8 vertices + 12 edges with 1 DOF each + 6 faces with 1 DOF each + 1 volume with 1 DOF
        EXPECT_EQ(dofsPerElement, 27u);
    } else if constexpr (dim == 3 && order == 3) {
        // 8 vertices + 12 edges with 2 DOFs each + 6 faces with 4 DOFs each + 1 volume with 8 DOFs
        EXPECT_EQ(dofsPerElement, 64u);
    }
    else if constexpr (dim == 3 && order == 4) {
        // 8 vertices + 12 edges with 3 DOFs each + 6 faces with 9 DOFs each + 1 volume with 27 DOFs
        EXPECT_EQ(dofsPerElement, 125u);
    } else {
        FAIL() << "Unsupported dimension/order combination";
    }
}

// Test entity DOF range functions
TYPED_TEST(DOFHandlerTest, EntityDOFRanges) {
    using DOFHandler_type = typename TestFixture::DOFHandler_type;
    constexpr unsigned dim = TestFixture::dim;
    constexpr unsigned order = TestFixture::order;

    // Test Vertex range
    if constexpr (dim >= 1) {
        constexpr size_t vertexStart = DOFHandler_type::template getEntityDOFStart<ippl::Vertex<dim>>();
        constexpr size_t vertexEnd = DOFHandler_type::template getEntityDOFEnd<ippl::Vertex<dim>>();

        EXPECT_EQ(vertexStart, 0u);  // Vertices always start at 0

        // 2^Dim vertex DOFs (1 per vertex)
        constexpr size_t numVertices = (1 << dim);
        EXPECT_EQ(vertexEnd, numVertices);
    }

    // Test Edge ranges (Order >= 2)
    if constexpr (dim >= 1 && order >= 2) {
        if constexpr (dim == 1) {
            // 1D: 1 EdgeX with (Order-1) DOFs
            constexpr size_t edgeStart = DOFHandler_type::template getEntityDOFStart<ippl::EdgeX<dim>>();
            constexpr size_t edgeEnd = DOFHandler_type::template getEntityDOFEnd<ippl::EdgeX<dim>>();
            EXPECT_EQ(edgeStart, 2u);  // After 2 vertices
            EXPECT_EQ(edgeEnd - edgeStart, order - 1);
        } else if constexpr (dim == 2) {
            // 2D: 2 EdgeX and 2 EdgeY, each with (Order-1) DOFs
            constexpr size_t edgeXStart = DOFHandler_type::template getEntityDOFStart<ippl::EdgeX<dim>>();
            constexpr size_t edgeXEnd = DOFHandler_type::template getEntityDOFEnd<ippl::EdgeX<dim>>();
            EXPECT_EQ(edgeXStart, 4u);  // After 4 vertices
            EXPECT_EQ(edgeXEnd - edgeXStart, 2 * (order - 1));

            constexpr size_t edgeYStart = DOFHandler_type::template getEntityDOFStart<ippl::EdgeY<dim>>();
            constexpr size_t edgeYEnd = DOFHandler_type::template getEntityDOFEnd<ippl::EdgeY<dim>>();
            EXPECT_EQ(edgeYStart, edgeXEnd);
            EXPECT_EQ(edgeYEnd - edgeYStart, 2 * (order - 1));
        } else if constexpr (dim == 3) {
            // 3D: 4 edges per direction, each with (Order-1) DOFs
            constexpr size_t edgeXStart = DOFHandler_type::template getEntityDOFStart<ippl::EdgeX<dim>>();
            constexpr size_t edgeXEnd = DOFHandler_type::template getEntityDOFEnd<ippl::EdgeX<dim>>();
            EXPECT_EQ(edgeXStart, 8u);  // After 8 vertices
            EXPECT_EQ(edgeXEnd - edgeXStart, 4 * (order - 1));
        }
    }

    // Test Face ranges (2D/3D, Order >= 2)
    if constexpr (dim >= 2 && order >= 2) {
        if constexpr (dim == 2) {
            // 2D: 1 FaceXY with (Order-1)^2 DOFs
            constexpr size_t faceStart = DOFHandler_type::template getEntityDOFStart<ippl::FaceXY<dim>>();
            constexpr size_t faceEnd = DOFHandler_type::template getEntityDOFEnd<ippl::FaceXY<dim>>();
            // After 4 vertices + 4 edges
            EXPECT_EQ(faceStart, 4u + 4 * (order - 1));
            EXPECT_EQ(faceEnd - faceStart, (order - 1) * (order - 1));
        } else if constexpr (dim == 3) {
            // 3D: 2 faces per orientation, each with (Order-1)^2 DOFs
            // FaceXY
            constexpr size_t faceXYStart = DOFHandler_type::template getEntityDOFStart<ippl::FaceXY<dim>>();
            constexpr size_t faceXYEnd = DOFHandler_type::template getEntityDOFEnd<ippl::FaceXY<dim>>();
            // After 8 vertices + 12 edges
            EXPECT_EQ(faceXYStart, 8u + 12 * (order - 1));
            EXPECT_EQ(faceXYEnd - faceXYStart, 2 * (order - 1) * (order - 1));

            // FaceXZ
            constexpr size_t faceXZStart = DOFHandler_type::template getEntityDOFStart<ippl::FaceXZ<dim>>();
            constexpr size_t faceXZEnd = DOFHandler_type::template getEntityDOFEnd<ippl::FaceXZ<dim>>();
            // Start after FaceXY
            EXPECT_EQ(faceXZStart, faceXYEnd);
            EXPECT_EQ(faceXZEnd - faceXZStart, 2 * (order - 1) * (order - 1));

            // FaceYZ
            constexpr size_t faceYZStart = DOFHandler_type::template getEntityDOFStart<ippl::FaceYZ<dim>>();
            constexpr size_t faceYZEnd = DOFHandler_type::template getEntityDOFEnd<ippl::FaceYZ<dim>>();
            // Start after FaceXZ
            EXPECT_EQ(faceYZStart, faceXZEnd);
            EXPECT_EQ(faceYZEnd - faceYZStart, 2 * (order - 1) * (order - 1));
        }
    }
}

// Test element NDIndex conversion
TYPED_TEST(DOFHandlerTest, ElementNDIndexConversion) {
    auto& dofHandler = this->dofHandler;
    constexpr unsigned dim = TestFixture::dim;
    const auto& nPoints = this->nPoints;

    // Number of elements in each direction
    unsigned numElements = 1;
    ippl::Vector<size_t, dim> ne;
    for (unsigned d = 0; d < dim; ++d) {
        ne[d] = nPoints[d] - 1;
        numElements *= ne[d];
    }

    // Test a few specific elements
    // Element 0 should be at [0, 0, ...]
    auto ndIndex0 = dofHandler->getElementNDIndex(0);
    for (unsigned d = 0; d < dim; ++d) {
        EXPECT_EQ(ndIndex0[d], 0u);
    }

    // Last element
    auto ndIndexLast = dofHandler->getElementNDIndex(numElements - 1);
    for (unsigned d = 0; d < dim; ++d) {
        EXPECT_EQ(ndIndexLast[d], ne[d] - 1);
    }

    // Test all elements for consistency
    for (size_t elemIdx = 0; elemIdx < numElements; ++elemIdx) {
        auto ndIndex = dofHandler->getElementNDIndex(elemIdx);

        // Verify all indices are within bounds
        for (unsigned d = 0; d < dim; ++d) {
            EXPECT_LT(ndIndex[d], ne[d]);
        }

        // Verify we can reconstruct the linear index
        size_t reconstructed = 0;
        size_t stride = 1;
        for (unsigned d = 0; d < dim; ++d) {
            reconstructed += ndIndex[d] * stride;
            stride *= ne[d];
        }
        EXPECT_EQ(reconstructed, elemIdx);
    }
}

// Test DOF mapping for specific elements
TYPED_TEST(DOFHandlerTest, ElementDOFMapping) {
    auto& dofHandler = this->dofHandler;
    constexpr unsigned dim = TestFixture::dim;
    constexpr unsigned order = TestFixture::order;
    constexpr unsigned dofsPerElement = TestFixture::DOFHandler_type::dofsPerElement;

    // Get mapping for element 0
    for (size_t localDOF = 0; localDOF < dofsPerElement; ++localDOF) {
        auto mapping = dofHandler->getElementDOFMapping(localDOF);

        // Check that entity type index is in valid range
        EXPECT_LT(mapping.entityTypeIndex, std::tuple_size<typename TestFixture::SpaceTraits::EntityTypes>::value);

        // Check that entity local index is within bounds
        for (unsigned d = 0; d < dim; ++d) {
            EXPECT_LE(mapping.entityLocalIndex[d], 1u);  // Should be 0 or 1
        }

        // Check that entity local DOF is reasonable
        if constexpr (order == 1) {
            EXPECT_EQ(mapping.entityLocalDOF, 0);  // At most one DOF per vertex
        } else if constexpr (order >= 2 && dim == 1) {
            EXPECT_LT(mapping.entityLocalDOF, (order - 1));  // At most (Order-1) DOFs on 1D edge
        } else if constexpr (order >= 2 && dim == 2) {
            EXPECT_LT(mapping.entityLocalDOF, (order - 1) * (order - 1));  // At most (Order-1)^2 on 2D faces
        } else if constexpr (order >= 2 && dim == 3) {
            EXPECT_LT(mapping.entityLocalDOF, (order - 1) * (order - 1) * (order - 1));  // At most (Order-1)^3 on 3D volumes
        }
    }
}

// Test that vertex DOFs come first
TYPED_TEST(DOFHandlerTest, VertexDOFsFirst) {
    auto& dofHandler = this->dofHandler;
    constexpr unsigned dim = TestFixture::dim;
    constexpr unsigned numVertices = (1 << dim);

    // All DOFs should be vertex DOFs for Order 1
    for (size_t localDOF = 0; localDOF < numVertices; ++localDOF) {
        auto mapping = dofHandler->getElementDOFMapping(localDOF);

        // Should map to vertex entity type (index 0)
        EXPECT_EQ(mapping.entityTypeIndex, 0u);

        // Each vertex should have only 1 DOF
        EXPECT_EQ(mapping.entityLocalDOF, 0u);
    }
}

// Test counter-clockwise vertex ordering
TYPED_TEST(DOFHandlerTest, CounterClockwiseVertexOrdering) {
    if constexpr (TestFixture::dim == 1) {
        auto& dofHandler = this->dofHandler;

        // Expected counter-clockwise vertex positions: [0], [1]
        std::array<size_t, 2> expectedOffsets = {0, 1};

        for (size_t v = 0; v < 2; ++v) {
            auto mapping = dofHandler->getElementDOFMapping(v);
            EXPECT_EQ(mapping.entityLocalIndex[0], expectedOffsets[v]);
        }
    } else if constexpr (TestFixture::dim == 2) {
        auto& dofHandler = this->dofHandler;

        // Expected counter-clockwise vertex positions: [0,0], [1,0], [1,1], [0,1]
        std::array<std::array<size_t, 2>, 4> expectedOffsets = {{
            {0, 0}, {1, 0}, {1, 1}, {0, 1}
        }};

        for (size_t v = 0; v < 4; ++v) {
            auto mapping = dofHandler->getElementDOFMapping(v);
            EXPECT_EQ(mapping.entityLocalIndex[0], expectedOffsets[v][0]);
            EXPECT_EQ(mapping.entityLocalIndex[1], expectedOffsets[v][1]);
        }
    }
    else if constexpr (TestFixture::dim == 3) {
        auto& dofHandler = this->dofHandler;

        // Expected counter-clockwise vertex positions
        // z=0 plane: [0,0,0], [1,0,0], [1,1,0], [0,1,0]
        // z=1 plane: [0,0,1], [1,0,1], [1,1,1], [0,1,1]
        std::array<std::array<size_t, 3>, 8> expectedOffsets = {{
            {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
            {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}
        }};

        for (size_t v = 0; v < 8; ++v) {
            auto mapping = dofHandler->getElementDOFMapping(v);
            EXPECT_EQ(mapping.entityLocalIndex[0], expectedOffsets[v][0]);
            EXPECT_EQ(mapping.entityLocalIndex[1], expectedOffsets[v][1]);
            EXPECT_EQ(mapping.entityLocalIndex[2], expectedOffsets[v][2]);
        }
    }
}

// Test counter-clockwise edge ordering
TYPED_TEST(DOFHandlerTest, CounterClockwiseEdgeOrdering) {
    auto& dofHandler = this->dofHandler;
    constexpr unsigned order = TestFixture::order;

    if constexpr (TestFixture::dim == 1 && order >= 2) {
        // Expected edge positions: EdgeX(0)
        std::array<size_t, 1> expectedOffsets = {0};

        for (size_t localDOF = 0; localDOF < order - 1; ++localDOF) {
            size_t dofIndex = 2 + localDOF;  // After 2 vertex DOFs
            auto mapping = dofHandler->getElementDOFMapping(dofIndex);
            EXPECT_EQ(mapping.entityLocalIndex[0], expectedOffsets[0]);
            EXPECT_EQ(mapping.entityLocalDOF, localDOF);
        }
    } else

    if constexpr (TestFixture::dim == 2 && order >= 2) {
        // Expected edge positions: EdgeX(0,0), EdgeX(0,1), EdgeY(0,0), EdgeY(1,0)
        std::array<std::array<size_t, 2>, 4> expectedOffsets = {{
            {0, 0}, {0, 1}, {0, 0}, {1, 0}
        }};

        for (size_t e = 0; e < 4; ++e) {
            for (size_t localDOF = 0; localDOF < order - 1; ++localDOF) {
                size_t dofIndex = 4 + e * (order - 1) + localDOF;  // After 4 vertex DOFs
                auto mapping = dofHandler->getElementDOFMapping(dofIndex);
                EXPECT_EQ(mapping.entityLocalIndex[0], expectedOffsets[e][0]);
                EXPECT_EQ(mapping.entityLocalIndex[1], expectedOffsets[e][1]);
                EXPECT_EQ(mapping.entityLocalDOF, localDOF);
            }
        }
    } else if constexpr (TestFixture::dim == 3 && order >= 2) {
        // Expected counter-clockwise edge positions
        // EdgeX: (0,0,0), (0,1,0), (0,1,1), (0,0,1)
        // EdgeY: (0,0,0), (1,0,0), (1,0,1), (0,0,1)
        // EdgeZ: (0,0,0), (1,0,0), (1,1,0), (0,1,0)

        std::array<std::array<size_t, 3>, 12> expectedOffsets = {{
            {0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1},  // EdgeX
            {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1},  // EdgeY
            {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}   // EdgeZ
        }};

        for (size_t e = 0; e < 12; ++e) {
            for (size_t localDOF = 0; localDOF < order - 1; ++localDOF) {
                size_t dofIndex = 8 + e * (order - 1) + localDOF;  // After 8 vertex DOFs
                auto mapping = dofHandler->getElementDOFMapping(dofIndex);
                EXPECT_EQ(mapping.entityLocalIndex[0], expectedOffsets[e][0]);
                EXPECT_EQ(mapping.entityLocalIndex[1], expectedOffsets[e][1]);
                EXPECT_EQ(mapping.entityLocalIndex[2], expectedOffsets[e][2]);
                EXPECT_EQ(mapping.entityLocalDOF, localDOF);
            }
        }
    }
}

// Test face DOF ordering in 2D and 3D
TYPED_TEST(DOFHandlerTest, FaceDOFOrdering) {
    auto& dofHandler = this->dofHandler;
    constexpr unsigned dim = TestFixture::dim;
    constexpr unsigned order = TestFixture::order;

    if constexpr (dim == 2 && order >= 2) {
        // Expected face position: FaceXY(0,0)
        std::array<size_t, 2> expectedOffsets = {0, 0};

        size_t faceStartIndex = 4 + 4 * (order - 1);  // After vertices and edges
        size_t numFaceDOFs = (order - 1) * (order - 1);

        for (size_t localDOF = 0; localDOF < numFaceDOFs; ++localDOF) {
            size_t dofIndex = faceStartIndex + localDOF;
            auto mapping = dofHandler->getElementDOFMapping(dofIndex);
            EXPECT_EQ(mapping.entityLocalIndex[0], expectedOffsets[0]);
            EXPECT_EQ(mapping.entityLocalIndex[1], expectedOffsets[1]);
            EXPECT_EQ(mapping.entityLocalDOF, localDOF);
        }
    } else if constexpr (dim == 3 && order >= 2) {
        // Expected face positions:
        // FacesXY: (0,0,0), (0,0,1)
        // FacesXZ: (0,0,0), (0,1,0)
        // FacesYZ: (0,0,0), (1,0,0)

        std::array<std::array<size_t, 3>, 6> expectedOffsets = {{
            {0, 0, 0}, {0, 0, 1},   // FacesXY
            {0, 0, 0}, {0, 1, 0},   // FacesXZ
            {0, 0, 0}, {1, 0, 0}    // FacesYZ
        }};

        size_t faceStartIndex = 8 + 12 * (order - 1);  // After vertices and edges
        size_t numFaceDOFs = (order - 1) * (order - 1);

        for (unsigned f = 0; f < 6; ++f) {
            for (size_t localDOF = 0; localDOF < numFaceDOFs; ++localDOF) {
                size_t dofIndex = faceStartIndex + f * (order - 1) * (order - 1) + localDOF;
                auto mapping = dofHandler->getElementDOFMapping(dofIndex);
                EXPECT_EQ(mapping.entityLocalIndex[0], expectedOffsets[f][0]);
                EXPECT_EQ(mapping.entityLocalIndex[1], expectedOffsets[f][1]);
                EXPECT_EQ(mapping.entityLocalIndex[2], expectedOffsets[f][2]);
                EXPECT_EQ(mapping.entityLocalDOF, localDOF);
            }
        }
    }
}

int main(int argc, char* argv[]) {
    ::testing::InitGoogleTest(&argc, argv);

    int success = 1;

    ippl::initialize(argc, argv);
    {
        success = RUN_ALL_TESTS();
    }
    ippl::finalize();

    return success;
}