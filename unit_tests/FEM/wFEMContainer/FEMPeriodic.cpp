#include "Ippl.h"

#include <string_view>

#include "Oracle/FEMOracleTestData.h"
#include "PoissonSolvers/FEMPoissonSolver_wFEMContainer.h"
#include "gtest/gtest.h"

namespace {
    template <unsigned Dim, unsigned P>
    void checkPeriodic(const char* reuse = "full") {
        using Handler            = ippl::LagrangeDOFHandler<double, Dim, P>;
        using Field              = typename Handler::FEMContainer_t;
        using Solver             = ippl::FEMPoissonSolver_wFEMContainer<Field, Field, P, P + 1>;
        constexpr unsigned Cells = 4;
        ippl::NDIndex<Dim> domain(ippl::Vector<unsigned, Dim>(Cells + 1));
        std::array<bool, Dim> parallel;
        parallel.fill(true);
        ippl::FieldLayout<Dim> layout(MPI_COMM_WORLD, domain, parallel, true);
        ippl::Vector<double, Dim> spacing;
        double volume = 1, det = 1, count = 1;
        for (unsigned d = 0; d < Dim; ++d) {
            spacing[d] = (1.0 + 0.25 * d) / Cells;
            volume *= spacing[d] * Cells;
            det *= spacing[d];
            count *= Cells * P;
        }
        ippl::UniformCartesian<double, Dim> mesh(domain, spacing, ippl::Vector<double, Dim>(0));
        std::array<ippl::FieldBC, 2 * Dim> bc;
        bc.fill(ippl::PERIODIC_FACE);
        for (const auto* family : {"gll", "equispaced"}) {
            SCOPED_TRACE(family);
            Field x(mesh, layout), y(mesh, layout), one(mesh, layout);
            x.setFieldBC(bc);
            y.setFieldBC(bc);
            one.setFieldBC(bc);
            one = 1;
            EXPECT_DOUBLE_EQ(innerProduct(one, one), count);
            Solver solver(x, y);
            ippl::ParameterList params;
            params.add("interpolation_nodes", std::string(family));
            params.add("preconditioned", false);
            params.add("fem_reuse", std::string(reuse));
            params.add("tolerance", 1e-12);
            params.add("max_iterations", 2000);
            solver.mergeParameters(params);
            solver.configureNodeFamilies();
            auto& space = solver.getSpace();
            ippl::EvalFunctor<double, Dim, Handler::dofsPerElement> eval(1.0 / spacing, det);
            one.fillHalo();
            auto zero = space.evaluateAx(one, eval);
            EXPECT_DOUBLE_EQ(ippl::norm(zero), 0);
            auto weights = one.deepCopy();
            space.evaluateLoadVector(weights);
            EXPECT_NEAR(innerProduct(one, weights), volume, 2e-12 * volume);

            Kokkos::View<double*> coefficients("periodic probe", space.numGlobalDOFs());
            auto host = Kokkos::create_mirror_view(coefficients);
            for (size_t i = 0; i < host.extent(0); ++i)
                host(i) = std::sin(0.7 * i);
            Kokkos::deep_copy(coefficients, host);
            space.fillFromGlobalCoefficients(x, coefficients);
            for (size_t i = 0; i < host.extent(0); ++i)
                host(i) = std::cos(0.31 * i);
            Kokkos::deep_copy(coefficients, host);
            space.fillFromGlobalCoefficients(y, coefficients);
            x.fillHalo();
            y.fillHalo();
            auto ax = space.evaluateAx(x, eval);
            auto ay = space.evaluateAx(y, eval);
            // Tensorize the published Basix 1D matrices, then identify periodic endpoints.
            // This reference never reads IPPL's element matrix or local DOF mapping.
            const fem_oracle::Reference* reference = nullptr;
            for (const auto& c : fem_oracle::referenceCases())
                if (c.order == P && std::string_view(c.interpolationFamily) == family) {
                    reference = &c.reference;
                    break;
                }
            ASSERT_NE(reference, nullptr);
            constexpr unsigned AxisNodes = P + 1;
            constexpr unsigned N         = Handler::dofsPerElement;
            auto canonical               = [](unsigned i) {
                return i == 0 ? 0u : i == P ? 1u : i + 1;
            };
            std::vector<double> element(N * N), expected(count), lower(count), upper(count),
                diagonal(count);
            for (unsigned i = 0; i < N; ++i)
                for (unsigned j = 0; j < N; ++j) {
                    for (unsigned deriv = 0; deriv < Dim; ++deriv) {
                        unsigned ii = i, jj = j;
                        double value = det / (spacing[deriv] * spacing[deriv]);
                        for (unsigned d = 0; d < Dim; ++d) {
                            const auto entry =
                                canonical(ii % AxisNodes) * AxisNodes + canonical(jj % AxisNodes);
                            value *=
                                d == deriv ? reference->stiffness[entry] : reference->mass[entry];
                            ii /= AxisNodes;
                            jj /= AxisNodes;
                        }
                        element[i * N + j] += value;
                    }
                }
            unsigned cells = 1;
            for (unsigned d = 0; d < Dim; ++d)
                cells *= Cells;
            for (unsigned cell = 0; cell < cells; ++cell) {
                std::array<unsigned, N> global;
                for (unsigned i = 0; i < N; ++i) {
                    unsigned ci = cell, li = i, stride = 1;
                    global[i] = 0;
                    for (unsigned d = 0; d < Dim; ++d) {
                        global[i] += ((ci % Cells * P + li % AxisNodes) % (Cells * P)) * stride;
                        stride *= Cells * P;
                        ci /= Cells;
                        li /= AxisNodes;
                    }
                }
                for (unsigned i = 0; i < N; ++i)
                    for (unsigned j = 0; j < N; ++j) {
                        const double value = element[i * N + j] * std::sin(0.7 * global[j]);
                        expected[global[i]] += value;
                        if (global[i] > global[j])
                            lower[global[i]] += value;
                        if (global[i] < global[j])
                            upper[global[i]] += value;
                        if (global[i] == global[j])
                            diagonal[global[i]] += value;
                    }
            }
            auto compare = [&](const std::vector<double>& values, const Field& actual) {
                for (size_t i = 0; i < host.extent(0); ++i)
                    host(i) = values[i];
                Kokkos::deep_copy(coefficients, host);
                auto expectedField = one.deepCopy();
                space.fillFromGlobalCoefficients(expectedField, coefficients);
                EXPECT_LT(ippl::norm(actual - expectedField),
                          2e-10 * std::max(1.0, ippl::norm(expectedField)));
            };
            compare(expected, ax);
            compare(lower, space.evaluateAx_lower(x, eval));
            compare(upper, space.evaluateAx_upper(x, eval));
            compare(diagonal, space.evaluateAx_diag(x, eval));
            EXPECT_NEAR(innerProduct(x, ay), innerProduct(ax, y),
                        2e-11 * ippl::norm(x) * ippl::norm(ay));
            EXPECT_NEAR(innerProduct(one, ax), 0, 2e-12 * ippl::norm(ax));
            const auto exact = x.deepCopy();
            const auto load  = ax.deepCopy();
            for (bool preconditioned : {false, true}) {
                params.update("preconditioned", preconditioned);
                solver.mergeParameters(params);
                x = 0;
                solver.solveLoadVector(load);
                auto firstSolution = x.deepCopy();
                solver.solveLoadVector(load);
                if (std::string_view(reuse) == "full") {
                    EXPECT_EQ(solver.getSolveStatistics().operatorSetups, 0u);
                    EXPECT_EQ(solver.getSolveStatistics().preconditionerSetups, 0u);
                }
                EXPECT_LT(ippl::norm(x - firstSolution),
                          2e-10 * std::max(1.0, ippl::norm(firstSolution)));
                auto difference = x - exact;
                difference -= innerProduct(one, difference) / count;
                EXPECT_LT(ippl::norm(difference), 2e-9 * ippl::norm(exact));
                auto fresh = load - space.evaluateAx(x, eval);
                EXPECT_LT(ippl::norm(fresh), 3e-11 * ippl::norm(load));
                EXPECT_NEAR(solver.getAvg(true), 0, 2e-12);
                EXPECT_DOUBLE_EQ(ippl::norm(load - ax), 0);
                EXPECT_LT(solver.getIterationCount(), 2000);
            }
            EXPECT_THROW(solver.solveLoadVector(one), IpplException);
            x = 0;
            y = 0;
            EXPECT_NO_THROW(solver.solveLoadVector(y));
            EXPECT_EQ(solver.getIterationCount(), 0);
        }
    }
}  // namespace

#ifndef FEM_PERIODIC_CASE
int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
#endif
