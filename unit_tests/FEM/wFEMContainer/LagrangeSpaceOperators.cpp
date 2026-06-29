// Matrix-free certification tests for Track B LagrangeSpace_wFEMContainer.
//
// Compares matrix-free stiffness application, load assembly, explicit dense
// residuals, scatter/gather consistency, and matrix-free residual-at-solution
// against the explicit validation oracle on the shared core grid.
#include "Ippl.h"

#include "FEM/Validation/FEMMatrixFreeComparison.h"
#include "gtest/gtest.h"

namespace {

constexpr double kTol = 1e-10;

template <unsigned Dim, unsigned Order>
void expectSmallRelativeError(const std::function<double(std::size_t)>& pattern) {
    const double rel =
        ippl::femvalidate::relativeErrorEvaluateAxVsExplicit<Dim, Order, 9>(pattern);
    EXPECT_LT(rel, kTol) << "dim=" << Dim << " order=" << Order << " rel=" << rel;
}

template <unsigned Dim, unsigned Order>
void expectSmallLoadError(
    const std::function<double(const ippl::Vector<double, Dim>&)>& source) {
    const double rel =
        ippl::femvalidate::relativeErrorEvaluateLoadVectorVsExplicit<Dim, Order, 9>(source);
    EXPECT_LT(rel, kTol) << "dim=" << Dim << " order=" << Order << " rel=" << rel;
}

template <unsigned Dim>
auto poissonSource() {
    return [](const ippl::Vector<double, Dim>& x) -> double {
        if constexpr (Dim == 1) {
            return 2.0 * x[0];
        } else if constexpr (Dim == 2) {
            return x[0] + x[1];
        } else {
            return x[0] + x[1] + x[2];
        }
    };
}

template <unsigned Dim, unsigned Order>
void expectSmallExplicitResidual(
    const std::function<double(const ippl::Vector<double, Dim>&)>& source) {
    const double rel =
        ippl::femvalidate::relativeExplicitSolutionResidual<Dim, Order, 9>(source);
    EXPECT_LT(rel, kTol) << "dim=" << Dim << " order=" << Order << " rel=" << rel;

    const double sg =
        ippl::femvalidate::relativeGlobalVectorScatterGatherError<Dim, Order, 9>(
            ippl::femvalidate::solveExplicitCorePoisson<Dim, Order, 9>(source, 0.0));
    EXPECT_LT(sg, kTol) << "scatter/gather dim=" << Dim << " order=" << Order << " rel=" << sg;

    const double mf =
        ippl::femvalidate::relativeMatrixFreeResidualAtExplicitSolution<Dim, Order, 9>(source);
    EXPECT_LT(mf, kTol) << "mf residual dim=" << Dim << " order=" << Order << " rel=" << mf;
}

template <unsigned Dim, unsigned Order>
void runCase() {
    expectSmallRelativeError<Dim, Order>([](std::size_t) { return 1.25; });
    expectSmallRelativeError<Dim, Order>(
        [](std::size_t g) { return 1.0 + 0.1 * std::sin(static_cast<double>(g)); });

    expectSmallLoadError<Dim, Order>([](const ippl::Vector<double, Dim>&) { return 1.25; });
    expectSmallLoadError<Dim, Order>(poissonSource<Dim>());
    expectSmallExplicitResidual<Dim, Order>(poissonSource<Dim>());
}

}  // namespace

#define MATRIX_FREE_CASE(dim, order) \
    TEST(MatrixFreeVsExplicit, Dim##dim##Order##order) { runCase<dim, order>(); }

MATRIX_FREE_CASE(1, 1)
MATRIX_FREE_CASE(1, 2)
MATRIX_FREE_CASE(1, 3)
MATRIX_FREE_CASE(2, 1)
MATRIX_FREE_CASE(2, 2)
MATRIX_FREE_CASE(2, 3)
MATRIX_FREE_CASE(3, 1)
MATRIX_FREE_CASE(3, 2)
MATRIX_FREE_CASE(3, 3)

int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    ippl::finalize();
    return rc;
}
