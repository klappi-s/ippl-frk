//
////
//// TestFEMContainerSize.cpp
////   Tis program to test the creation of a FEMContainer with different sizes. It
////   creates a FEMContainer with DOFs on all element types, and sets the DOFs to 1.0.
////
//// Usage:
////   srun ./TestFEMContainerSize 10 false --info 10
////
////
//

#include "Ippl.h"

#include <array>
#include <iostream>
#include <typeinfo>

int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);
    {
        constexpr unsigned int dim = 3;
        using Mesh_t               = ippl::UniformCartesian<double, dim>;
        using Centering_t          = Mesh_t::DefaultCentering;
        using Layout_t            = ippl::FieldLayout<dim>;

        // Get number of points and lagrange order from command line
        int pt         = std::atoi(argv[1]);
        constexpr unsigned order = 3;

        // true to create a field, false to create a FEMContainer
        bool field = std::atoi(argv[2]);

        // For field, we use h refinemendt, so increase number of points
        // according to the order of the Lagrange polynomial
        if(field) {
           pt = order * (pt - 1) + 1;
        }

        ippl::Index I(pt);
        ippl::NDIndex<dim> owned(I, I, I);

        // Specifies SERIAL, PARALLEL dims
        std::array<bool, dim> isParallel;
        isParallel.fill(true);

        // all parallel layout, standard domain, normal axis order
        Layout_t layout(MPI_COMM_WORLD, owned, isParallel);

        // domain [0,1]^3
        double dx                      = 1.0 / double(pt);
        ippl::Vector<double, 3> hx     = {dx, dx, dx};
        ippl::Vector<double, 3> origin = {0.0, 0.0, 0.0};
        Mesh_t mesh(owned, hx, origin);


        if(field) {
            // create field
            ippl::Field<double, dim, Mesh_t, Centering_t> field(mesh, layout, 1);

            field = 1.0;

            std::cout << "Field created with order " << order << " and set to 1.0." << std::endl;
        }
        else {
            // Number of DOFs equal to 1 for each element
            using femcontainer_full_type = std::conditional_t<
                                                order == 1,
                                                ippl::FEMContainer<double, dim,
                                                    std::tuple<ippl::Vertex<dim>>,
                                                    std::tuple<std::integral_constant<unsigned, 1>>>,
                                                ippl::FEMContainer<double, dim,
                                                    std::tuple<ippl::Vertex<dim>, ippl::EdgeX<dim>, ippl::EdgeY<dim>, ippl::EdgeZ<dim>,
                                                            ippl::FaceXY<dim>, ippl::FaceXZ<dim>, ippl::FaceYZ<dim>, ippl::Hexahedron<dim>
                                                    >,
                                                    std::tuple<std::integral_constant<unsigned, 1>,
                                                            std::integral_constant<unsigned, order - 1>,
                                                            std::integral_constant<unsigned, order - 1>,
                                                            std::integral_constant<unsigned, order - 1>,
                                                            std::integral_constant<unsigned, (order - 1) * (order - 1)>,
                                                            std::integral_constant<unsigned, (order - 1) * (order - 1)>,
                                                            std::integral_constant<unsigned, (order - 1) * (order - 1)>,
                                                            std::integral_constant<unsigned, (order - 1) * (order - 1) * (order - 1)>
                                                    >
                                                > // Full DOFs
                                            >;

            femcontainer_full_type femContainer(mesh, layout);

            femContainer = 1.0;

            std::cout << "FEMContainer with order " << order << " created with DOFs on all element types." << std::endl;
        }
    }
    ippl::finalize();

    return 0;
}
