#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

#include "LandauDampingStreamManager.h"

const char* TestName = "LandauDampingStream";

namespace {
    void usage() {
        std::cout << "LandauDampingStream nx ny nz totalParticles steps solver lbThreshold LeapFrog\n"
                     "  [preconditioner arguments] [--output-dir PATH] [--io-backend hdf5|bp5] [--no-output]\n"
                     "  [--output-kind particles|fields]\n"
                     "  [--staging-mib 64] [--length-unit-si 1] [--time-unit-si 1] [IPPL options]\n"
                     "Writes positions, velocities and IDs (default), or normalized E and rho.\n"
                     "Both modes write after every timestep; fields exclude ghost cells.\n"
                     "totalParticles is a global count, not particles per cell.\n"
                     "For PCG, supply a preconditioner such as jacobi after LeapFrog.\n"
                     "Output path: --output-dir overrides IPPL_OPENPMD_OUTPUT_DIR; default openpmd.\n"
                     "HDF5 is the default; BP5 uses ADIOS2. PATH must not already exist.\n";
    }

    template <class Integer>
    Integer integer(std::string_view text, const char* name) {
        Integer value{};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value <= 0)
            throw IpplException(TestName, std::string("Expected a positive integer for ") + name);
        return value;
    }

    double number(const std::string& text, const char* name) {
        std::size_t end;
        const double value = std::stod(text, &end);
        if (end != text.size() || !std::isfinite(value))
            throw IpplException(TestName, std::string("Invalid number for ") + name);
        return value;
    }
}  // namespace

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--help" || std::string_view(argv[i]) == "-h") {
            usage();
            return 0;
        }
    ippl::initialize(argc, argv);
    try {
        if (argc < 9) {
            if (ippl::Comm->rank() == 0)
                usage();
            throw IpplException(TestName, "Missing Landau damping arguments");
        }
        Vector_t<int, 3> grid;
        int arg = 1;
        for (unsigned d = 0; d < 3; ++d)
            grid[d] = integer<int>(argv[arg++], "grid extent");
        const auto particles = integer<size_type>(argv[arg++], "totalParticles");
        const auto steps = integer<int>(argv[arg++], "steps");
        std::string solver = argv[arg++];
        const double threshold = number(argv[arg++], "lbThreshold");
        if (threshold < 0 || threshold > 1)
            throw IpplException(TestName, "lbThreshold must be between 0 and 1");
        std::string method = argv[arg++];
        if (method != "LeapFrog")
            throw IpplException(TestName, "Only LeapFrog is supported");
        std::vector<std::string> preconditioner;
        if (solver == "PCG" || solver == "FEM_PRECON")
            while (arg < argc && !std::string_view(argv[arg]).starts_with("--")
                   && std::string_view(argv[arg]) != "-b" && std::string_view(argv[arg]) != "-i")
                preconditioner.emplace_back(argv[arg++]);
        if (solver == "PCG" && preconditioner.empty())
            throw IpplException(TestName, "PCG requires a preconditioner, for example jacobi");

        alpine_stream::OutputOptions options;
        if (const char* outputDir = std::getenv("IPPL_OPENPMD_OUTPUT_DIR");
            outputDir && *outputDir)
            options.writer.directory = outputDir;
        while (arg < argc) {
            const std::string token = argv[arg++];
            auto next = [&]() -> std::string {
                if (arg == argc)
                    throw IpplException(TestName, "Missing value for " + token);
                return argv[arg++];
            };
            if (token == "--output-dir")
                options.writer.directory = next();
            else if (token == "--output-kind") {
                const auto kind = next();
                if (kind == "particles")
                    options.kind = alpine_stream::OutputKind::Particles;
                else if (kind == "fields")
                    options.kind = alpine_stream::OutputKind::Fields;
                else
                    throw IpplException(TestName, "output-kind must be particles or fields");
            }
            else if (token == "--io-backend") {
                const auto backend = next();
                if (backend == "hdf5")
                    options.writer.backend = ippl::OpenPMDBackend::HDF5;
                else if (backend == "bp5")
                    options.writer.backend = ippl::OpenPMDBackend::BP5;
                else
                    throw IpplException(TestName, "io-backend must be hdf5 or bp5");
            } else if (token == "--no-output")
                options.enabled = false;
            else if (token == "--staging-mib") {
                const auto mib = integer<std::size_t>(next(), "staging-mib");
                constexpr std::size_t MIB = 1024 * 1024;
                if (mib > std::numeric_limits<std::size_t>::max() / MIB)
                    throw IpplException(TestName, "staging-mib overflows size_t");
                options.writer.stagingBytes = mib * MIB;
            } else if (token == "--length-unit-si")
                options.lengthUnitSI = number(next(), "length-unit-si");
            else if (token == "--time-unit-si")
                options.writer.timeUnitSI = number(next(), "time-unit-si");
            else if (token == "--overallocate" || token == "-b" || token == "--info"
                     || token == "-i" || token == "--timer-fences")
                (void)next();  // already handled by ippl::initialize
            else
                throw IpplException(TestName, "Unknown argument: " + token);
        }

        const auto total = IpplTimings::getTimer("total");
        const auto initialize = IpplTimings::getTimer("initialize");
        const auto timestepping = IpplTimings::getTimer("timestepping");
        IpplTimings::startTimer(total);
        IpplTimings::startTimer(initialize);
        alpine_stream::LandauDampingStreamManager manager(
            particles, steps, grid, threshold, solver, method, preconditioner, options);
        manager.pre_run();
        Kokkos::fence("Landau initialization end");
        IpplTimings::stopTimer(initialize);
        ippl::Comm->barrier();
        IpplTimings::startTimer(timestepping);
        manager.run(steps);
        IpplTimings::stopTimer(timestepping);
        manager.closeOutput();
        IpplTimings::stopTimer(total);
        manager.writeSummary();
        IpplTimings::print();
        IpplTimings::print("timing.dat");
        IpplTimings::dumpToCSV("timings.csv");
    } catch (const IpplException& error) {
        if (ippl::Comm->rank() == 0)
            std::cerr << TestName << ": " << error.where() << ": " << error.what() << '\n';
        ippl::Comm->abort(1);
    } catch (const std::exception& error) {
        if (ippl::Comm->rank() == 0)
            std::cerr << TestName << ": " << error.what() << '\n';
        ippl::Comm->abort(1);
    }
    ippl::finalize();
    return 0;
}
