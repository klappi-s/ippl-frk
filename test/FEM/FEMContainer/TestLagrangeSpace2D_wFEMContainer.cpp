#include "Ippl.h"

#include <fstream>

// #include <typeinfo>

template <typename T, unsigned Dim, unsigned Order>
void runLagrangeSpaceTest() {
    const unsigned number_of_points_per_dim = 200;

    // Create a 2D uniform mesh centered at 0.0.
    const unsigned number_of_vertices_per_dim = 5;
    const unsigned number_of_elements_per_dim = number_of_vertices_per_dim - 1;
    const double interval_size                = 2.0;
    const double h                            = interval_size / (number_of_elements_per_dim);
    const double dx                           = interval_size / number_of_points_per_dim;

    using MeshType       = ippl::UniformCartesian<T, Dim>;
    using ElementType    = ippl::QuadrilateralElement<T>;
    using QuadratureType = ippl::MidpointQuadrature<T, 1, ElementType>;

    const ippl::NDIndex<2> meshIndex(number_of_vertices_per_dim, number_of_vertices_per_dim);
    MeshType mesh(meshIndex, {h, h}, {-1.0, -1.0});
    // specifies decomposition; here all dimensions are parallel
    std::array<bool, Dim> isParallel;
    isParallel.fill(true);
    ippl::FieldLayout<Dim> layout(MPI_COMM_WORLD, meshIndex, isParallel);

    using DOFHandler_t = ippl::DOFHandler<T, ippl::FiniteElementSpaceTraits<ippl::LagrangeSpaceTag, Dim, Order>>;
    using FieldType    = typename DOFHandler_t::FEMContainer_t;

    // Reference element
    ElementType quad_element;

    // Create Midpoint Quadrature
    const ippl::MidpointQuadrature<T, 1, ElementType> midpoint_quadrature(quad_element);

    // Create LagrangeSpace_wfc
    const ippl::LagrangeSpace_wfc<T, Dim, Order, ElementType, QuadratureType, FieldType, FieldType>
        lagrange_space(mesh, quad_element, midpoint_quadrature, layout);

    // Calculate number of DOFs per element type for 2D quadrilateral
    const unsigned number_of_local_vertices  = 4;  // 4 vertices for quad
    const unsigned number_of_local_edge_dofs = 4 * (Order - 1);  // 4 edges with (Order-1) DOFs each
    const unsigned number_of_local_face_dofs = (Order - 1) * (Order - 1);  // interior face DOFs
    const unsigned total_local_dofs = number_of_local_vertices + number_of_local_edge_dofs + number_of_local_face_dofs;

    // Print the values for the local basis functions
    const std::string local_basis_filename = "2D_lagrange_local_basis_order" + std::to_string(Order) + ".csv";
    std::cout << "Writing local basis function to " << local_basis_filename << "\n";
    std::ofstream local_basis_out(local_basis_filename, std::ios::out);

    local_basis_out << "x,y";
    for (unsigned i = 0; i < number_of_local_vertices; ++i) {
        local_basis_out << ",v_" << i;
    }
    for (unsigned i = number_of_local_vertices; i < number_of_local_vertices + number_of_local_edge_dofs; ++i) {
        local_basis_out << ",e_" << i;
    }
    for (unsigned i = number_of_local_vertices + number_of_local_edge_dofs; i < total_local_dofs; ++i) {
        local_basis_out << ",f_" << i;
    }
    local_basis_out << "\n";

    for (double x = 0.0; x <= 1.0; x += dx) {
        for (double y = 0.0; y <= 1.0; y += dx) {
            local_basis_out << x << "," << y;
            for (unsigned i = 0; i < total_local_dofs; ++i) {
                local_basis_out << ","
                                << lagrange_space.evaluateRefElementShapeFunction(i, {x, y});
            }
            local_basis_out << "\n";
        }
    }
    local_basis_out.close();
}

int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);
    {
        Inform out("Test LagrangeSpace2DMidpoint");

        using T                = double;
        constexpr unsigned Dim = 2;

        // Run tests for all orders
        runLagrangeSpaceTest<T, Dim, 1>();
        runLagrangeSpaceTest<T, Dim, 2>();
        runLagrangeSpaceTest<T, Dim, 3>();
        runLagrangeSpaceTest<T, Dim, 4>();
    }
    ippl::finalize();

    return 0;
}
