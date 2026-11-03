if(NOT IPPL_ENABLE_FFT)
  message(FATAL_ERROR "The alpineStream demo requires IPPL_ENABLE_FFT=ON")
endif()

add_executable(LandauDampingStream LandauDampingStream.cpp)
target_link_libraries(LandauDampingStream PRIVATE IPPL::ippl)

if(IPPL_ENABLE_LAYOUT_COMPARISON)
  add_custom_target(LandauDampingStreamLayouts)
  foreach(particle_layout AoS SoA)
    foreach(field_layout AoS SoA)
      set(layout_target "LandauDampingStream_${particle_layout}_${field_layout}")
      add_executable(${layout_target} EXCLUDE_FROM_ALL LandauDampingStream.cpp)
      add_dependencies(LandauDampingStreamLayouts ${layout_target})
      target_link_libraries(${layout_target} PRIVATE IPPL::ippl)
      target_compile_definitions(${layout_target} PRIVATE
        "IPPL_ALPINE_PARTICLE_LAYOUT=ippl::${particle_layout}"
        "IPPL_ALPINE_FIELD_LAYOUT=ippl::${field_layout}")
    endforeach()
  endforeach()
endif()
