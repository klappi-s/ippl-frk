find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  if(IPPL_PLATFORMS STREQUAL "SERIAL" AND "HDF5" IN_LIST IPPL_OPENPMD_BACKENDS)
    find_program(IPPL_FEL_H5DIFF h5diff)
    if(IPPL_FEL_H5DIFF)
      set(_compare --exact-hdf5 "${IPPL_FEL_H5DIFF}")
    endif()
  endif()
  add_executable(FELCheckpointCopy FELCheckpointCopy.cpp)
  target_link_libraries(FELCheckpointCopy PRIVATE IPPL::ippl catalyst::catalyst)
  target_include_directories(FELCheckpointCopy PRIVATE "${PROJECT_SOURCE_DIR}/demos/fel")
  target_compile_definitions(FELCheckpointCopy PRIVATE $<TARGET_PROPERTY:FreeElectronLaser,COMPILE_DEFINITIONS>)
  foreach(ranks IN ITEMS 1 2)
    add_test(NAME FELRestart_${ranks}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/FELRestart.py"
              ${_compare} $<TARGET_FILE:FreeElectronLaser> "${PROJECT_SOURCE_DIR}/demos/fel/config.json"
              ${ranks} "${MPIEXEC_EXECUTABLE}" ${MPIEXEC_NUMPROC_FLAG} @RANKS@
              ${MPIEXEC_PREFLAGS} @FEL@ ${MPIEXEC_POSTFLAGS})
    set_tests_properties(FELRestart_${ranks} PROPERTIES TIMEOUT 240 PROCESSORS ${ranks}
      LABELS "integration;fel;openpmd" ENVIRONMENT "OMP_NUM_THREADS=1")
  endforeach()
  add_test(NAME FELRestartRepartition
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/FELRestart.py"
            ${_compare} --copy $<TARGET_FILE:FELCheckpointCopy>
            $<TARGET_FILE:FreeElectronLaser> "${PROJECT_SOURCE_DIR}/demos/fel/config.json"
            1 "${MPIEXEC_EXECUTABLE}" ${MPIEXEC_NUMPROC_FLAG} @RANKS@
            ${MPIEXEC_PREFLAGS} @FEL@ ${MPIEXEC_POSTFLAGS})
  set_tests_properties(FELRestartRepartition PROPERTIES TIMEOUT 240 PROCESSORS 3
    LABELS "integration;fel;openpmd" ENVIRONMENT "OMP_NUM_THREADS=1")
endif()
