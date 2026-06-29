#include "Ippl.h"

#include <fstream>
#include <sstream>

// Generate CSV samples of reference-element Lagrange basis functions for
// Track B `LagrangeSpace_wfc`, for manual plotting / inspection.
//
// Usage (examples):
//   mpirun -n 1 ./GenerateLagrangeBasisSamples --dim 1
//   mpirun -n 1 ./GenerateLagrangeBasisSamples --dim 2
//
// For dim=1 this writes `1D_lagrange_local_basis_orderP.csv` files.
// For dim=2 this writes `2D_lagrange_local_basis_orderP.csv` files.

namespace {

template <typename T, unsigned Order>
void run1DOrder() {
    constexpr unsigned Dim = 1;
    using MeshType         = ippl::UniformCartesian<T, Dim>;
    using ElementType      = ippl::EdgeElement<T>;
    using QuadratureType   = ippl::MidpointQuadrature<T, 1, ElementType>;

    const unsigned number_of_vertices = 10;
    const unsigned number_of_elements = number_of_vertices - 1;
    const T interval_size             = 2.0;
    MeshType mesh(number_of_vertices, {interval_size / number_of_elements}, {-1.0});

    std::array<bool, Dim> isParallel;
    isParallel.fill(true);
    ippl::FieldLayout<Dim> layout(MPI_COMM_WORLD, number_of_vertices, isParallel);

    using DOFHandler_t =
        ippl::DOFHandler<T, ippl::FiniteElementSpaceTraits<ippl::LagrangeSpaceTag, Dim, Order>>;
    using FieldType = typename DOFHandler_t::FEMContainer_t;

    ElementType ref_element;
    const QuadratureType midpoint_quadrature(ref_element);

    const unsigned number_of_local_vertices  = 2;
    const unsigned number_of_local_edge_dofs = Order - 1;

    const ippl::LagrangeSpace_wfc<T, Dim, Order, ElementType, QuadratureType, FieldType, FieldType>
        lagrange_space(mesh, ref_element, midpoint_quadrature, layout);

    const unsigned number_of_points = 200;
    const T dx                      = interval_size / (number_of_points - 1);

    const std::string local_basis_filename =
        "1D_lagrange_local_basis_order" + std::to_string(Order) + ".csv";
    std::cout << "Writing 1D local basis functions (order=" << Order << ") to "
              << local_basis_filename << "\n";
    std::ofstream local_basis_out(local_basis_filename, std::ios::out);

    local_basis_out << "x";
    for (unsigned i = 0; i < number_of_local_vertices; ++i) {
        local_basis_out << ",v_" << i;
    }
    for (unsigned i = number_of_local_vertices;
         i < number_of_local_vertices + number_of_local_edge_dofs; ++i) {
        local_basis_out << ",e_" << i;
    }
    local_basis_out << "\n";

    for (ippl::Vector<T, 1> x = {0.0}; x[0] <= 1.0; x[0] += dx) {
        local_basis_out << x[0];
        for (unsigned i = 0; i < number_of_local_vertices; ++i) {
            local_basis_out << "," << lagrange_space.evaluateRefElementShapeFunction(i, x);
        }
        for (unsigned i = number_of_local_vertices;
             i < number_of_local_vertices + number_of_local_edge_dofs; ++i) {
            local_basis_out << "," << lagrange_space.evaluateRefElementShapeFunction(i, x);
        }
        local_basis_out << "\n";
    }
}

template <typename T>
void run1D() {
    run1DOrder<T, 1>();
    run1DOrder<T, 2>();
    run1DOrder<T, 3>();
    run1DOrder<T, 4>();
}

template <typename T, unsigned Order>
void run2DOrder() {
    constexpr unsigned Dim = 2;

    const unsigned number_of_points_per_dim   = 200;
    const unsigned number_of_vertices_per_dim = 5;
    const unsigned number_of_elements_per_dim = number_of_vertices_per_dim - 1;
    const double interval_size                = 2.0;
    const double h                            = interval_size / number_of_elements_per_dim;
    const double dx                           = interval_size / number_of_points_per_dim;

    using MeshType       = ippl::UniformCartesian<T, Dim>;
    using ElementType    = ippl::QuadrilateralElement<T>;
    using QuadratureType = ippl::MidpointQuadrature<T, 1, ElementType>;

    const ippl::NDIndex<2> meshIndex(number_of_vertices_per_dim, number_of_vertices_per_dim);
    MeshType mesh(meshIndex, {h, h}, {-1.0, -1.0});

    std::array<bool, Dim> isParallel;
    isParallel.fill(true);
    ippl::FieldLayout<Dim> layout(MPI_COMM_WORLD, meshIndex, isParallel);

    using DOFHandler_t =
        ippl::DOFHandler<T, ippl::FiniteElementSpaceTraits<ippl::LagrangeSpaceTag, Dim, Order>>;
    using FieldType = typename DOFHandler_t::FEMContainer_t;

    ElementType quad_element;
    const QuadratureType midpoint_quadrature(quad_element);

    const ippl::LagrangeSpace_wfc<T, Dim, Order, ElementType, QuadratureType, FieldType, FieldType>
        lagrange_space(mesh, quad_element, midpoint_quadrature, layout);

    const unsigned number_of_local_vertices  = 4;
    const unsigned number_of_local_edge_dofs = 4 * (Order - 1);
    const unsigned number_of_local_face_dofs = (Order - 1) * (Order - 1);
    const unsigned total_local_dofs =
        number_of_local_vertices + number_of_local_edge_dofs + number_of_local_face_dofs;

    const std::string local_basis_filename =
        "2D_lagrange_local_basis_order" + std::to_string(Order) + ".csv";
    std::cout << "Writing 2D local basis functions (order=" << Order << ") to "
              << local_basis_filename << "\n";
    std::ofstream local_basis_out(local_basis_filename, std::ios::out);

    local_basis_out << "x,y";
    for (unsigned i = 0; i < number_of_local_vertices; ++i) {
        local_basis_out << ",v_" << i;
    }
    for (unsigned i = number_of_local_vertices;
         i < number_of_local_vertices + number_of_local_edge_dofs; ++i) {
        local_basis_out << ",e_" << i;
    }
    for (unsigned i = number_of_local_vertices + number_of_local_edge_dofs; i < total_local_dofs;
         ++i) {
        local_basis_out << ",f_" << i;
    }
    local_basis_out << "\n";

    for (double x = 0.0; x <= 1.0; x += dx) {
        for (double y = 0.0; y <= 1.0; y += dx) {
            local_basis_out << x << "," << y;
            for (unsigned i = 0; i < total_local_dofs; ++i) {
                local_basis_out << ","
                                << lagrange_space.evaluateRefElementShapeFunction(
                                       i, ippl::Vector<T, 2>{x, y});
            }
            local_basis_out << "\n";
        }
    }
}

template <typename T>
void run2D() {
    run2DOrder<T, 1>();
    run2DOrder<T, 2>();
    run2DOrder<T, 3>();
    run2DOrder<T, 4>();
}

unsigned parse_dim(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--dim" && i + 1 < argc) {
            return static_cast<unsigned>(std::stoul(argv[i + 1]));
        }
    }
    return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);
    {
        const unsigned dim = parse_dim(argc, argv);
        using T            = double;

        if (dim == 1) {
            run1D<T>();
        } else if (dim == 2) {
            run2D<T>();
        } else {
            std::cerr << "GenerateLagrangeBasisSamples: only --dim 1 or 2 are supported.\n";
        }
    }
    ippl::finalize();
    return 0;
}
