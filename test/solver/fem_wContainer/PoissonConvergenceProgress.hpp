// Console progress logging for FEMPoissonSolver convergence drivers.
#ifndef IPPL_POISSON_CONVERGENCE_PROGRESS_WFC_HPP
#define IPPL_POISSON_CONVERGENCE_PROGRESS_WFC_HPP

#include "PoissonConvergenceSources.hpp"

#include "FEM/InterpolationNodeFamilies.h"
#include "FEM/Quadrature/SelectableQuadrature.h"
#include "Ippl.h"

#include <chrono>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace ippl {
namespace poisson_convergence_wfc {

inline unsigned countRefinementLevels(unsigned min_nodes, unsigned max_nodes) {
    unsigned levels = 0;
    for (unsigned n = min_nodes; n <= max_nodes; n <<= 1) {
        ++levels;
    }
    return levels;
}

inline unsigned totalConvergenceCases(unsigned min_nodes, unsigned max_nodes,
                                      unsigned num_orders = 3) {
    constexpr unsigned num_sources = 4;
    return num_sources * num_orders * countRefinementLevels(min_nodes, max_nodes);
}

inline unsigned maxLagrangeOrderForPreconditioner(const std::string& /*precon_type*/) {
    return 3u;
}

inline std::string parseStringFlag(int argc, char* argv[], const std::string& flag,
                                   const std::string& default_value) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == flag) {
            return argv[i + 1];
        }
    }
    return default_value;
}

/** @brief Parse an int CLI flag; default if absent. */
inline int parseIntFlag(int argc, char* argv[], const std::string& flag, int default_value) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == flag) {
            return std::stoi(argv[i + 1]);
        }
    }
    return default_value;
}

inline std::string canonicalInterpolationTag(const std::string& raw) {
    return toString(parseLagrangeNodeFamily(raw));
}

inline std::string canonicalQuadratureTag(const std::string& raw) {
    return toString(parseQuadratureNodeFamily(raw));
}

inline std::string convergenceDatFilename(unsigned dim, const std::string& interpolation_tag) {
    return "convergence_FEMPoissonSolver" + std::to_string(dim) + "D_" + interpolation_tag
           + ".dat";
}

inline void logStudyBanner(unsigned dim, unsigned quad_nodes, unsigned min_nodes,
                           unsigned max_nodes, const std::string& out_path,
                           const char* domain_note, unsigned num_orders = 3,
                           const std::string& interpolation_tag = "gll",
                           const std::string& quadrature_tag    = "gauss_legendre") {
    if (Comm->rank() != 0) {
        return;
    }
    const unsigned total = totalConvergenceCases(min_nodes, max_nodes, num_orders);
    std::cout << "FEMPoissonSolver_wFEMContainer — " << dim << "D convergence study\n"
              << "  domain: " << domain_note << "\n"
              << "  interpolation_nodes=" << interpolation_tag
              << "  quadrature_nodes=" << quadrature_tag << " (count " << quad_nodes << ")\n"
              << "  nodes/axis " << min_nodes << " … " << max_nodes << " ("
              << countRefinementLevels(min_nodes, max_nodes) << " levels)\n"
              << "  cases: " << total << " (= 4 sources × " << num_orders << " orders × "
              << countRefinementLevels(min_nodes, max_nodes) << " meshes)\n"
              << "  output: " << out_path << "\n"
              << std::flush;
}

class ConvergenceProgressLog {
public:
    ConvergenceProgressLog(unsigned total_cases, unsigned spatial_dim)
        : total_cases_(total_cases), spatial_dim_(spatial_dim) {}

    void beginCase(const char* source, unsigned order, unsigned num_nodes) {
        if (Comm->rank() != 0) {
            return;
        }
        ++case_index_;
        t0_ = std::chrono::steady_clock::now();
        std::cout << '[' << case_index_ << '/' << total_cases_ << "] " << source << "  P" << order
                  << "  N=" << num_nodes;
        if (spatial_dim_ > 1) {
            std::cout << '^' << spatial_dim_;
        }
        std::cout << "  running…" << std::flush;
    }

    void endCase(const ConvergenceRow& row) {
        if (Comm->rank() != 0) {
            return;
        }
        const auto elapsed = std::chrono::steady_clock::now() - t0_;
        const double sec   = std::chrono::duration<double>(elapsed).count();
        std::cout << "  L2=" << std::scientific << std::setprecision(4) << row.rel_l2
                  << "  cg_iters=" << row.cg_iterations << "  (" << std::fixed
                  << std::setprecision(2) << sec << " s)\n"
                  << std::flush;
    }

private:
    unsigned total_cases_ = 0;
    unsigned spatial_dim_ = 0;
    unsigned case_index_  = 0;
    std::chrono::steady_clock::time_point t0_{};
};

inline void writeDatHeader(std::ostream& out, unsigned dim, const char* domain_note,
                           const std::string& interpolation_tag = "gll",
                           const std::string& quadrature_tag    = "gauss_legendre") {
    out << "# FEMPoissonSolver_wFEMContainer — " << dim << "D convergence (TASK3 sources)\n";
    out << "# domain " << domain_note << "\n";
    out << "# interpolation_nodes=" << interpolation_tag
        << "  quadrature_nodes=" << quadrature_tag << "  quad_nodes=9 (count)\n";
    out << "# columns: source order quad_nodes interpolation_nodes quadrature_family "
           "num_nodes h rel_L2 cg_residue cg_iterations\n";
    if (dim > 1) {
        out << "# num_nodes = nodes per axis (uniform tensor grid)\n";
    }
}

inline void appendDatRow(std::ostream& out, const ConvergenceRow& row) {
    out << row.source << ' ' << row.order << ' ' << row.quad_nodes << ' '
        << row.interpolation_nodes << ' ' << row.quadrature_family << ' ' << row.num_nodes << ' '
        << std::setprecision(17) << row.h << ' ' << row.rel_l2 << ' ' << row.cg_residue << ' '
        << row.cg_iterations << '\n';
    out.flush();
}

}  // namespace poisson_convergence_wfc
}  // namespace ippl

#endif
