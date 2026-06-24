//
// Unit test FEMContainerTest
//   Test the functionality of the class FEMContainer.
//
#include "Ippl.h"

#include <Kokkos_MathematicalConstants.hpp>
#include <Kokkos_MathematicalFunctions.hpp>

#include "../src/PoissonSolvers/LaplaceHelpers.h"
#include "TestUtils.h"
#include "gtest/gtest.h"

template <typename>
class FEMContainerTest;

template <typename T, typename ExecSpace, unsigned Dim>
class FEMContainerTest<Parameters<T, ExecSpace, Rank<Dim>>> : public ::testing::Test {
public:
    using value_type              = T;
    using exec_space              = ExecSpace;
    constexpr static unsigned dim = Dim;

    using mesh_type      = ippl::UniformCartesian<T, Dim>;
    using centering_type = typename mesh_type::DefaultCentering;
    using layout_type    = ippl::FieldLayout<Dim>;

    // Vertex-only FEMContainer: Only DOFs on vertices
    using femcontainer_vertex_only_type = ippl::FEMContainer<T, Dim, std::tuple<ippl::Vertex<Dim>>,
                                            std::tuple<std::integral_constant<unsigned, 1>>>;

    // Full FEMContainer: DOFs on all element types
    using full_entitys = std::conditional_t<Dim == 1,
                                        std::tuple<ippl::Vertex<Dim>, ippl::EdgeX<Dim>>,
                                    std::conditional_t<Dim == 2,
                                        std::tuple<ippl::Vertex<Dim>, ippl::EdgeX<Dim>, ippl::EdgeY<Dim>, ippl::FaceXY<Dim>>,
                                    std::tuple<ippl::Vertex<Dim>, ippl::EdgeX<Dim>, ippl::EdgeY<Dim>, ippl::EdgeZ<Dim>,
                                        ippl::FaceXY<Dim>, ippl::FaceXZ<Dim>, ippl::FaceYZ<Dim>, ippl::Hexahedron<Dim>>>>;

    using full_dofnums = std::conditional_t<Dim == 1,
                                        std::tuple<std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>>,
                                    std::conditional_t<Dim == 2,
                                        std::tuple<std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>>,
                                    std::tuple<std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>>>>;

    using femcontainer_full_type =  ippl::FEMContainer<T, Dim, full_entitys, full_dofnums>;

    FEMContainerTest()
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
        
        // Set up DOF numbers for FEMContainer
        // Vertex-only FEMContainer: Only DOFs on vertices (comparable to Field)
        femContainerVertexOnly = std::make_shared<femcontainer_vertex_only_type>(*mesh, *layout);

        // Full FEMContainer: DOFs on all element types
        femContainerFull = std::make_shared<femcontainer_full_type>(*mesh, *layout);
    }

    std::shared_ptr<mesh_type> mesh;
    std::shared_ptr<layout_type> layout;
    std::shared_ptr<femcontainer_vertex_only_type> femContainerVertexOnly;  // Only vertex DOFs
    std::shared_ptr<femcontainer_full_type> femContainerFull;       // All element type DOFs
    std::array<size_t, Dim> nPoints;
    std::array<T, Dim> domain;
};

template <typename Params>
struct VFieldVal {
    using vfield_view_type        = typename FEMContainerTest<Params>::vfield_type::view_type;
    using T                       = typename FEMContainerTest<Params>::value_type;
    constexpr static unsigned Dim = FEMContainerTest<Params>::dim;

    const vfield_view_type vview;
    const ippl::NDIndex<Dim> lDom;

    ippl::Vector<T, Dim> dx;
    int shift;

    VFieldVal(const vfield_view_type& view, const ippl::NDIndex<Dim>& lDom, ippl::Vector<T, Dim> hx,
              int shift = 0)
        : vview(view)
        , lDom(lDom)
        , dx(hx)
        , shift(shift) {}

    template <typename... Idx>
    KOKKOS_INLINE_FUNCTION void operator()(const Idx... args) const {
        ippl::Vector<T, Dim> coords = {static_cast<T>(args)...};
        vview(args...)              = (0.5 + coords + lDom.first()) * dx;
    }
};

