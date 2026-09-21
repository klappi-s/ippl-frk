#include "Ippl.h"

#include "PoissonSolvers/FEMPoissonSolver_wFEMContainer.h"
#include "gtest/gtest.h"

// Probe existing preconditioner objects, without adding a second CG implementation.
TEST(FEMPreconditionerProperties, PeriodicP3ProjectedSymmetryAndSpectrum) {
    using Handler = ippl::LagrangeDOFHandler<double, 1, 3>;
    using Field   = Handler::FEMContainer_t;
    using Solver  = ippl::FEMPoissonSolver_wFEMContainer<Field, Field, 3, 4>;
    struct Probe : Solver::PCGSolverAlgorithm_t {
        using Solver::PCGSolverAlgorithm_t::preconditioner_m;
    };
    ippl::NDIndex<1> domain(ippl::Vector<unsigned, 1>(5));
    ippl::FieldLayout<1> layout(MPI_COMM_WORLD, domain, std::array<bool, 1>{true}, true);
    ippl::UniformCartesian<double, 1> mesh(domain, ippl::Vector<double, 1>(0.25),
                                           ippl::Vector<double, 1>(0));
    Field x(mesh, layout), b(mesh, layout), one(mesh, layout), out(mesh, layout);
    const std::array<ippl::FieldBC, 2> bc{ippl::PERIODIC_FACE, ippl::PERIODIC_FACE};
    x.setFieldBC(bc);
    b.setFieldBC(bc);
    one.setFieldBC(bc);
    one = 1;
    Solver solver(x, b);
    auto& space          = solver.getSpace();
    constexpr unsigned N = 12;
    Kokkos::View<double*> coefficients("preconditioner column", N);
    auto host = Kokkos::create_mirror_view(coefficients);
    auto fill = [&](Field& field, unsigned column) {
        for (unsigned i = 0; i < N; ++i)
            host(i) = i == column ? 1.0 : i == N - 1 ? -1.0 : 0;
        Kokkos::deep_copy(coefficients, host);
        space.fillFromGlobalCoefficients(field, coefficients);
        field.setFieldBC(bc);
        field.fillHalo();
    };
    for (auto family : {ippl::LagrangeNodeFamily::GLL, ippl::LagrangeNodeFamily::Equispaced}) {
        space.setInterpolationNodes(family);
        ippl::EvalFunctor<double, 1, 4> eval(ippl::Vector<double, 1>(4), 0.25);
        auto a = [&](Field f) {
            f.setFieldBC(bc);
            f.fillHalo();
            return space.evaluateAx(f, eval);
        };
        auto l = [&](Field f) {
            f.setFieldBC(bc);
            f.fillHalo();
            return space.evaluateAx_lower(f, eval);
        };
        auto u = [&](Field f) {
            f.setFieldBC(bc);
            f.fillHalo();
            return space.evaluateAx_upper(f, eval);
        };
        auto ul = [&](Field f) {
            f.setFieldBC(bc);
            f.fillHalo();
            return space.evaluateAx_upperlower(f, eval);
        };
        auto invd = [&](Field f) {
            f.setFieldBC(bc);
            f.fillHalo();
            return space.evaluateAx_inversediag(f, eval);
        };
        auto d = [&](Field f) {
            f.setFieldBC(bc);
            f.fillHalo();
            return space.evaluateAx_diag(f, eval);
        };
        const auto element = space.assembleElementMatrix(eval);
        double bound       = 0;
        for (unsigned i = 0; i < 4; ++i) {
            double row = 0;
            for (unsigned j = 0; j < 4; ++j)
                row += std::abs(element[i][j]);
            bound = std::max(bound, row);
        }
        const double alpha =
            0.5 * 4 * Kokkos::numbers::pi_v<double> * Kokkos::numbers::pi_v<double> * 0.25 / 64;
        const double beta = 2.02 * bound;
        // Check the interval against the full assembled periodic spectrum.
        std::vector<double> stiffness(N * N), vectors;
        for (unsigned j = 0; j < N; ++j) {
            for (unsigned k = 0; k < N; ++k)
                host(k) = k == j ? 1.0 : 0.0;
            Kokkos::deep_copy(coefficients, host);
            space.fillFromGlobalCoefficients(b, coefficients);
            auto ab = a(b);
            for (unsigned i = 0; i < N; ++i) {
                for (unsigned k = 0; k < N; ++k)
                    host(k) = k == i ? 1.0 : 0.0;
                Kokkos::deep_copy(coefficients, host);
                space.fillFromGlobalCoefficients(x, coefficients);
                stiffness[i * N + j] = innerProduct(x, ab);
            }
        }
        ippl::nodes1d::detail::denseSymmetricEigenJacobi(stiffness, vectors, N);
        unsigned nullity = 0;
        for (unsigned i = 0; i < N; ++i) {
            double lambda = stiffness[i * N + i];
            if (std::abs(lambda) < 1e-10)
                ++nullity;
            else {
                EXPECT_GE(lambda, alpha);
                EXPECT_LE(lambda, beta);
            }
        }
        EXPECT_EQ(nullity, 1u);
        for (const std::string label : {"jacobi", "newton", "chebyshev", "chebyshev16",
                                        "gauss-seidel", "gs_2_1", "ssor", "richardson",
                                        "richardson_alt", "richardson3", "richardson_alt3"}) {
            const bool oddRichardson = label == "richardson3" || label == "richardson_alt3";
            const std::string method = label == "gs_2_1"       ? "gauss-seidel"
                                     : label == "chebyshev16" ? "chebyshev"
                                     : oddRichardson          ? label.substr(0, label.size() - 1)
                                                              : label;
            Probe pcg;
            pcg.setPreconditioner(a, l, u, ul, invd, d, alpha, beta, method, 2,
                                  label == "chebyshev16" ? 16 : 8, oddRichardson ? 3 : 4, 2,
                                  label == "gs_2_1" ? 1 : 2, 1.0);
            pcg.preparePreconditioner(x);
            std::vector<double> matrix((N - 1) * (N - 1)), eigenvectors;
            for (unsigned j = 0; j < N - 1; ++j) {
                fill(b, j);
                (*pcg.preconditioner_m)(b, out);
                out -= innerProduct(one, out) / N;
                for (unsigned i = 0; i < N - 1; ++i) {
                    fill(x, i);
                    matrix[i * (N - 1) + j] = innerProduct(x, out);
                }
            }
            double asymmetry = 0, scale = 0;
            for (unsigned i = 0; i < N - 1; ++i)
                for (unsigned j = 0; j < N - 1; ++j) {
                    asymmetry = std::max(
                        asymmetry, std::abs(matrix[i * (N - 1) + j] - matrix[j * (N - 1) + i]));
                    scale = std::max(scale, std::abs(matrix[i * (N - 1) + j]));
                }
            ippl::nodes1d::detail::denseSymmetricEigenJacobi(matrix, eigenvectors, N - 1);
            double minimum = matrix[0];
            for (unsigned i = 1; i < N - 1; ++i)
                minimum = std::min(minimum, matrix[i * (N - 1) + i]);
            std::cout << "periodic_preconditioner," << int(family) << ',' << label << ','
                      << asymmetry << ',' << minimum << '\n';
            if (method == "jacobi" || method == "chebyshev" || method == "newton"
                || method == "gauss-seidel" || method == "ssor" || oddRichardson) {
                EXPECT_LT(asymmetry, 1e-10 * scale) << method;
                EXPECT_GT(minimum, 0) << label;
            } else {
                // These four-step undamped Richardson variants are indefinite here.
                EXPECT_LT(minimum, 0) << label;
            }
        }
    }
}
int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
