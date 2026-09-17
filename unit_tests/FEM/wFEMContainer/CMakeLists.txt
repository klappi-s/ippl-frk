file(RELATIVE_PATH _relPath "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "Adding unit tests found in ${_relPath}")

add_ippl_test(DOFHandler)
add_ippl_test(FEMContainerHalo)
add_ippl_test(FEMContainer)
add_ippl_test(LagrangeSpace_firstOrder)
add_ippl_test(LagrangeSpace_higherOrder)
add_ippl_test(LagrangeGLLNodes)
add_ippl_test(FEMOracleRuntimeTest NUM_PROCS 1 LABELS FEM oracle)
add_ippl_test(FEMConstantPreservation NUM_PROCS 1 LABELS FEM operator solver)
include(Oracle/FEMOracleDependency.cmake)
ippl_resolve_fem_oracles()
# Only test targets depend on the package. The IPPL library is unaffected.
add_library(FEMOraclePackage INTERFACE)
target_include_directories(FEMOraclePackage INTERFACE
  "${FEM_ORACLE_PACKAGE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/Oracle")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${FEM_ORACLE_PACKAGE_DIR}/include/FEMOracleRegistry.h")
# Register the same immutable cases and shards for each stiffness mode.
function(add_fem_mpi_run name target ranks label)
  cmake_parse_arguments(RUN "CORRECTED_ONLY" "SHARD;SHARDS" "ARGS" ${ARGN})
  set(_modes standard)
  if(name MATCHES "FEM(Operator|Poisson)Oracle")
    list(APPEND _modes constant_preserving)
  endif()
  foreach(_mode IN LISTS _modes)
    set(_name "${name}")
    if(_mode STREQUAL "standard")
      if(RUN_CORRECTED_ONLY)
        continue()
      endif()
    else()
      string(APPEND _name "_${_mode}")
    endif()
    add_test(NAME ${_name}
      COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} ${ranks} ${MPIEXEC_PREFLAGS}
        $<TARGET_FILE:${target}> ${MPIEXEC_POSTFLAGS} ${RUN_ARGS})
    set_tests_properties(${_name} PROPERTIES TIMEOUT 60 LABELS "unit;FEM;${label};${_mode}"
      PROCESSORS ${ranks} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      ENVIRONMENT "IPPL_FEM_STIFFNESS_MODE=${_mode}")
    if(DEFINED RUN_SHARDS)
      set_property(TEST ${_name} APPEND PROPERTY ENVIRONMENT
        "GTEST_TOTAL_SHARDS=${RUN_SHARDS}" "GTEST_SHARD_INDEX=${RUN_SHARD}")
    endif()
  endforeach()
endfunction()

foreach(_dim RANGE 1 3)
  # Share the small runtime loader across all three suites.
  add_library(FEMOracleData${_dim}D OBJECT FEMOracleTestData.cpp)
  target_compile_features(FEMOracleData${_dim}D PRIVATE cxx_std_20)
  target_compile_definitions(FEMOracleData${_dim}D PRIVATE FEM_ORACLE_DIM=${_dim}
    FEM_ORACLE_DATA_DIR="${FEM_ORACLE_PACKAGE_DIR}/data")
  target_link_libraries(FEMOracleData${_dim}D PRIVATE FEMOraclePackage)
  # One translation unit per generated order bounds compiler memory and lets -j8
  # parallelize these templates. The registry also controls the number of units.
  file(STRINGS "${FEM_ORACLE_PACKAGE_DIR}/include/FEMOracleRegistry.h" _registry_line
    REGEX "^namespace fem_oracle::d${_dim} ")
  string(REGEX MATCHALL "CaseTag<" _registry_tags "${_registry_line}")
  list(LENGTH _registry_tags _tag_count)
  if(_tag_count EQUAL 0)
    message(FATAL_ERROR "No generated FEM case tags for dimension ${_dim}")
  endif()
  math(EXPR _last_tag "${_tag_count} - 1")
  set(_operator_sources)
  foreach(_slot RANGE 0 ${_last_tag})
    set(_unit "${CMAKE_CURRENT_BINARY_DIR}/FEMOperatorOracle${_dim}D_case${_slot}.cpp")
    file(GENERATE OUTPUT "${_unit}" CONTENT
      "#define FEM_ORACLE_ORDER_SLOT ${_slot}\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/FEMOperatorOracle.cpp\"\n")
    list(APPEND _operator_sources "${_unit}")
  endforeach()
  add_ippl_test(FEMReferenceOracle${_dim}D SOURCES FEMReferenceOracle.cpp
    $<TARGET_OBJECTS:FEMOracleData${_dim}D> NUM_PROCS 1 LABELS FEM reference)
  target_compile_definitions(FEMReferenceOracle${_dim}D PRIVATE FEM_ORACLE_DIM=${_dim})
  target_link_libraries(FEMReferenceOracle${_dim}D PRIVATE FEMOraclePackage)
  set(_operator_args)
  if(_dim EQUAL 3)
    list(APPEND _operator_args "--gtest_filter=-*SingleCellStiffnessAndMassColumnsNoFace:*NodalLoadRetainsBoundarySourceSamples:*SourceSamplingUsesPhysicalNodesAndSelectedFamily:*ConstrainedActionsBoundaryLiftAndRhs")
  endif()
  add_ippl_test(FEMOperatorOracle${_dim}D SOURCES FEMOperatorOracleMain.cpp ${_operator_sources} $<TARGET_OBJECTS:FEMOracleData${_dim}D> NUM_PROCS 1 LABELS FEM operator
    ARGS ${_operator_args} INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/test/solver/fem_wContainer)
  target_compile_definitions(FEMOperatorOracle${_dim}D PRIVATE FEM_ORACLE_DIM=${_dim})
  target_link_libraries(FEMOperatorOracle${_dim}D PRIVATE FEMOraclePackage)
  if(BUILD_TESTING)
    set_property(TEST FEMOperatorOracle${_dim}D APPEND PROPERTY ENVIRONMENT
      "IPPL_FEM_STIFFNESS_MODE=standard")
    add_fem_mpi_run(FEMOperatorOracle${_dim}D FEMOperatorOracle${_dim}D 1 operator
      CORRECTED_ONLY ARGS ${_operator_args})
  endif()
