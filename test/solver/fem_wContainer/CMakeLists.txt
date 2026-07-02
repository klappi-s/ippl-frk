# -----------------------------------------------------------------------------
# test/solver/fem_wContainer/CMakeLists.txt
#
# Integration tests for Track B FEM Poisson solver (FEMPoissonSolver_wFEMContainer).
# -----------------------------------------------------------------------------

file(RELATIVE_PATH _relPath "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "🔧 Adding FEM wFEMContainer solver integration tests from: ${_relPath}")

# Manual convergence studies (compile-only; run by hand, writes .dat for plotting).
add_ippl_integration_test(TestFEMPoissonSolver1D_zeroDirichlet_convergence
                          COMPILE_ONLY NO_MPI LABELS solver fem integration wFEMContainer)
add_ippl_integration_test(TestFEMPoissonSolver2D_zeroDirichlet_convergence
                          COMPILE_ONLY NO_MPI LABELS solver fem integration wFEMContainer)
add_ippl_integration_test(TestFEMPoissonSolver3D_zeroDirichlet_convergence
                          COMPILE_ONLY NO_MPI LABELS solver fem integration wFEMContainer)
