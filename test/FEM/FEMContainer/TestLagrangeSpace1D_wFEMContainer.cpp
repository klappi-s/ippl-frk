#include "Ippl.h"

#include <fstream>

// #include <typeinfo>

template <typename T, unsigned Dim, unsigned Order>
void runLagrangeSpaceTest() {
    using MeshType       = ippl::UniformCartesian<T, Dim>;
    using ElementType    = ippl::EdgeElement<T>;
    using QuadratureType = ippl::MidpointQuadrature<T, 1, ElementType>;

    // Create a 1D uniform mesh centered at 0.0.
    const unsigned number_of_vertices = 10;
    const unsigned number_of_elements = number_of_vertices - 1;
    const T interval_size             = 2.0;
    MeshType mesh(number_of_vertices, {interval_size / number_of_elements}, {-1.0});

    // specifies decomposition; here all dimensions are parallel
    std::array<bool, Dim> isParallel;
    isParallel.fill(true);
    ippl::FieldLayout<Dim> layout(MPI_COMM_WORLD, number_of_vertices, isParallel);

    using DOFHandler_t = ippl::DOFHandler<T, ippl::FiniteElementSpaceTraits<ippl::LagrangeSpaceTag, Dim, Order>>;
    using FieldType    = typename DOFHandler_t::FEMContainer_t;

    // Reference element
    ElementType ref_element;

    // Create Midpoint Quadrature
    const ippl::MidpointQuadrature<T, 1, ElementType> midpoint_quadrature(ref_element);

    // Create LagrangeSpace_wfc
    const unsigned number_of_local_vertices  = 2;
    const unsigned number_of_local_edge_dofs = Order - 1;

    const ippl::LagrangeSpace_wfc<T, Dim, Order, ElementType, QuadratureType, FieldType, FieldType>
        lagrange_space(mesh, ref_element, midpoint_quadrature, layout);

    // Print the local basis values for plotting
    const unsigned number_of_points = 200;
    const T dx                      = interval_size / (number_of_points - 1);

    const std::string local_basis_filename = "1D_lagrange_local_basis_order" + std::to_string(Order) + ".csv";
    std::cout << "Writing local basis functions to " << local_basis_filename << "\n";
    std::ofstream local_basis_out(local_basis_filename, std::ios::out);

    local_basis_out << "x";
    for (unsigned i = 0; i < number_of_local_vertices; ++i) {
        local_basis_out << ",v_" << i;
    }
    for (unsigned i = number_of_local_vertices; i < number_of_local_vertices + number_of_local_edge_dofs; ++i) {
        local_basis_out << ",e_" << i;
    }

    local_basis_out << "\n";

    for (ippl::Vector<T, 1> x = {0.0}; x[0] <= 1.0; x[0] += dx) {
        local_basis_out << x[0];
        for (unsigned i = 0; i < number_of_local_vertices; ++i) {
            local_basis_out << "," << lagrange_space.evaluateRefElementShapeFunction(i, x);
        }
        for (unsigned i = number_of_local_vertices; i < number_of_local_vertices + number_of_local_edge_dofs; ++i) {
            local_basis_out << "," << lagrange_space.evaluateRefElementShapeFunction(i, x);
        }
        local_basis_out << "\n";
    }
}

int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);
    {
        using T                = double;
        constexpr unsigned Dim = 1;

        // Run tests for all orders
        runLagrangeSpaceTest<T, Dim, 1>();
        runLagrangeSpaceTest<T, Dim, 2>();
        runLagrangeSpaceTest<T, Dim, 3>();
        runLagrangeSpaceTest<T, Dim, 4>();
    }
    ippl::finalize();

    return 0;
}
