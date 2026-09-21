#include "ExperimentConfig.h"
#include "FEMDemoDispatch.h"

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout
            << "LandauDampingFEM --dim 1|2|3 --order 1..5 --cells N[,N,N] --particles N --steps "
               "10\n"
               "  --solver plain|preconditioned --preconditioner_type "
               "jacobi|newton|chebyshev|richardson|richardson_alt|gauss-seidel|ssor\n"
               "  --reuse off|operator|full --initial_guess zero|previous --quadrature_points "
               "9|exact\n"
               "  --interpolation_nodes gll|equispaced --tolerance 1e-10 --max_iterations 10000\n"
               "  --timer_level coarse|detailed --replay plain,jacobi,chebyshev --output "
               "DIRECTORY\n"
               "See demos/electrostaticFEM/README.md for physics and preconditioner controls.\n";
        return 0;
    }
    ippl::initialize(argc, argv);
    int result = 0;
    try {
        const auto config = electrostatic_fem::ExperimentConfig::parse(argc, argv);
        electrostatic_fem::dispatch(config);
    } catch (const std::exception& error) {
        std::cerr << "LandauDampingFEM rank " << ippl::Comm->rank() << ": " << error.what() << '\n';
        result = 1;
    } catch (const IpplException& error) {
        std::cerr << "LandauDampingFEM rank " << ippl::Comm->rank() << ": " << error.what() << '\n';
        result = 1;
    }
    ippl::finalize();
    return result;
}
