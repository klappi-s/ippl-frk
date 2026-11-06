# -----------------------------------------------------------------------------
# demos/alpine/CMakeLists.txt
#
# This module provides example physics applications using IPPL.
# -----------------------------------------------------------------------------
message(STATUS "Configuring demos/alpine/")

add_subdirectory(ExamplesWithoutPicManager)

function(add_alpine_example name)
  add_executable(${name} ${name}.cpp)

  target_link_libraries(${name} PRIVATE IPPL::ippl)

  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

if(IPPL_ENABLE_TESTS)
  # cmake-format: off

  set(LANDAU_RANKS 2)
  set(LANDAU_CSV "${PROJECT_BINARY_DIR}/demos/alpine/data/FieldLandau_${LANDAU_RANKS}_manager.csv")

  # Create the data directory and remove any stale CSV so the LandauDamping
  # fixture starts clean. We use bash instead of ${CMAKE_COMMAND} because the
  # build and test phases may run with different uenv mount points, making the
  # absolute cmake path from the build phase unavailable at test time.
  add_test(NAME LandauDampingInit
    COMMAND bash -c "mkdir -p '${PROJECT_BINARY_DIR}/demos/alpine/data' && rm -f '${LANDAU_CSV}'")
  set_tests_properties(LandauDampingInit PROPERTIES FIXTURES_SETUP landau_fixture)

  # Add the test
  add_ippl_integration_test(LandauDamping 
    ARGS "16" "16" "16" "10000000" "25" "FFT" "0.01" "LeapFrog" "--overallocate" "2.0" "--info" "10"
    LABELS alpine integration
    NUM_PROCS ${LANDAU_RANKS}
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/demos/alpine/")

  set_tests_properties(LandauDamping PROPERTIES FIXTURES_REQUIRED landau_fixture)
    
  # cmake-format: on
else()
  add_alpine_example(LandauDamping)
endif()

add_alpine_example(PenningTrap)
add_alpine_example(BumponTailInstability)

if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    string(TOLOWER "${IPPL_PLATFORMS}" _platforms)
    set(_benchmark_backend Serial)
    if(_platforms MATCHES "cuda")
      set(_benchmark_backend Cuda)
    elseif(_platforms MATCHES "hip")
      set(_benchmark_backend HIP)
    elseif(_platforms MATCHES "sycl")
      set(_benchmark_backend SYCL)
    elseif(_platforms MATCHES "openmp")
      set(_benchmark_backend OpenMP)
    endif()
    foreach(_ranks IN ITEMS 1 2)
      add_test(NAME LandauDampingBenchmark_MPI${_ranks}
        COMMAND ${CMAKE_COMMAND}
          -DAPP=$<TARGET_FILE:LandauDamping> -DPYTHON=${Python3_EXECUTABLE}
          -DBACKEND=${_benchmark_backend}
          -DMPIEXEC=${MPIEXEC_EXECUTABLE} -DRANK_FLAG=${MPIEXEC_NUMPROC_FLAG} -DRANKS=${_ranks}
          -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/benchmark-smoke-${_ranks}
          -DRUNNER=${CMAKE_CURRENT_SOURCE_DIR}/../electrostaticFEM/experiments/run_mesh_sweep.py
          -P ${CMAKE_CURRENT_SOURCE_DIR}/BenchmarkSmoke.cmake)
      set_tests_properties(LandauDampingBenchmark_MPI${_ranks} PROPERTIES TIMEOUT 120 PROCESSORS ${_ranks} LABELS "alpine;demo")
    endforeach()
  endif()
endif()

add_subdirectory(validation)
