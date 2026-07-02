// Console progress logging for FEMPoissonSolver convergence drivers.
#ifndef IPPL_POISSON_CONVERGENCE_PROGRESS_WFC_HPP
#define IPPL_POISSON_CONVERGENCE_PROGRESS_WFC_HPP

#include "PoissonConvergenceSources.hpp"

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

inline unsigned totalConvergenceCases(unsigned min_nodes, unsigned max_nodes) {
    constexpr unsigned num_sources = 4;
    constexpr unsigned num_orders  = 3;
    return num_sources * num_orders * countRefinementLevels(min_nodes, max_nodes);
}

inline void logStudyBanner(unsigned dim, unsigned quad_nodes, unsigned min_nodes,
                           unsigned max_nodes, const std::string& out_path,
                           const char* domain_note) {
    if (Comm->rank() != 0) {
        return;
    }
    const unsigned total = totalConvergenceCases(min_nodes, max_nodes);
    std::cout << "FEMPoissonSolver_wFEMContainer — " << dim << "D convergence study\n"
              << "  domain: " << domain_note << "\n"
              << "  GL quadrature order " << quad_nodes << ", nodes/axis " << min_nodes << " … "
              << max_nodes << " (" << countRefinementLevels(min_nodes, max_nodes)
              << " levels)\n"
              << "  cases: " << total << " (= 4 sources × 3 orders × "
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

inline void writeDatHeader(std::ostream& out, unsigned dim, const char* domain_note) {
    out << "# FEMPoissonSolver_wFEMContainer — " << dim << "D convergence (TASK3 sources)\n";
    out << "# domain " << domain_note << ", homogeneous Dirichlet, GL quadrature order 9\n";
    out << "# columns: source order quad_nodes num_nodes h rel_L2 cg_residue cg_iterations\n";
    if (dim > 1) {
        out << "# num_nodes = nodes per axis (uniform tensor grid)\n";
    }
}

inline void appendDatRow(std::ostream& out, const ConvergenceRow& row) {
    out << row.source << ' ' << row.order << ' ' << row.quad_nodes << ' ' << row.num_nodes << ' '
        << std::setprecision(17) << row.h << ' ' << row.rel_l2 << ' ' << row.cg_residue << ' '
        << row.cg_iterations << '\n';
    out.flush();
}

}  // namespace poisson_convergence_wfc
}  // namespace ippl

#endif
