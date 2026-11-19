# -----------------------------------------------------------------------------
# src/Stream/CMakeLists.txt
# -----------------------------------------------------------------------------
add_subdirectory(InSitu)
add_subdirectory(Registry)
add_subdirectory(Steering)

if(IPPL_ENABLE_OPENPMD OR IPPL_ENABLE_H5HUT)
  add_subdirectory(IO)
endif()
