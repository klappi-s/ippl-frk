// Convergence study for FEMPoissonSolver_wFEMContainer (Track B), 1D.
//
// Four sources on [0,1]: lowOrderPolynomial, highOrderPolynomial,
// shiftedExponential, sines.
// Lagrange orders P1-P3, Gauss-Legendre quadrature order 9 (all orders).
//
// Writes: convergence_FEMPoissonSolver1D.dat
//
// Usage:
//   ./TestFEMPoissonSolver1D_zeroDirichlet_convergence [--info 5]
//   ./TestFEMPoissonSolver1D_zeroDirichlet_convergence --max-nodes 1024
//   ./TestFEMPoissonSolver1D_zeroDirichlet_convergence --solver plain        # unpreconditioned CG
//   ./TestFEMPoissonSolver1D_zeroDirichlet_convergence --solver preconditioned  # Jacobi PCG (default)

#include "Ippl.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "PoissonConvergenceProgress.hpp"
#include "PoissonConvergenceSources.hpp"
#include "PoissonSolvers/FEMPoissonSolver_wFEMContainer.h"

namespace {

using namespace ippl::poisson_convergence_wfc;

static constexpr unsigned Dim         = 1;
static constexpr unsigned QuadNodes   = 9;
static constexpr double domain_start  = 0.0;
static constexpr double domain_end    = 1.0;

template <unsigned Order, SourceCase Src>
ConvergenceRow runCase(unsigned num_nodes, bool preconditioned) {
    using T = double;

    using DOFHandler_t =
        ippl::DOFHandler<T, ippl::FiniteElementSpaceTraits<ippl::LagrangeSpaceTag, Dim, Order>>;
    using Field_t = typename DOFHandler_t::FEMContainer_t;
    using BC_t    = std::array<ippl::FieldBC, 2 * Dim>;

    const unsigned num_cells = num_nodes - 1;
    const unsigned nghost    = 1;

    const ippl::Vector<unsigned, Dim> nodes_per_dim(num_nodes);
    ippl::NDIndex<Dim> domain(nodes_per_dim);
    const ippl::Vector<T, Dim> cell_spacing(
        (domain_end - domain_start) / static_cast<T>(num_cells));
    const ippl::Vector<T, Dim> origin(domain_start);
    ippl::UniformCartesian<T, Dim> mesh(domain, cell_spacing, origin);

    std::array<bool, Dim> is_parallel{};
    is_parallel.fill(true);
    ippl::FieldLayout<Dim> layout(MPI_COMM_WORLD, domain, is_parallel);

    Field_t lhs(mesh, layout, nghost);
    Field_t rhs(mesh, layout, nghost);

    BC_t bc{};
    bc.fill(ippl::ZERO_FACE);
    lhs.setFieldBC(bc);
    rhs.setFieldBC(bc);

    assignSourceToField<T, Dim, Order, Src>(rhs, mesh, layout);

    ippl::FEMPoissonSolver_wFEMContainer<Field_t, Field_t, Order, QuadNodes> solver(lhs, rhs);

    ippl::ParameterList params;
    params.add("tolerance", 1e-13);
    params.add("max_iterations", 150000);
    params.add("preconditioned", preconditioned);
    solver.mergeParameters(params);
    solver.solve();

    AnalyticSolutionFunctor<Src, Dim, T> analytic;

    ConvergenceRow row;
    row.source       = sourceTag(Src);
    row.order        = Order;
    row.quad_nodes   = QuadNodes;
    row.num_nodes    = num_nodes;
    row.h            = cell_spacing[0];
    row.rel_l2       = solver.getL2Error(analytic);
    row.cg_residue   = solver.getResidue();
    row.cg_iterations = solver.getIterationCount();
    return row;
}

template <unsigned Order>
ConvergenceRow runCaseSource(unsigned num_nodes, SourceCase src, bool preconditioned) {
    switch (src) {
        case SourceCase::LowOrderPolynomial:
            return runCase<Order, SourceCase::LowOrderPolynomial>(num_nodes, preconditioned);
        case SourceCase::HighOrderPolynomial:
            return runCase<Order, SourceCase::HighOrderPolynomial>(num_nodes, preconditioned);
        case SourceCase::ShiftedExponential:
            return runCase<Order, SourceCase::ShiftedExponential>(num_nodes, preconditioned);
        case SourceCase::Sines:
            return runCase<Order, SourceCase::Sines>(num_nodes, preconditioned);
        default:
            throw std::runtime_error("unknown source");
    }
}

unsigned parseMaxNodes(int argc, char* argv[], unsigned default_max) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--max-nodes") {
            return static_cast<unsigned>(std::stoul(argv[i + 1]));
        }
    }
    return default_max;
}

