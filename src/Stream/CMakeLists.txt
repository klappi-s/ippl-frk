# -----------------------------------------------------------------------------
# src/Stream/CMakeLists.txt
# -----------------------------------------------------------------------------
add_subdirectory(InSitu)
add_subdirectory(Registry)
add_subdirectory(Steering)

if(IPPL_ENABLE_OPENPMD)
  add_subdirectory(OpenPMD)
  target_link_libraries(ippl PUBLIC openPMD::openPMD)
  target_compile_definitions(ippl PUBLIC IPPL_ENABLE_OPENPMD)
endif()
