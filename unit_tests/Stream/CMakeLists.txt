add_ippl_test(OpenPMDRoundTrip NUM_PROCS 2 TIMEOUT 90)
set_tests_properties(OpenPMDRoundTrip PROPERTIES LABELS "unit;openpmd")
add_test(NAME OpenPMDRoundTripSerial COMMAND $<TARGET_FILE:OpenPMDRoundTrip>)
set_tests_properties(OpenPMDRoundTripSerial PROPERTIES TIMEOUT 90
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/serial" LABELS "unit;openpmd")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/serial")

add_ippl_test(OpenPMDVectorLayouts NUM_PROCS 2 TIMEOUT 120 LABELS openpmd)
add_test(NAME OpenPMDVectorLayoutsSerial COMMAND $<TARGET_FILE:OpenPMDVectorLayouts>)
set_tests_properties(OpenPMDVectorLayoutsSerial PROPERTIES TIMEOUT 120
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/serial" LABELS "unit;openpmd")

if(IPPL_ENABLE_ALPINE)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/landau_stream")
  add_ippl_test(LandauDampingStreamTest NUM_PROCS 2 TIMEOUT 120
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/demos/alpineStream"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/landau_stream"
    LABELS openpmd alpine)
  if(IPPL_ENABLE_LAYOUT_COMPARISON)
    foreach(particle_layout AoS SoA)
      foreach(field_layout AoS SoA)
        set(layout_test "LandauDampingStreamTest_${particle_layout}_${field_layout}")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/layouts/${layout_test}")
        add_ippl_test(${layout_test} SOURCES LandauDampingStreamTest.cpp NUM_PROCS 2 TIMEOUT 120
          INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/demos/alpineStream"
          WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/layouts/${layout_test}" LABELS openpmd alpine)
        target_compile_definitions(${layout_test} PRIVATE
          "IPPL_ALPINE_PARTICLE_LAYOUT=ippl::${particle_layout}"
          "IPPL_ALPINE_FIELD_LAYOUT=ippl::${field_layout}")
      endforeach()
    endforeach()
  endif()
endif()

# Exercise the same tests for every requested backend in independent directories.
get_property(openpmdTests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS openpmdTests)
  if("ADIOS2" IN_LIST IPPL_OPENPMD_BACKENDS)
    if(test MATCHES "Serial$")
      set(processors 1)
      string(REGEX REPLACE "Serial$" "" executable "${test}")
      add_test(NAME ${test}BP5 COMMAND $<TARGET_FILE:${executable}>)
    else()
      set(processors 2)
      add_test(NAME ${test}BP5 COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2
        ${MPIEXEC_PREFLAGS} $<TARGET_FILE:${test}> ${MPIEXEC_POSTFLAGS})
    endif()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${test}_bp5")
    get_property(testLabels TEST ${test} PROPERTY LABELS)
    set_tests_properties(${test}BP5 PROPERTIES TIMEOUT 180
      LABELS "${testLabels};bp5" PROCESSORS ${processors}
      ENVIRONMENT "IPPL_TEST_BP5=1"
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${test}_bp5")
  endif()
  if(NOT "HDF5" IN_LIST IPPL_OPENPMD_BACKENDS)
    set_tests_properties(${test} PROPERTIES DISABLED TRUE)
  endif()
endforeach()