template <typename Params>
struct FieldVal {
    using field_view_type         = typename FEMContainerTest<Params>::field_type::view_type;
    using T                       = typename FEMContainerTest<Params>::value_type;
    constexpr static unsigned Dim = FEMContainerTest<Params>::dim;

    const field_view_type view;

    const ippl::NDIndex<Dim> lDom;

    ippl::Vector<T, Dim> hx   = 0;
    ippl::Vector<T, Dim> rmax = 0;
    int shift;

    FieldVal(const field_view_type& view, const ippl::NDIndex<Dim>& lDom, ippl::Vector<T, Dim> hx,
             int shift = 0, ippl::Vector<T, Dim> rmax = 0)
        : view(view)
        , lDom(lDom)
        , hx(hx)
        , rmax(rmax)
        , shift(shift) {}

    // range policy tags
    struct Norm {};
    struct Integral {};
    struct Hessian {};

    const T pi = Kokkos::numbers::pi_v<T>;

    template <typename... Idx>
    KOKKOS_INLINE_FUNCTION void operator()(const Norm&, const Idx... args) const {
        T tot = (args + ...);
        for (unsigned d = 0; d < Dim; d++) {
            tot += lDom[d].first();
        }
        view(args...) = tot - 1;
    }

    template <typename... Idx>
    KOKKOS_INLINE_FUNCTION void operator()(const Integral&, const Idx... args) const {
        ippl::Vector<T, Dim> coords = {static_cast<T>(args)...};
        coords                      = (0.5 + coords + lDom.first() - shift) * hx;
        view(args...)               = 1;
        for (const auto& x : coords) {
            view(args...) *= Kokkos::sin(200 * pi * x);
        }
    }

    template <typename... Idx>
    KOKKOS_INLINE_FUNCTION void operator()(const Hessian&, const Idx... args) const {
        ippl::Vector<T, Dim> coords = {static_cast<T>(args)...};
        coords                      = (0.5 + coords + lDom.first() - shift) * hx;
        view(args...)               = 1;
        for (const auto& x : coords) {
            view(args...) *= x;
        }
    }
};

using Tests = TestParams::tests<1, 2, 3>;
TYPED_TEST_SUITE(FEMContainerTest, Tests);


TYPED_TEST(FEMContainerTest, CopyConstructorVertexOnly) {
    // Test copy constructor
    using T = typename TestFixture::value_type;

    auto& container = this->femContainerVertexOnly;
    *container = 2.5;

    // Create a copy using copy constructor
    auto containerCopy = *container;

    // Verify the values match by comparing norms
    T norm_original = container->norm(2);
    T norm_copy = containerCopy.norm(2);

    assertEqual<T>(norm_original, norm_copy);

    // Modify the copy and ensure original is unchanged
    containerCopy += 1.0;
    T norm_modified = containerCopy.norm(2);
    T norm_original_after = container->norm(2);

    assertEqual<T>(norm_original, norm_original_after);  // Original unchanged

    // Modified copy should have larger norm
    ASSERT_GT(norm_modified, norm_original);
}

TYPED_TEST(FEMContainerTest, CopyConstructorFull) {
    // Test copy constructor
    using T = typename TestFixture::value_type;

    auto& container = this->femContainerFull;
    *container = 2.5;

    // Create a copy using copy constructor
    auto containerCopy = *container;

    // Verify the values match by comparing norms
    T norm_original = container->norm(2);
    T norm_copy = containerCopy.norm(2);

    assertEqual<T>(norm_original, norm_copy);

    // Modify the copy and ensure original is unchanged
    containerCopy += 1.0;
    T norm_modified = containerCopy.norm(2);
    T norm_original_after = container->norm(2);

    assertEqual<T>(norm_original, norm_original_after);  // Original unchanged

    // Modified copy should have larger norm
    ASSERT_GT(norm_modified, norm_original);
}

