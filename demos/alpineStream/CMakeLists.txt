if(NOT IPPL_ENABLE_FFT)
  message(FATAL_ERROR "The alpineStream demo requires IPPL_ENABLE_FFT=ON")
endif()

add_executable(LandauDampingStream LandauDampingStream.cpp)
target_link_libraries(LandauDampingStream PRIVATE IPPL::ippl)
