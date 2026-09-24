file(RELATIVE_PATH _relPath "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "Adding unit tests found in ${_relPath}")

# GaussLegendreQuadrature also carries the former GaussJacobiQuadrature typed test (merged
# 2026-08-17). The weighted Gauss-Chebyshev rule was dropped (see GaussLegendreQuadrature.h);
# Chebyshev node math stays covered by the Nodes1D unit tests.
# Reference-rule checks are rank-local; allow the one-GPU validation allocation.
add_ippl_test(Quadrature NUM_PROCS 1)
add_ippl_test(GaussLegendreQuadrature)
add_ippl_test(GaussLobattoQuadrature)