TYPED_TEST(FEMContainerTest, AssignmentOperatorVertexOnly) {
    // Test assignment operator (FEMContainer = FEMContainer)
    using T = typename TestFixture::value_type;

    auto& container1 = this->femContainerVertexOnly;
    *container1 = 3.5;

    // Create another container and assign
    typename TestFixture::femcontainer_vertex_only_type container2(*this->mesh, *this->layout);
    container2 = 1.0;

    T norm_before = container2.norm(2);
    container2 = *container1;
    T norm_after = container2.norm(2);

    // After assignment, norms should match
    assertEqual<T>(container1->norm(2), norm_after);

    // Norm should have changed
    ASSERT_NE(norm_before, norm_after);
}

TYPED_TEST(FEMContainerTest, AssignmentOperatorFull) {
    // Test assignment operator (FEMContainer = FEMContainer)
    using T = typename TestFixture::value_type;

    auto& container1 = this->femContainerFull;
    *container1 = 3.5;

    // Create another container and assign
    typename TestFixture::femcontainer_full_type container2(*this->mesh, *this->layout);
    container2 = 1.0;

    T norm_before = container2.norm(2);
    container2 = *container1;
    T norm_after = container2.norm(2);

    // After assignment, norms should match
    assertEqual<T>(container1->norm(2), norm_after);

    // Norm should have changed
    ASSERT_NE(norm_before, norm_after);
}

TYPED_TEST(FEMContainerTest, AssignmentScalarVertexOnly) {
    // Test assignment of scalar to FEMContainer
    using T = typename TestFixture::value_type;
    unsigned constexpr Dim = TestFixture::dim;

    auto& container = this->femContainerVertexOnly;
    *container = 42;

    // Verify by computing expected L1 norm (all values are positive 42)
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= gridsizes[d];  //Only vertex DOFs, so this is number of DOFs in each direction
    }

    T expected_norm1 = 42 * numPoints;
    T norm1 = container->norm(1);

    assertEqual<T>(expected_norm1, norm1);
}

TYPED_TEST(FEMContainerTest, AssignmentScalarFull) {
    // Test assignment of scalar to FEMContainer
    using T = typename TestFixture::value_type;
    unsigned constexpr Dim = TestFixture::dim;

    auto& container = this->femContainerFull;
    *container = 42;

    // Verify by computing expected L1 norm (all values are positive 42)
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= (2*gridsizes[d]-1);  //Vertey, edges, faces, volumes all have 1 DOF per entity, so total DOFs per direction is 2*nPoints[d]-1
    }

    T expected_norm1 = 42 * numPoints;
    T norm1 = container->norm(1);

    assertEqual<T>(expected_norm1, norm1);
}

TYPED_TEST(FEMContainerTest, AdditionScalarVertexOnly) {
    // Test operator+= with scalar
    using T = typename TestFixture::value_type;
    unsigned constexpr Dim = TestFixture::dim;

    auto& container = this->femContainerVertexOnly;
    *container = 2.0;

    *container += 3.0;

    // L1 norm should be 5.0 * numPoints
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= gridsizes[d];  //Only vertex DOFs, so this is number of DOFs in each direction
    }

    T expected_norm1 = 5.0 * numPoints;
    T norm1 = container->norm(1);

    assertEqual<T>(expected_norm1, norm1);
}

TYPED_TEST(FEMContainerTest, AdditionScalarFull) {
    // Test operator+= with scalar
    using T = typename TestFixture::value_type;
    unsigned constexpr Dim = TestFixture::dim;

    auto& container = this->femContainerFull;
    *container = 2.0;

    *container += 3.0;

    // L1 norm should be 5.0 * numPoints
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= (2*gridsizes[d]-1);  //Vertey, edges, faces, volumes all have 1 DOF per entity, so total DOFs per direction is 2*nPoints[d]-1
    }

    T expected_norm1 = 5.0 * numPoints;
    T norm1 = container->norm(1);

    assertEqual<T>(expected_norm1, norm1);
}

