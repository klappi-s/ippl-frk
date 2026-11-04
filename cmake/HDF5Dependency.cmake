# ------------------------------------------------------------------------------
# Parallel HDF5: shared by H5hut and openPMD source builds.
# Prefer a suitable installation; otherwise build the MPI C library with IPPL.
# ------------------------------------------------------------------------------
include_guard(GLOBAL)
function(ippl_resolve_hdf5)
  get_property(_resolved GLOBAL PROPERTY IPPL_IO_HDF5_TARGET)
  if(_resolved)
    foreach(_name IN ITEMS TARGET FETCHED PACKAGE_HINT VERSION)
      get_property(_value GLOBAL PROPERTY IPPL_IO_HDF5_${_name})
      set(IPPL_IO_HDF5_${_name} "${_value}" PARENT_SCOPE)
    endforeach()
    return()
  endif()
  set(HDF5_VERSION "1.14.6" CACHE STRING "HDF5 release or git.tag/commit")
  set(_requestedVersion "${HDF5_VERSION}")
  extract_git_label(HDF5_VERSION _hdf5Revision)
  set(_reuse OFF)
  if(HDF5_DIR)
    cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${HDF5_DIR}" NORMALIZE _reuse)
  endif()
  set(_preexistingTarget FALSE)
  if(TARGET HDF5::HDF5 OR TARGET hdf5::hdf5 OR TARGET hdf5-static)
    set(_preexistingTarget TRUE)
  endif()
  if(NOT _hdf5Revision AND NOT _reuse)
    set(HDF5_PREFER_PARALLEL TRUE)
    find_package(HDF5 QUIET MODULE COMPONENTS C)
    if(HDF5_FOUND AND HDF5_IS_PARALLEL AND
       HDF5_VERSION VERSION_GREATER_EQUAL _requestedVersion)
      message(STATUS "Parallel HDF5 ${HDF5_VERSION} found externally")
      set_property(GLOBAL PROPERTY IPPL_IO_HDF5_TARGET HDF5::HDF5)
      set_property(GLOBAL PROPERTY IPPL_IO_HDF5_VERSION "${HDF5_VERSION}")
      set(IPPL_IO_HDF5_TARGET HDF5::HDF5 PARENT_SCOPE)
      set(IPPL_IO_HDF5_VERSION "${HDF5_VERSION}" PARENT_SCOPE)
      return()
    endif()
    if(HDF5_FOUND)
      message(STATUS "HDF5 ${HDF5_VERSION} is unsuitable: parallel=${HDF5_IS_PARALLEL}, required version >= ${_requestedVersion}")
    endif()
  endif()
  if(NOT _hdf5Revision)
    set(_hdf5Revision "hdf5_${_requestedVersion}")
  endif()
  if(_preexistingTarget)
    message(FATAL_ERROR "An incompatible HDF5 target is already loaded. Configure all I/O dependencies against one parallel HDF5 installation.")
  endif()
  message(STATUS "Parallel HDF5 ${_hdf5Revision} building from source")

  # Scope generic options to HDF5. Build a position-independent static C library
  # suitable for either static or shared IPPL; no additional filters or tools.
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_STATIC_LIBS ON)
  set(BUILD_TESTING OFF)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(HDF5_ENABLE_PARALLEL ON CACHE BOOL "Build parallel HDF5 for IPPL I/O" FORCE)
  foreach(_option IN ITEMS BUILD_CPP_LIB BUILD_FORTRAN BUILD_JAVA BUILD_HL_LIB
      BUILD_TOOLS BUILD_UTILS BUILD_EXAMPLES BUILD_GENERATORS BUILD_DOC
      ENABLE_Z_LIB_SUPPORT ENABLE_SZIP_SUPPORT ENABLE_PLUGIN_SUPPORT
      ENABLE_ROS3_VFD ENABLE_HDFS ENABLE_SUBFILING_VFD ENABLE_THREADSAFE)
    set(HDF5_${_option} OFF CACHE BOOL "" FORCE)
  endforeach()
  set(HDF5_INSTALL_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/hdf5")
  FetchContent_Declare(hdf5 GIT_REPOSITORY https://github.com/HDFGroup/hdf5.git
    GIT_TAG "${_hdf5Revision}" OVERRIDE_FIND_PACKAGE)
  FetchContent_MakeAvailable(hdf5)
  if(NOT TARGET hdf5-static)
    message(FATAL_ERROR "Fetched HDF5 did not provide its static C target")
  endif()
  # FindHDF5 may have created an unusable aggregate target during this lookup.
  # It has no consumers yet; route it to the fetched library so later discovery
  # cannot accidentally retain the rejected serial installation. Never replace
  # a target supplied by a parent project or an installed dependency.
  if(TARGET HDF5::HDF5)
    set_target_properties(HDF5::HDF5 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ""
      INTERFACE_COMPILE_DEFINITIONS "" INTERFACE_LINK_LIBRARIES hdf5-static)
  endif()
  get_directory_property(_hdf5Version DIRECTORY "${hdf5_SOURCE_DIR}"
    DEFINITION HDF5_PACKAGE_VERSION)

  # HDF5 omits package exports when used as a subproject. Supply those exports
  # and install its own generated config, without modifying upstream sources.
  set(_package "${hdf5_BINARY_DIR}/ippl-package")
  export(EXPORT hdf5-targets FILE "${_package}/hdf5-targets.cmake" NAMESPACE hdf5::)
  install(EXPORT hdf5-targets FILE hdf5-targets.cmake NAMESPACE hdf5::
    DESTINATION "${HDF5_INSTALL_CMAKE_DIR}")
  write_basic_package_version_file("${_package}/hdf5-config-version.cmake"
    VERSION "${_hdf5Version}" COMPATIBILITY SameMinorVersion)
  install(FILES "${hdf5_BINARY_DIR}/CMakeFiles/hdf5-config.cmake"
    "${_package}/hdf5-config-version.cmake" DESTINATION "${HDF5_INSTALL_CMAKE_DIR}")

  # FindHDF5 interrogates installed libraries, which do not exist yet. CMake's
  # FetchContent redirect lets openPMD use the actual source target immediately.
  # The same config imports the generated export for build-tree consumers.
  file(CONFIGURE OUTPUT "${_package}/hdf5-config.cmake" CONTENT [=[
include(CMakeFindDependencyMacro)
find_dependency(MPI COMPONENTS C)
set(HDF5_VERSION "@_hdf5Version@")
set(HDF5_IS_PARALLEL TRUE)
set(HDF5_ENABLE_PARALLEL TRUE)
set(HDF5_C_FOUND TRUE)
set(HDF5_FOUND TRUE)
if(TARGET hdf5-static)
  set(HDF5_LIBRARIES hdf5-static)
else()
  include("${CMAKE_CURRENT_LIST_DIR}/hdf5-targets.cmake")
  set(HDF5_LIBRARIES hdf5::hdf5-static)
endif()
set(HDF5_C_LIBRARIES "${HDF5_LIBRARIES}")
get_target_property(HDF5_INCLUDE_DIRS ${HDF5_LIBRARIES} INTERFACE_INCLUDE_DIRECTORIES)
set(HDF5_INCLUDE_DIR "${HDF5_INCLUDE_DIRS}")
set(HDF5_DEFINITIONS "")
foreach(_component IN LISTS HDF5_FIND_COMPONENTS)
  if(NOT _component STREQUAL "C" AND NOT _component STREQUAL "static")
    set(HDF5_FOUND FALSE)
    set(HDF5_NOT_FOUND_MESSAGE "IPPL's source-built HDF5 provides only the parallel C library")
  endif()
endforeach()
unset(_component)
]=] @ONLY)
  file(CONFIGURE OUTPUT "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/hdf5-config.cmake"
    CONTENT [=[include("@_package@/hdf5-config.cmake")
]=] @ONLY)
  configure_file("${_package}/hdf5-config-version.cmake"
    "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/hdf5-config-version.cmake" COPYONLY)
  set(IPPL_IO_HDF5_FETCHED TRUE PARENT_SCOPE)
  set(IPPL_IO_HDF5_PACKAGE_HINT "${_package}" PARENT_SCOPE)
  set(IPPL_IO_HDF5_TARGET hdf5-static PARENT_SCOPE)
  set(IPPL_IO_HDF5_VERSION "${_hdf5Version}" PARENT_SCOPE)
  set_property(GLOBAL PROPERTY IPPL_IO_HDF5_TARGET hdf5-static)
  set_property(GLOBAL PROPERTY IPPL_IO_HDF5_VERSION "${_hdf5Version}")
  set_property(GLOBAL PROPERTY IPPL_IO_HDF5_FETCHED TRUE)
  set_property(GLOBAL PROPERTY IPPL_IO_HDF5_PACKAGE_HINT "${_package}")
endfunction()

