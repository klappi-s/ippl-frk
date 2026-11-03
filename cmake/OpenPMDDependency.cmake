# openPMD dependency setup, called from Dependencies.cmake when I/O is enabled.
# Installed packages are preferred; missing packages or git revisions use FetchContent.
# Function scope keeps temporary variables and non-cache options out of the caller.

# ------------------------------------------------------------------------------
# Parallel HDF5: needed only for an openPMD source build with the HDF5 backend.
# Prefer a suitable installation; otherwise build the MPI C library with IPPL.
# ------------------------------------------------------------------------------
function(ippl_resolve_hdf5)
  set(HDF5_VERSION "1.14.6" CACHE STRING "HDF5 release or git.tag/commit")
  set(_requestedVersion "${HDF5_VERSION}")
  extract_git_label(HDF5_VERSION _hdf5Revision)
  set(_reuse OFF)
  if(HDF5_DIR)
    cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${HDF5_DIR}" NORMALIZE _reuse)
  endif()
  if(NOT _hdf5Revision AND NOT _reuse)
    set(HDF5_PREFER_PARALLEL TRUE)
    find_package(HDF5 QUIET MODULE COMPONENTS C)
    if(HDF5_FOUND AND HDF5_IS_PARALLEL AND
       HDF5_VERSION VERSION_GREATER_EQUAL _requestedVersion)
      message(STATUS "Parallel HDF5 ${HDF5_VERSION} found externally")
      return()
    endif()
    if(HDF5_FOUND)
      message(STATUS "HDF5 ${HDF5_VERSION} is unsuitable: parallel=${HDF5_IS_PARALLEL}, required version >= ${_requestedVersion}")
    endif()
  endif()
  if(NOT _hdf5Revision)
    set(_hdf5Revision "hdf5_${_requestedVersion}")
  endif()
  message(STATUS "Parallel HDF5 ${_hdf5Revision} building from source")

  # Scope generic options to HDF5. Build a position-independent static C library
  # suitable for either static or shared IPPL; no additional filters or tools.
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_STATIC_LIBS ON)
  set(BUILD_TESTING OFF)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(HDF5_ENABLE_PARALLEL ON CACHE BOOL "Build parallel HDF5 for openPMD" FORCE)
  foreach(_option IN ITEMS BUILD_CPP_LIB BUILD_FORTRAN BUILD_JAVA BUILD_HL_LIB
      BUILD_TOOLS BUILD_EXAMPLES BUILD_GENERATORS BUILD_DOC
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
  set(IPPL_OPENPMD_HDF5_FETCHED TRUE PARENT_SCOPE)
  set(IPPL_OPENPMD_HDF5_PACKAGE_HINT "${_package}" PARENT_SCOPE)
endfunction()

# ------------------------------------------------------------------------------
# ADIOS2: needed only when building openPMD from source with the BP5 backend.
# Installed openPMD packages resolve their own ADIOS2 dependency.
# ------------------------------------------------------------------------------
function(ippl_resolve_adios2)
  set(ADIOS2_VERSION "2.12.1" CACHE STRING "ADIOS2 release or git.tag/commit")
  extract_git_label(ADIOS2_VERSION _adiosRevision)
  # A previous configure's package inside this build is not an external install.
  set(_reuse OFF)
  if(ADIOS2_DIR)
    cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${ADIOS2_DIR}" NORMALIZE _reuse)
  endif()
  if(NOT _adiosRevision AND NOT _reuse)
    find_package(ADIOS2 ${ADIOS2_VERSION} CONFIG QUIET COMPONENTS CXX MPI)
    if(ADIOS2_FOUND)
      set(IPPL_ADIOS2_PACKAGE_HINT "${ADIOS2_DIR}" PARENT_SCOPE)
      return()
    endif()
    if(TARGET adios2::cxx OR (ADIOS2_DIR AND NOT ADIOS2_DIR MATCHES "NOTFOUND$"))
      message(FATAL_ERROR "ADIOS2 at '${ADIOS2_DIR}' lacks the requested version/CXX/MPI support. Use a compatible package or ADIOS2_VERSION=git.v2.12.1.")
    endif()
  endif()
  if(NOT _adiosRevision)
    set(_adiosRevision "v${ADIOS2_VERSION}")
  endif()
  # Build only the MPI file-I/O support needed by openPMD; omit optional engines,
  # language bindings and compression integrations from this fallback build.
  set(BUILD_TESTING OFF)
  set(ADIOS2_USE_MPI ON CACHE STRING "" FORCE)
  set(ADIOS2_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  foreach(_feature IN ITEMS SST HDF5 Fortran Python BigWhoop Blosc2 BZip2 Caliper Campaign
      Catalyst CUDA CURL DAOS DataMan DataSpaces HDF5_VOL IME LIBPRESSIO MGARD MHS
      OpenSSL PNG PRODM Profiling Sodium SZ SZ3 SysVShMem UCX XRootD ZeroMQ ZFP)
    set(ADIOS2_USE_${_feature} OFF CACHE STRING "" FORCE)
  endforeach()
  FetchContent_Declare(adios2 GIT_REPOSITORY https://github.com/ornladios/ADIOS2.git
    GIT_TAG "${_adiosRevision}")
  get_property(_hadInstallDir CACHE CMAKE_INSTALL_CMAKEDIR PROPERTY TYPE SET)
  FetchContent_MakeAvailable(adios2)
  # ADIOS2 creates this generic cache entry for its own package directory.
  # Do not leave a new value behind for unrelated dependency projects.
  if(NOT _hadInstallDir)
    unset(CMAKE_INSTALL_CMAKEDIR CACHE)
  endif()
  if(NOT TARGET adios2::cxx_mpi)
    message(FATAL_ERROR "Fetched ADIOS2 did not provide its MPI C++ target")
  endif()
  # openPMD calls find_package(ADIOS2) during this same configure, before ADIOS2's
  # export() writes its target files. Generate a build-only config that checks the
  # existing targets, using the upstream version file for version compatibility.
  # Installed consumers use ADIOS2's normal package, not this bridge.
  file(GLOB _versions "${adios2_BINARY_DIR}/lib*/cmake/adios2/adios2-config-version.cmake"
    "${adios2_BINARY_DIR}/adios2-config-version.cmake")
  list(LENGTH _versions _count)
  if(NOT _count EQUAL 1)
    message(FATAL_ERROR "Cannot identify fetched ADIOS2 package version")
  endif()
  list(GET _versions 0 _versionFile)
  include("${_versionFile}")
  set(IPPL_ADIOS2_VERSION "${PACKAGE_VERSION}")
  set(_bridge "${adios2_BINARY_DIR}/ippl-package")
  file(MAKE_DIRECTORY "${_bridge}")
  file(CONFIGURE OUTPUT "${_bridge}/adios2-config.cmake" CONTENT [=[
# Build-only bridge to ADIOS2 targets already created by FetchContent.
set(ADIOS2_VERSION "@IPPL_ADIOS2_VERSION@")
set(ADIOS2_CXX_FOUND FALSE)
set(ADIOS2_MPI_FOUND FALSE)
if(TARGET adios2::cxx)
  set(ADIOS2_CXX_FOUND TRUE)
endif()
if(TARGET adios2::cxx_mpi)
  set(ADIOS2_MPI_FOUND TRUE)
endif()
set(ADIOS2_HAVE_MPI "${ADIOS2_MPI_FOUND}")
set(ADIOS2_FOUND TRUE)
foreach(_component IN LISTS ADIOS2_FIND_COMPONENTS)
  if(NOT ADIOS2_${_component}_FOUND)
    set(ADIOS2_FOUND FALSE)
    set(ADIOS2_NOT_FOUND_MESSAGE "Fetched ADIOS2 lacks ${_component}")
  endif()
endforeach()
unset(_component)
]=] @ONLY)
  configure_file("${_versionFile}" "${_bridge}/adios2-config-version.cmake" COPYONLY)
  set(IPPL_ADIOS2_PACKAGE_HINT "${_bridge}" PARENT_SCOPE)
endfunction()

# ------------------------------------------------------------------------------
# openPMD: validate backend selection, then find or build the matching package.
# Return package hints/components for IPPL's build-tree and installed exports.
# ------------------------------------------------------------------------------
function(ippl_setup_openpmd)
  set(IPPL_OPENPMD_HDF5_FETCHED FALSE PARENT_SCOPE)
  set(openPMD_VERSION "0.17.1" CACHE STRING "openPMD release or git.tag/commit")
  set(_components MPI)
  foreach(_backend IN LISTS IPPL_OPENPMD_BACKENDS)
    if(NOT _backend MATCHES "^(HDF5|ADIOS2)$")
      message(FATAL_ERROR "IPPL_OPENPMD_BACKENDS accepts HDF5 and ADIOS2; got '${_backend}'")
    endif()
    list(APPEND _components "${_backend}")
  endforeach()
  if(NOT IPPL_OPENPMD_BACKENDS)
    message(FATAL_ERROR "IPPL_OPENPMD_BACKENDS must select at least one backend")
  endif()
  list(REMOVE_DUPLICATES _components)
  extract_git_label(openPMD_VERSION _openpmdRevision)
  # Recreate source-built targets on reconfigure instead of importing this build.
  set(_reuse OFF)
  if(openPMD_DIR)
    cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${openPMD_DIR}" NORMALIZE _reuse)
  endif()
  # Backend source revisions require rebuilding openPMD against those revisions.
  if((ADIOS2_VERSION MATCHES "^git\\." AND "ADIOS2" IN_LIST IPPL_OPENPMD_BACKENDS) OR
     (HDF5_VERSION MATCHES "^git\\." AND "HDF5" IN_LIST IPPL_OPENPMD_BACKENDS))
    if(NOT _openpmdRevision)
      set(_openpmdRevision "${openPMD_VERSION}")
    endif()
  endif()
  if(NOT _openpmdRevision AND NOT _reuse)
    find_package(openPMD ${openPMD_VERSION} CONFIG QUIET COMPONENTS ${_components})
    if(openPMD_FOUND)
      set(IPPL_OPENPMD_PACKAGE_HINT "${openPMD_DIR}" PARENT_SCOPE)
      set(IPPL_OPENPMD_COMPONENTS "${_components}" PARENT_SCOPE)
      return()
    endif()
    if(TARGET openPMD::openPMD OR (openPMD_DIR AND NOT openPMD_DIR MATCHES "NOTFOUND$"))
      message(FATAL_ERROR "openPMD at '${openPMD_DIR}' lacks requested components ${_components}. Use a compatible package or openPMD_VERSION=git.0.17.1.")
    endif()
  endif()
  if(NOT _openpmdRevision)
    set(_openpmdRevision "${openPMD_VERSION}")
  endif()
  # Set backend options before adding the source project. These cannot change
  # capabilities of an installed openPMD package, which is checked above.
  set(openPMD_USE_MPI ON CACHE STRING "" FORCE)
  foreach(_backend IN ITEMS HDF5 ADIOS2)
    if(_backend IN_LIST IPPL_OPENPMD_BACKENDS)
      set(openPMD_USE_${_backend} ON CACHE STRING "" FORCE)
    else()
      set(openPMD_USE_${_backend} OFF CACHE STRING "" FORCE)
    endif()
  endforeach()
  if("ADIOS2" IN_LIST IPPL_OPENPMD_BACKENDS)
    ippl_resolve_adios2()
    set(ADIOS2_DIR "${IPPL_ADIOS2_PACKAGE_HINT}")
  endif()
  if("HDF5" IN_LIST IPPL_OPENPMD_BACKENDS)
    ippl_resolve_hdf5()
    set(IPPL_OPENPMD_HDF5_FETCHED "${IPPL_OPENPMD_HDF5_FETCHED}" PARENT_SCOPE)
    set(IPPL_OPENPMD_HDF5_PACKAGE_HINT "${IPPL_OPENPMD_HDF5_PACKAGE_HINT}" PARENT_SCOPE)
  endif()
  set(openPMD_USE_PYTHON OFF CACHE STRING "" FORCE)
  foreach(_option IN ITEMS BUILD_TESTING BUILD_EXAMPLES BUILD_CLI_TOOLS)
    set(openPMD_${_option} OFF CACHE BOOL "" FORCE)
  endforeach()
  set(openPMD_INSTALL ON CACHE BOOL "" FORCE)
  # Use openPMD's own setting, independent of ADIOS2's generic install path.
  if(NOT openPMD_INSTALL_CMAKEDIR)
    set(openPMD_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/openPMD")
  endif()
  FetchContent_Declare(openpmd GIT_REPOSITORY https://github.com/openPMD/openPMD-api.git
    GIT_TAG "${_openpmdRevision}")
  FetchContent_MakeAvailable(openpmd)
  # Make this build usable by downstream find_package(IPPL) consumers as well.
  export(EXPORT openPMDTargets FILE "${openpmd_BINARY_DIR}/openPMDTargets.cmake"
    NAMESPACE openPMD::)
  if(NOT TARGET openPMD::openPMD)
    message(FATAL_ERROR "Fetched openPMD did not provide openPMD::openPMD")
  endif()
  set(IPPL_OPENPMD_PACKAGE_HINT "${openpmd_BINARY_DIR}" PARENT_SCOPE)
  set(IPPL_OPENPMD_COMPONENTS "${_components}" PARENT_SCOPE)
endfunction()