TYPED_TEST(FEMContainerTest, SubtractionScalarVertexOnly) {
    // Test operator-= with scalar
    using T = typename TestFixture::value_type;
    unsigned constexpr Dim = TestFixture::dim;

    auto& container = this->femContainerVertexOnly;
    *container = 10.0;

    *container -= 3.0;

    // L1 norm should be 7.0 * numPoints
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= gridsizes[d];  //Only vertex DOFs, so this is number of DOFs in each direction
    }

    T expected_norm1 = 7.0 * numPoints;
    T norm1 = container->norm(1);

    assertEqual<T>(expected_norm1, norm1);
}

TYPED_TEST(FEMContainerTest, SubtractionScalarFull) {
    // Test operator-= with scalar
    using T = typename TestFixture::value_type;
    unsigned constexpr Dim = TestFixture::dim;

    auto& container = this->femContainerFull;
    *container = 10.0;

    *container -= 3.0;

    // L1 norm should be 7.0 * numPoints
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= (2*gridsizes[d]-1);  //Vertey, edges, faces, volumes all have 1 DOF per entity, so total DOFs per direction is 2*nPoints[d]-1
    }

    T expected_norm1 = 7.0 * numPoints;
    T norm1 = container->norm(1);

    assertEqual<T>(expected_norm1, norm1);
}

TYPED_TEST(FEMContainerTest, AdditionContainersVertexOnly) {
    // Test operator+= with two FEMContainers
    using T = typename TestFixture::value_type;
    unsigned constexpr Dim = TestFixture::dim;

    auto& container1 = this->femContainerVertexOnly;
    *container1 = 2.0;

    typename TestFixture::femcontainer_vertex_only_type container2(*this->mesh, *this->layout);
    container2 = 3.0;

    *container1 += container2;

    // L1 norm should be 5.0 * numPoints
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= gridsizes[d];  //Only vertex DOFs, so this is number of DOFs in each direction
    }

    T expected_norm1 = 5.0 * numPoints;
    T norm1 = container1->norm(1);

    assertEqual<T>(expected_norm1, norm1);
}

TYPED_TEST(FEMContainerTest, AdditionContainersFull) {
    // Test operator+= with two FEMContainers
    using T = typename TestFixture::value_type;
    unsigned constexpr Dim = TestFixture::dim;

    auto& container1 = this->femContainerFull;
    *container1 = 2.0;

    typename TestFixture::femcontainer_full_type container2(*this->mesh, *this->layout);
    container2 = 3.0;

    *container1 += container2;

    // L1 norm should be 5.0 * numPoints
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= (2*gridsizes[d]-1);  //Vertey, edges, faces, volumes all have 1 DOF per entity, so total DOFs per direction is 2*nPoints[d]-1
    }

    T expected_norm1 = 5.0 * numPoints;
    T norm1 = container1->norm(1);

    assertEqual<T>(expected_norm1, norm1);
}

TYPED_TEST(FEMContainerTest, SubtractionContainersVertexOnly) {
    // Test operator-= with two FEMContainers
    using T = typename TestFixture::value_type;
    unsigned constexpr Dim = TestFixture::dim;

    auto& container1 = this->femContainerVertexOnly;
    *container1 = 10.0;

    typename TestFixture::femcontainer_vertex_only_type container2(*this->mesh, *this->layout);
    container2 = 3.0;

    *container1 -= container2;

    // L1 norm should be 7.0 * numPoints
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= gridsizes[d];  //Only vertex DOFs, so this is number of DOFs in each direction
    }

    T expected_norm1 = 7.0 * numPoints;
    T norm1 = container1->norm(1);

    assertEqual<T>(expected_norm1, norm1);
}