endforeach()
# Keep exhaustive 3D columns under the normal per-test timeout without compiling
# duplicate binaries. Across shards, column % 16 assigns every column exactly once.
if(BUILD_TESTING)
  # Load assembly integrates its mass terms per cell on each call. Keep those
  # stages separate by precision so P5 also fits the same 60-second budget.
  foreach(_precision IN ITEMS Double Float)
    foreach(_stage IN ITEMS NodalLoadRetainsBoundarySourceSamples
        SourceSamplingUsesPhysicalNodesAndSelectedFamily ConstrainedActionsBoundaryLiftAndRhs)
      set(_test FEMOperatorOracle3D_${_precision}_${_stage})
      add_fem_mpi_run(${_test} FEMOperatorOracle3D 1 operator
        ARGS "--gtest_filter=*${_precision}*.${_stage}")
    endforeach()
  endforeach()
  foreach(_shard RANGE 0 15)
    add_fem_mpi_run(FEMOperatorOracle3D_columns_${_shard} FEMOperatorOracle3D 1 operator
      ARGS "--gtest_filter=*SingleCellStiffnessAndMassColumnsNoFace"
        --fem-column-shard=${_shard} --fem-column-shards=16)
  endforeach()
endif()
# add_ippl_test(MatrixFreeVsExplicit)
# add_ippl_test(TestCoreGridPoisson_wFEMContainer)

foreach(_dim RANGE 1 3)
  set(_solve_args)
  if(_dim EQUAL 3)
    list(APPEND _solve_args "--gtest_filter=FEMPoissonRegistry.*")
  endif()
  add_ippl_test(FEMPoissonOracle${_dim}D
    SOURCES FEMPoissonOracle.cpp $<TARGET_OBJECTS:FEMOracleData${_dim}D> NUM_PROCS 1 LABELS FEM solver
    ARGS ${_solve_args})
  target_compile_definitions(FEMPoissonOracle${_dim}D PRIVATE FEM_ORACLE_DIM=${_dim})
  target_link_libraries(FEMPoissonOracle${_dim}D PRIVATE FEMOraclePackage)
  if(BUILD_TESTING)
    set_property(TEST FEMPoissonOracle${_dim}D APPEND PROPERTY ENVIRONMENT
      "IPPL_FEM_STIFFNESS_MODE=standard")
    add_fem_mpi_run(FEMPoissonOracle${_dim}D FEMPoissonOracle${_dim}D 1 solver
      CORRECTED_ONLY ARGS ${_solve_args})
  endif()
endforeach()

# Reuse the same executable/data on every rank count. GoogleTest sharding keeps
# expensive 3D solves within the normal timeout and follows generated cases automatically.

if(BUILD_TESTING)
  foreach(_dim RANGE 1 2)
    add_fem_mpi_run(FEMOperatorOracle${_dim}D_mpi2 FEMOperatorOracle${_dim}D 2 operator
      ARGS "--gtest_filter=-*SingleCellStiffnessAndMassColumnsNoFace")
    add_fem_mpi_run(FEMPoissonOracle${_dim}D_mpi2 FEMPoissonOracle${_dim}D 2 solver)
  endforeach()
  add_fem_mpi_run(FEMOperatorOracle3D_mpi2 FEMOperatorOracle3D 2 operator
    ARGS ${_operator_args})
  foreach(_precision IN ITEMS Double Float)
    foreach(_stage IN ITEMS NodalLoadRetainsBoundarySourceSamples
        SourceSamplingUsesPhysicalNodesAndSelectedFamily ConstrainedActionsBoundaryLiftAndRhs)
      add_fem_mpi_run(FEMOperatorOracle3D_${_precision}_${_stage}_mpi2
        FEMOperatorOracle3D 2 operator ARGS "--gtest_filter=*${_precision}*.${_stage}")
    endforeach()
  endforeach()
  foreach(_ranks IN ITEMS 1 2)
    foreach(_shard RANGE 0 31)
      add_fem_mpi_run(FEMPoissonOracle3D_mpi${_ranks}_${_shard} FEMPoissonOracle3D ${_ranks} solver
        SHARD ${_shard} SHARDS 32 ARGS "--gtest_filter=FEMPoissonOracle.*")
    endforeach()
  endforeach()
  add_fem_mpi_run(FEMOperatorOracle3D_mpi4 FEMOperatorOracle3D 4 operator
    ARGS "--gtest_filter=*DoubleP3*")
  foreach(_shard RANGE 0 15)
    add_fem_mpi_run(FEMPoissonOracle3D_mpi4_${_shard} FEMPoissonOracle3D 4 solver
      SHARD ${_shard} SHARDS 16
      ARGS "--gtest_filter=*p3_3d_*_stretched*:*p5_3d_*")
  endforeach()
endif()

add_ippl_test(FEMThinHalo NUM_PROCS 1 LABELS FEM halo)
if(BUILD_TESTING)
  add_fem_mpi_run(FEMThinHalo_mpi2 FEMThinHalo 2 halo)
  add_fem_mpi_run(FEMThinHalo_mpi4 FEMThinHalo 4 halo)
endif()
