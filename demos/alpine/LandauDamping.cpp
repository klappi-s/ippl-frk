// Landau Damping Test
//   Usage:
//     srun ./LandauDamping
//                  <nx> [<ny>...] <Np> <Nt> <stype> <lbthres>
//                  <t_method> --overallocate <ovfactor> --info 10
//     nx       = No. cell-centered points in the x-direction
//     ny...    = No. cell-centered points in the y-, z-, ...-direction
//     Np       = Total no. of macro-particles in the simulation
//     Nt       = Number of time steps
//     stype    = Field solver type (FFT and CG supported)
//     lbthres  = Load balancing threshold i.e., lbthres*100 is the maximum load imbalance
//                percentage which can be tolerated and beyond which
//                particle load balancing occurs. A value of 0.01 is good for many typical
//                simulations.
//     t_method = Time-stepping method used e.g. Leapfrog
//     ovfactor = Over-allocation factor for the buffers used in the communication. Typical
//                values are 1.0, 2.0. Value 1.0 means no over-allocation.
//     Example:
//     srun ./LandauDamping 128 128 128 10000 10 FFT 0.01 LeapFrog --overallocate 2.0 --info 10

constexpr unsigned Dim = 3;
using T                = double;
const char* TestName   = "LandauDamping";

#include "Ippl.h"

#include <Kokkos_MathematicalConstants.hpp>
#include <Kokkos_MathematicalFunctions.hpp>
#include <Kokkos_Random.hpp>
#include <chrono>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "Manager/datatypes.h"

#include "Utility/IpplTimings.h"

#include "LandauDampingManager.h"
#include "Manager/PicManager.h"

int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);
    int exit_code = 0;
    {
        try {
            Inform msg(TestName);
            Inform msg2all(TestName, INFORM_ALL_NODES);

            static IpplTimings::TimerRef mainTimer       = IpplTimings::getTimer("total");
            static IpplTimings::TimerRef initializeTimer = IpplTimings::getTimer("initialize");
            IpplTimings::startTimer(mainTimer);
            IpplTimings::startTimer(initializeTimer);

            // Read input parameters, assign them to the corresponding memebers of manager
            if (argc < 9)
                throw std::runtime_error("Usage: LandauDamping nx ny nz Np Nt CG|PCG lbthres LeapFrog [preconditioner arguments] [--benchmark-output DIR --benchmark-tolerance 1e-10 --benchmark-max-iterations 10000 --benchmark-dt 0.05]");
            int arg = 1;
            Vector_t<int, Dim> nr;
            for (unsigned d = 0; d < Dim; d++) {
                nr[d] = std::atoi(argv[arg++]);
            }

            size_type totalP   = std::atoll(argv[arg++]);
            int nt             = std::atoi(argv[arg++]);
            std::string solver = argv[arg++];

            double lbt              = std::atof(argv[arg++]);
            std::string step_method = argv[arg++];
            if (totalP < 1 || nt < 0 || !std::isfinite(lbt))
                throw std::runtime_error("Invalid particle count, steps or balancing threshold");
            for (unsigned d = 0; d < Dim; ++d)
                if (nr[d] < 2) throw std::runtime_error("At least two cells per axis required");

            std::vector<std::string> preconditioner_params;

            if (solver == "PCG" || solver == "FEM_PRECON") {
                while (arg < argc) {
                    const std::string token = argv[arg];
                    if (token.rfind("--", 0) == 0) {
                        break;
                    }
                    preconditioner_params.push_back(token);
                    ++arg;
                }
            }

            std::string benchmarkOutput;
            double benchmarkTolerance = 1e-10, benchmarkDt = .05;
            int benchmarkMaxIterations = 10000;
            while (arg < argc) {
                const std::string key = argv[arg++];
                if (arg == argc) throw std::runtime_error("Missing value for " + key);
                const std::string value = argv[arg++];
                size_t used = 0;
                if (key == "--benchmark-output") benchmarkOutput = value;
                else if (key == "--benchmark-tolerance") {
                    benchmarkTolerance = std::stod(value, &used);
                    if (used != value.size()) throw std::runtime_error("Invalid tolerance");
                } else if (key == "--benchmark-dt") {
                    benchmarkDt = std::stod(value, &used);
                    if (used != value.size()) throw std::runtime_error("Invalid timestep");
                } else if (key == "--benchmark-max-iterations") {
                    benchmarkMaxIterations = std::stoi(value, &used);
                    if (used != value.size()) throw std::runtime_error("Invalid iteration limit");
                } else if (key != "--overallocate" && key != "--info" && key != "--timer-fences")
                    throw std::runtime_error("Unknown option: " + key);
            }

            // Create an instance of a manager for the considered application
            LandauDampingManager<T, Dim> manager(totalP, nt, nr, lbt, solver, step_method,
                                                 preconditioner_params);
            if (!benchmarkOutput.empty())
                manager.enableBenchmark(benchmarkOutput, benchmarkTolerance, benchmarkMaxIterations, benchmarkDt);

            // Perform pre-run operations, including creating mesh, particles,...
            manager.pre_run();

            IpplTimings::stopTimer(initializeTimer);

            manager.setTime(0.0);

            msg << "Starting iterations ..." << endl;

            manager.run(manager.getNt());
            manager.flushBenchmark();

            msg << "End." << endl;

            IpplTimings::stopTimer(mainTimer);
            IpplTimings::print();
            IpplTimings::print(std::string("timing.dat"));
            if (!benchmarkOutput.empty())
                IpplTimings::dumpSummaryToCSV("timer-summary.csv");
        } catch (const IpplException& ex) {
            Inform err(TestName);
            err << "IPPL exception: " << ex.what() << endl;
            exit_code = 1;
        } catch (const std::exception& ex) {
            Inform err(TestName);
            err << "Unhandled std::exception: " << ex.what() << endl;
            exit_code = 1;
        } catch (...) {
            Inform err(TestName);
            err << "Unhandled unknown exception" << endl;
            exit_code = 1;
        }
    }
    ippl::finalize();

    return exit_code;
}
