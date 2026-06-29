// GenerateFemTestData — export Poisson FEM reference data (SCHEMA.md) via LagrangeSpace_wfc.
//
// Usage:
//   GenerateFemTestData --dim 1 --order 1 --quad 9 --nel 4 \
//       --origin 0 --corner 1 --case sine --solve explicit --outdir /path/to/out
//
// Run serially: mpirun -n 1 ./GenerateFemTestData ...

#include "Ippl.h"

#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "FEM/Validation/FEMTestDataGenerator.h"

namespace {

template <unsigned Dim>
double sine_source(const ippl::Vector<double, Dim>& x) {
    double out = 1.0;
    for (unsigned d = 0; d < Dim; ++d) {
        out *= std::sin(M_PI * x[d]);
    }
    return out;
}

template <unsigned Dim>
double nonsymmetric_source(const ippl::Vector<double, Dim>& x) {
    if constexpr (Dim == 1) {
        return (1.0 + x[0]) * std::exp(-x[0]);
    } else if constexpr (Dim == 2) {
        return (1.0 + x[0]) * x[1] * x[1];
    } else {
        return (1.0 + x[0]) * x[1] * x[2] * x[2];
    }
}

struct Options {
    unsigned dim    = 1;
    unsigned order  = 1;
    unsigned quad   = 9;
    std::array<unsigned, 3> nel{4, 4, 4};
    std::array<double, 3> origin{0.0, 0.0, 0.0};
    std::array<double, 3> corner{1.0, 1.0, 1.0};
    std::string case_tag = "sine";
    std::string solve_tag = "explicit";
    std::string outdir;
};

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            out.push_back(item);
        }
    }
    return out;
}

Options parse_args(int argc, char* argv[]) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const char* flag) -> std::string {
            if (arg != flag || i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + flag);
            }
            return argv[++i];
        };
        if (arg == "--dim") {
            opt.dim = static_cast<unsigned>(std::stoul(need_value("--dim")));
        } else if (arg == "--order") {
            opt.order = static_cast<unsigned>(std::stoul(need_value("--order")));
        } else if (arg == "--quad") {
            opt.quad = static_cast<unsigned>(std::stoul(need_value("--quad")));
        } else if (arg == "--nel") {
            const auto parts = split_csv(need_value("--nel"));
            for (std::size_t k = 0; k < parts.size() && k < 3; ++k) {
                opt.nel[k] = static_cast<unsigned>(std::stoul(parts[k]));
            }
        } else if (arg == "--origin") {
            const auto parts = split_csv(need_value("--origin"));
            for (std::size_t k = 0; k < parts.size() && k < 3; ++k) {
                opt.origin[k] = std::stod(parts[k]);
            }
        } else if (arg == "--corner") {
            const auto parts = split_csv(need_value("--corner"));
            for (std::size_t k = 0; k < parts.size() && k < 3; ++k) {
                opt.corner[k] = std::stod(parts[k]);
            }
        } else if (arg == "--case") {
            opt.case_tag = need_value("--case");
        } else if (arg == "--solve") {
            opt.solve_tag = need_value("--solve");
        } else if (arg == "--outdir") {
            opt.outdir = need_value("--outdir");
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "GenerateFemTestData --dim D --order P --quad Q --nel n[,n,n] "
                   "--origin o[,o,o] --corner c[,c,c] --case sine|nonsymmetric "
                   "--solve explicit|cg --outdir PATH\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    if (opt.outdir.empty()) {
        throw std::runtime_error("--outdir is required");
    }
    if (opt.quad != 9) {
        throw std::runtime_error("only --quad 9 is supported in this build");
    }
    if (opt.dim < 1 || opt.dim > 3) {
        throw std::runtime_error("--dim must be 1, 2, or 3");
    }
    if (opt.order < 1 || opt.order > 3) {
        throw std::runtime_error("--order must be 1, 2, or 3");
    }
    if (opt.solve_tag != "explicit" && opt.solve_tag != "cg") {
        throw std::runtime_error("--solve must be explicit or cg");
    }
    return opt;
}

template <unsigned Dim, unsigned Order>
int run_case(const Options& opt) {
    std::array<unsigned, Dim> nel{};
    std::array<double, Dim> origin{};
    std::array<double, Dim> corner{};
    for (unsigned d = 0; d < Dim; ++d) {
        nel[d]    = opt.nel[d];
        origin[d] = opt.origin[d];
        corner[d] = opt.corner[d];
    }

    std::function<double(const ippl::Vector<double, Dim>&)> source;
    if (opt.case_tag == "sine") {
        source = [](const ippl::Vector<double, Dim>& x) { return sine_source<Dim>(x); };
    } else if (opt.case_tag == "nonsymmetric") {
        source = [](const ippl::Vector<double, Dim>& x) { return nonsymmetric_source<Dim>(x); };
    } else {
        throw std::runtime_error("unknown --case: " + opt.case_tag);
    }

    const ippl::femdata::SolveMode solve_mode =
        (opt.solve_tag == "cg") ? ippl::femdata::SolveMode::CG : ippl::femdata::SolveMode::Explicit;

    ippl::femdata::FemTestDataGenerator<Dim, Order, 9>::generate(nel, origin, corner, opt.outdir,
                                                                   source, 0.0, solve_mode);
    if (ippl::Comm->rank() == 0) {
        std::cout << "Wrote IPPL FEM test data to: " << opt.outdir << std::endl;
    }
    return 0;
}

int dispatch(const Options& opt) {
    if (opt.dim == 1) {
        if (opt.order == 1) {
            return run_case<1, 1>(opt);
        }
        if (opt.order == 2) {
            return run_case<1, 2>(opt);
        }
        return run_case<1, 3>(opt);
    }
    if (opt.dim == 2) {
        if (opt.order == 1) {
            return run_case<2, 1>(opt);
        }
        if (opt.order == 2) {
            return run_case<2, 2>(opt);
        }
        return run_case<2, 3>(opt);
    }
    if (opt.order == 1) {
        return run_case<3, 1>(opt);
    }
    if (opt.order == 2) {
        return run_case<3, 2>(opt);
    }
    return run_case<3, 3>(opt);
}

}  // namespace

int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);
    int code = 0;
    try {
        if (ippl::Comm->size() != 1) {
            throw std::runtime_error("GenerateFemTestData must run with a single MPI rank");
        }
        const Options opt = parse_args(argc, argv);
        code              = dispatch(opt);
    } catch (const std::exception& ex) {
        if (ippl::Comm->rank() == 0) {
            std::cerr << "GenerateFemTestData error: " << ex.what() << std::endl;
        }
        code = 1;
    }
    ippl::finalize();
    return code;
}
