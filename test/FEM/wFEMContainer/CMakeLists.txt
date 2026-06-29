# -----------------------------------------------------------------------------
# test/FEM/wFEMContainer/CMakeLists.txt
#
# Track B tools and compile-only demos (wFEMContainer stack).
# -----------------------------------------------------------------------------

file(RELATIVE_PATH _relPath "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "Adding index test found in ${_relPath}")

add_ippl_integration_test(GenerateLagrangeBasisSamples COMPILE_ONLY LABELS integration fem)
# add_ippl_integration_test(GenerateFemTestData COMPILE_ONLY NO_MPI NUM_PROCS 1 LABELS integration fem)
