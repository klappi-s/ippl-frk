// Integration-test driver. Restore and save at the same completed step so a
// change of MPI decomposition can be tested independently of the FEL integrator.
#include "Ippl.h"

#include "FreeElectronLaserManager.h"

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    try {
        if (argc < 3)
            throw std::runtime_error("Expected checkpoint directory and output directory");
        const std::filesystem::path source = argv[1];
        auto cfg                           = read_config((source / "config.json").c_str());
        cfg.output_path                    = std::string(argv[2]) + "/";
        FreeElectronLaserManager<double, 3> manager(cfg);
        manager.setRestartPath(source);
        manager.pre_run();
        FELCheckpoint::save(manager);
#ifdef IPPL_ENABLE_CATALYST
        manager.cat_viz.Finalize();
#endif
    } catch (const IpplException& e) {
        std::cerr << e.what() << std::endl;
        ippl::Comm->abort(1);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        ippl::Comm->abort(1);
    }
    ippl::finalize();
}
