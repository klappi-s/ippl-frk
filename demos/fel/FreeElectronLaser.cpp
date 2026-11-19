// Free Electron Laser simulation.
//
//   Usage:
//     srun ./FreeElectronLaser [<config.json>] --info [0-5]
//    or
//    mpirun -np [N] ./FreeElectronLaser  [<config.json>] --info [0-5]
//
//   Reads a MITHRA-style JSON job file (by default the config.json staged next to
//   the build-tree executable) describing the grid, the relativistic electron
//   bunch, and the undulator. The simulation
//   runs in a Lorentz frame co-moving with the bunch: a charge-conserving
//   current is deposited onto the grid, Maxwell's equations are advanced with a
//   standard FDTD solver (absorbing boundaries), and the particles are pushed
//   with a relativistic Boris pusher that also feels the (frame-transformed)
//   undulator field. Radiated power is written to a CSV and a
//   narrow-band radiation diagnostic is produced alongside it.



    // "resolution": [96, 96, 3000],
    // "resolution": [48, 48, 1500],

constexpr unsigned Dim = 3;
using T                = double;

#include "Ippl.h"

#include <Kokkos_Core.hpp>
#include <string>

#include "Utility/IpplTimings.h"

#include "FreeElectronLaserManager.h"

#ifndef IPPL_FEL_DEFAULT_CONFIG
#define IPPL_FEL_DEFAULT_CONFIG "config.json"
#endif

int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);
    int result = 0;
    try {
        Inform msg("FreeElectronLaser");

        static IpplTimings::TimerRef mainTimer = IpplTimings::getTimer("total");
        IpplTimings::startTimer(mainTimer);

        std::filesystem::path restartPath;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--restart") {
                if (++i == argc || argv[i][0] == '-' || !restartPath.empty())
                    throw std::runtime_error("--restart requires one checkpoint directory");
                restartPath = argv[i];
            }
        }
        std::filesystem::path configPath = IPPL_FEL_DEFAULT_CONFIG;
        if (!restartPath.empty()) {
#ifdef IPPL_ENABLE_OPENPMD
            std::error_code error;
            const auto directory = std::filesystem::canonical(restartPath, error);
            FELCheckpoint::requireRestart(!error && directory.extension() != ".partial"
                                              && std::filesystem::is_regular_file(directory / "config.json", error),
                                          "Incomplete FEL checkpoint: " + restartPath.string());
            configPath = directory / "config.json";
#else
            throw std::runtime_error("FEL checkpoints require IPPL_ENABLE_OPENPMD=ON");
#endif
        }
        if (argc > 1 && argv[1][0] != '-') configPath = argv[1];
        msg << "Reading configuration from " << configPath.string() << endl;
        config cfg = read_config(configPath.c_str());
#ifndef IPPL_ENABLE_OPENPMD
        if (cfg.checkpointInterval)
            throw std::runtime_error("FEL checkpoints require IPPL_ENABLE_OPENPMD=ON");
#endif
        FreeElectronLaserManager<T, Dim> manager(cfg);
        manager.setRestartPath(restartPath);

        // Pre-run: build mesh, fields, particles, FDTD solver; derive dt and nt.
        manager.pre_run();

        msg << "Starting iterations ..." << endl;

        manager.run(manager.remainingSteps());

#ifdef IPPL_ENABLE_CATALYST
        manager.cat_viz.Finalize();
#endif

        msg << "End." << endl;

        IpplTimings::stopTimer(mainTimer);
        IpplTimings::print();
        IpplTimings::print(std::string("timing.dat"));
#ifdef IPPL_ENABLE_OPENPMD
    } catch (const FELCheckpoint::InvalidRestart& error) {
        if (ippl::Comm->rank() == 0) std::cerr << "FEL: " << error.what() << std::endl;
        result = 1;
#endif
    } catch (const IpplException& error) {
        std::cerr << "FEL: " << error.where() << ": " << error.what() << std::endl;
        ippl::Comm->abort(1);
    } catch (const std::exception& error) {
        std::cerr << "FEL: " << error.what() << std::endl;
        ippl::Comm->abort(1);
    }
    ippl::finalize();

    return result;
}