TYPED_TEST(FEMContainerTest, SubtractionContainersFull) {
    // Test operator-= with two FEMContainers
    using T = typename TestFixture::value_type;
    unsigned constexpr Dim = TestFixture::dim;

    auto& container1 = this->femContainerFull;
    *container1 = 10.0;

    typename TestFixture::femcontainer_full_type container2(*this->mesh, *this->layout);
    container2 = 3.0;

    *container1 -= container2;

    // L1 norm should be 7.0 * numPoints
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= (2*gridsizes[d]-1);  //Vertey, edges, faces, volumes all have 1 DOF per entity, so total DOFs per direction is 2*nPoints[d]-1
    }

    T expected_norm1 = 7.0 * numPoints;
    T norm1 = container1->norm(1);

    assertEqual<T>(expected_norm1, norm1);
}

TYPED_TEST(FEMContainerTest, BinaryAdditionVertexOnly) {
    // Test operator+ (binary addition)
    using T = typename TestFixture::value_type;
    unsigned constexpr Dim = TestFixture::dim;

    auto& container1 = this->femContainerVertexOnly;
    *container1 = 2.0;

    typename TestFixture::femcontainer_vertex_only_type container2(*this->mesh, *this->layout);
    container2 = 3.0;

    auto result = *container1 + container2;

    // Result L1 norm should be 5.0 * numPoints
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= gridsizes[d];  //Only vertex DOFs, so this is number of DOFs in each direction
    }
    T expected_norm1 = 5.0 * numPoints;
    T norm1 = result.norm(1);

    assertEqual<T>(expected_norm1, norm1);

    // Verify original containers are unchanged
    assertEqual<T>(container1->norm(1), 2.0 * numPoints);
    assertEqual<T>(container2.norm(1), 3.0 * numPoints);
}

TYPED_TEST(FEMContainerTest, BinaryAdditionFull) {
    // Test operator+ (binary addition)
    using T = typename TestFixture::value_type;
    unsigned constexpr Dim = TestFixture::dim;

    auto& container1 = this->femContainerFull;
    *container1 = 2.0;

    typename TestFixture::femcontainer_full_type container2(*this->mesh, *this->layout);
    container2 = 3.0;

    auto result = *container1 + container2;

    // Result L1 norm should be 5.0 * numPoints
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= (2*gridsizes[d]-1);  //Vertey, edges, faces, volumes all have 1 DOF per entity, so total DOFs per direction is 2*nPoints[d]-1
    }
    T expected_norm1 = 5.0 * numPoints;
    T norm1 = result.norm(1);

    assertEqual<T>(expected_norm1, norm1);

    // Verify original containers are unchanged
    assertEqual<T>(container1->norm(1), 2.0 * numPoints);
    assertEqual<T>(container2.norm(1), 3.0 * numPoints);
}

TYPED_TEST(FEMContainerTest, BinarySubtractionVertexOnly) {
    // Test operator- (binary subtraction)
    using T = typename TestFixture::value_type;
    unsigned constexpr Dim = TestFixture::dim;

    auto& container1 = this->femContainerVertexOnly;
    *container1 = 10.0;

    typename TestFixture::femcontainer_vertex_only_type container2(*this->mesh, *this->layout);
    container2 = 3.0;

    auto result = *container1 - container2;

    // Result L1 norm should be 7.0 * numPoints
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= gridsizes[d];  //Only vertex DOFs, so this is number of DOFs in each direction
    }

    T expected_norm1 = 7.0 * numPoints;
    T norm1 = result.norm(1);

    assertEqual<T>(expected_norm1, norm1);

    // Verify original containers are unchanged
    assertEqual<T>(container1->norm(1), 10.0 * numPoints);
    assertEqual<T>(container2.norm(1), 3.0 * numPoints);
}

TYPED_TEST(FEMContainerTest, BinarySubtractionFull) {
    // Test operator- (binary subtraction)
    using T = typename TestFixture::value_type;
    unsigned constexpr Dim = TestFixture::dim;

    auto& container1 = this->femContainerFull;
    *container1 = 10.0;

    typename TestFixture::femcontainer_full_type container2(*this->mesh, *this->layout);
    container2 = 3.0;

    auto result = *container1 - container2;

    // Result L1 norm should be 7.0 * numPoints
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= (2*gridsizes[d]-1);  //Vertey, edges, faces, volumes all have 1 DOF per entity, so total DOFs per direction is 2*nPoints[d]-1
    }

    T expected_norm1 = 7.0 * numPoints;
    T norm1 = result.norm(1);

    assertEqual<T>(expected_norm1, norm1);

    // Verify original containers are unchanged
    assertEqual<T>(container1->norm(1), 10.0 * numPoints);
    assertEqual<T>(container2.norm(1), 3.0 * numPoints);
}

