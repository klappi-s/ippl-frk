# Shared ADIOS2 dependency setup. Resolve MPI/C++ support only when requested by
# a consumer; prefer installed packages and otherwise use FetchContent.
# The current source defaults support BP5. A future Catalyst integration can call
# this helper; additional streaming/engine requirements need separate configuration.
include_guard(GLOBAL)

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
  # Keep the source fallback limited to MPI file I/O with C++ bindings. Optional
  # engines, language bindings and compression integrations remain disabled.
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
  # Downstream projects may call find_package(ADIOS2) during this same configure,
  # before export() writes its target files. Generate a build-only config checking
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
