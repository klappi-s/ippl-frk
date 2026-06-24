# -----------------------------------------------------------------------------
# test/FEM/FEMContainer/CMakeLists.txt
#
# Integration tests for IPPL FEMContainer.
# -----------------------------------------------------------------------------

file(RELATIVE_PATH _relPath "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "Adding index test found in ${_relPath}")

add_ippl_integration_test(TestFEMContainer COMPILE_ONLY LABELS integration fem)
add_ippl_integration_test(TestLagrangeSpace1D_wFEMContainer COMPILE_ONLY LABELS integration fem)
add_ippl_integration_test(TestLagrangeSpace2D_wFEMContainer COMPILE_ONLY LABELS integration fem)