// Parse --solver plain|preconditioned (default: preconditioned = true = Jacobi PCG)
bool parsePreconditioned(int argc, char* argv[]) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--solver") {
            std::string mode = argv[i + 1];
            if (mode == "plain") return false;
            if (mode == "preconditioned") return true;
            throw std::runtime_error("unknown --solver mode '" + mode
                                     + "'. Use 'plain' or 'preconditioned'.");
        }
    }
    return true;  // default: Jacobi-preconditioned CG
}

double observedRate(const ConvergenceRow& coarse, const ConvergenceRow& fine) {
    if (coarse.rel_l2 <= 0.0 || fine.rel_l2 <= 0.0 || coarse.h <= fine.h) {
        return -1.0;
    }
    return std::log(coarse.rel_l2 / fine.rel_l2) / std::log(coarse.h / fine.h);
}

void printTableHeader(std::ostream& os) {
    os << std::setw(14) << "source" << std::setw(8) << "order" << std::setw(8) << "quad"
       << std::setw(12) << "num_nodes" << std::setw(22) << "h" << std::setw(22) << "rel_L2"
       << std::setw(22) << "cg_residue" << std::setw(12) << "cg_iters" << std::setw(14) << "rate"
       << '\n';
}

void printRow(std::ostream& os, const ConvergenceRow& row, double rate) {
    os << std::setw(14) << row.source << std::setw(8) << row.order << std::setw(8) << row.quad_nodes
       << std::setw(12) << row.num_nodes << std::setw(22) << std::setprecision(16) << row.h
       << std::setw(22) << row.rel_l2 << std::setw(22) << row.cg_residue << std::setw(12)
       << row.cg_iterations;
    if (rate >= 0.0) {
        os << std::setw(14) << std::setprecision(4) << rate;
    } else {
        os << std::setw(14) << "-";
    }
    os << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);
    int rc = 0;
    try {
        constexpr unsigned min_nodes = 1u << 2;
        const unsigned max_nodes   = parseMaxNodes(argc, argv, 1u << 10);
        const bool preconditioned  = parsePreconditioned(argc, argv);

        const auto out_path =
            std::filesystem::current_path() / "convergence_FEMPoissonSolver1D.dat";

        constexpr const char* domain_note = "[0,1] (homogeneous Dirichlet)";
        logStudyBanner(Dim, QuadNodes, min_nodes, max_nodes, out_path.string(), domain_note);
        if (ippl::Comm->rank() == 0) {
            std::cout << "Solver mode: " << (preconditioned ? "preconditioned (Jacobi)" : "plain (unpreconditioned)") << '\n';
        }

        std::unique_ptr<std::ofstream> dat_out;
        if (ippl::Comm->rank() == 0) {
            dat_out = std::make_unique<std::ofstream>(out_path);
            if (!dat_out->is_open()) {
                throw std::runtime_error("cannot open output file: " + out_path.string());
            }
            writeDatHeader(*dat_out, Dim, domain_note);
        }

        ConvergenceProgressLog progress(totalConvergenceCases(min_nodes, max_nodes), Dim);
        std::vector<std::vector<ConvergenceRow>> studies;

        for (SourceCase src :
             {SourceCase::LowOrderPolynomial, SourceCase::HighOrderPolynomial, SourceCase::ShiftedExponential, SourceCase::Sines}) {
            for (unsigned order : {1u, 2u, 3u}) {
                std::vector<ConvergenceRow> rows;
                for (unsigned n = min_nodes; n <= max_nodes; n <<= 1) {
                    progress.beginCase(sourceTag(src), order, n);
                    ConvergenceRow row = [&]() {
                        switch (order) {
                            case 1:
                                return runCaseSource<1>(n, src, preconditioned);
                            case 2:
                                return runCaseSource<2>(n, src, preconditioned);
                            default:
                                return runCaseSource<3>(n, src, preconditioned);
                        }
                    }();
                    progress.endCase(row);
                    rows.push_back(row);
                    if (dat_out) {
                        appendDatRow(*dat_out, row);
                    }
                }
                studies.push_back(std::move(rows));
            }
        }

        if (ippl::Comm->rank() == 0) {
            Inform msg("");
            msg << "\nSummary\n";
            printTableHeader(msg.getStream());

            for (const auto& rows : studies) {
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    const double rate = (i == 0) ? -1.0 : observedRate(rows[i - 1], rows[i]);
                    printRow(msg.getStream(), rows[i], rate);
                }
                msg << '\n';
            }

            std::cout << "Finished. Wrote plot data: " << out_path.string() << '\n';
        }
    } catch (const std::exception& ex) {
        if (ippl::Comm->rank() == 0) {
            Inform err("");
            err << "ERROR: " << ex.what() << endl;
        }
        rc = 1;
    }
    ippl::finalize();
    return rc;
}
