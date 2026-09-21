#pragma once
#include "ExperimentConfig.h"
#include "PoissonSolvers/FEMPoissonSolver_wFEMContainer.h"
namespace electrostatic_fem {
    template <unsigned Dim, unsigned Order, unsigned QuadPoints>
    struct FEMFields {
        using Handler = ippl::LagrangeDOFHandler<double, Dim, Order>;
        using Field   = typename Handler::FEMContainer_t;
        using Solver  = ippl::FEMPoissonSolver_wFEMContainer<Field, Field, Order, QuadPoints>;
        using Point   = ippl::Vector<double, Dim>;
        ippl::NDIndex<Dim> domain;
        ippl::FieldLayout<Dim> layout;
        ippl::UniformCartesian<double, Dim> mesh;
        Field potential, load, weights, one;
        Solver solver;
        double volume = 1;
        static ippl::NDIndex<Dim> makeDomain(const ExperimentConfig& c) {
            ippl::NDIndex<Dim> result;
            for (unsigned d = 0; d < Dim; ++d)
                result[d] = ippl::Index(c.cells[d] + 1);
            return result;
        }
        static std::array<bool, Dim> parallel() {
            std::array<bool, Dim> a;
            a.fill(true);
            return a;
        }
        static Point spacing(const ExperimentConfig& c) {
            Point h;
            for (unsigned d = 0; d < Dim; ++d)
                h[d] = c.lengths[d] / c.cells[d];
            return h;
        }
        explicit FEMFields(const ExperimentConfig& c)
            : domain(makeDomain(c))
            , layout(MPI_COMM_WORLD, domain, parallel(), true)
            , mesh(domain, spacing(c), Point(0))
            , potential(mesh, layout)
            , load(mesh, layout)
            , weights(mesh, layout)
            , one(mesh, layout)
            , solver(potential, load) {
            std::array<ippl::FieldBC, 2 * Dim> bc;
            bc.fill(ippl::PERIODIC_FACE);
            potential.setFieldBC(bc);
            load.setFieldBC(bc);
            weights.setFieldBC(bc);
            one.setFieldBC(bc);
            potential = 0;
            load      = 0;
            weights   = 1;
            one       = 1;
            for (unsigned d = 0; d < Dim; ++d)
                volume *= c.lengths[d];
            solver.mergeParameters(c.parameters(c.solver == "plain" ? "plain" : c.preconditioner));
            solver.configureNodeFamilies();
            weights.fillHalo();
            solver.getSpace().evaluateLoadVector(weights);
        }
    };
}  // namespace electrostatic_fem
