add_ippl_test(OpenPMDRoundTrip NUM_PROCS 2 TIMEOUT 90)
set_tests_properties(OpenPMDRoundTrip PROPERTIES LABELS "unit;openpmd")
add_test(NAME OpenPMDRoundTripSerial COMMAND $<TARGET_FILE:OpenPMDRoundTrip>)
set_tests_properties(OpenPMDRoundTripSerial PROPERTIES TIMEOUT 90
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/serial" LABELS "unit;openpmd")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/serial")