TYPED_TEST(FEMContainerTest, Norm1ContainerVertexOnly) {
    // Test L1 norm (p=1)
    using T = typename TestFixture::value_type;
    constexpr unsigned Dim = TestFixture::dim;

    auto& container = this->femContainerVertexOnly;
    *container = -2.0;

    T norm1 = container->norm(1);

    // L1 norm should be sum of absolute values = 2.0 * numPoints
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= gridsizes[d];  //Only vertex DOFs, so this is number of DOFs in each direction
    }

    T expected = 2.0 * numPoints;

    assertEqual<T>(expected, norm1);
}

TYPED_TEST(FEMContainerTest, Norm1ContainerFull) {
    // Test L1 norm (p=1)
    using T = typename TestFixture::value_type;
    constexpr unsigned Dim = TestFixture::dim;

    auto& container = this->femContainerFull;
    *container = -2.0;

    T norm1 = container->norm(1);

    // L1 norm should be sum of absolute values = 2.0 * numPoints
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= (2*gridsizes[d]-1);  //Vertey, edges, faces, volumes all have 1 DOF per entity, so total DOFs per direction is 2*nPoints[d]-1
    }

    T expected = 2.0 * numPoints;

    assertEqual<T>(expected, norm1);
}

TYPED_TEST(FEMContainerTest, Norm2ContainerVertexOnly) {
    // Test L2 norm (p=2)
    using T = typename TestFixture::value_type;
    constexpr unsigned Dim = TestFixture::dim;

    auto& container = this->femContainerVertexOnly;
    *container = 3.0;

    T norm2 = container->norm(2);

    // L2 norm should be sqrt(sum of squares) = sqrt(9.0 * numPoints)
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= gridsizes[d];  //Only vertex DOFs, so this is number of DOFs in each direction
    }
    T expected = std::sqrt(9.0 * numPoints);

    assertEqual<T>(expected, norm2);
}

TYPED_TEST(FEMContainerTest, Norm2ContainerFull) {
    // Test L2 norm (p=2)
    using T = typename TestFixture::value_type;
    constexpr unsigned Dim = TestFixture::dim;

    auto& container = this->femContainerFull;
    *container = 3.0;

    T norm2 = container->norm(2);

    // L2 norm should be sqrt(sum of squares) = sqrt(9.0 * numPoints)
    size_t numPoints = 1;
    ippl::Vector<T, Dim> gridsizes = this->mesh->getGridsize();
    for(unsigned d=0; d<Dim; d++){
        numPoints *= (2*gridsizes[d]-1);  //Vertey, edges, faces, volumes all have 1 DOF per entity, so total DOFs per direction is 2*nPoints[d]-1
    }
    T expected = std::sqrt(9.0 * numPoints);

    assertEqual<T>(expected, norm2);
}

TYPED_TEST(FEMContainerTest, NormZeroContainerVertexOnly) {
    // Test norm of zero container
    using T = typename TestFixture::value_type;
    
    auto& container = this->femContainerVertexOnly;
    *container = 0.0;
    
    T norm1 = container->norm(1);
    T norm2 = container->norm(2);
    
    assertEqual<T>(0.0, norm1);
    assertEqual<T>(0.0, norm2);
}

TYPED_TEST(FEMContainerTest, NormZeroContainerFull) {
    // Test norm of zero container
    using T = typename TestFixture::value_type;
    
    auto& container = this->femContainerFull;
    *container = 0.0;
    
    T norm1 = container->norm(1);
    T norm2 = container->norm(2);
    
    assertEqual<T>(0.0, norm1);
    assertEqual<T>(0.0, norm2);
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
