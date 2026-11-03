# Optional native H5Part/H5Block I/O. Prefer an existing compatible package and
# otherwise build the pinned CMake-capable source with the shared HDF5 target.
include(HDF5Dependency)

function(ippl_setup_h5hut)
  set(H5hut_VERSION "2.0.0" CACHE STRING "H5hut release or git.tag/commit")
  extract_git_label(H5hut_VERSION _revision)
  ippl_resolve_hdf5()
  foreach(_name IN ITEMS FETCHED PACKAGE_HINT)
    set(IPPL_IO_HDF5_${_name} "${IPPL_IO_HDF5_${_name}}" PARENT_SCOPE)
  endforeach()
  find_package(MPI REQUIRED COMPONENTS C)
  set(_reuse FALSE)
  if(H5hut_DIR)
    cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${H5hut_DIR}" NORMALIZE _reuse)
  endif()
  if(HDF5_VERSION MATCHES "^git\\." AND NOT _revision)
    set(_revision "b5c74f760bc9919d409f46b5472e9eedf9a764d5")
  endif()
  if(NOT _revision AND NOT _reuse AND NOT TARGET H5hut::H5hut AND NOT TARGET H5hut)
    find_package(H5hut ${H5hut_VERSION} CONFIG QUIET)
    if(NOT H5hut_FOUND)
      if(H5hut_DIR AND NOT H5hut_DIR MATCHES "NOTFOUND$")
        message(FATAL_ERROR "Incompatible H5hut package at ${H5hut_DIR}")
      endif()
      # Some installations provide only headers and a library, without a config package.
      find_path(H5hut_INCLUDE_DIR H5hut.h
        HINTS ${H5hut_ROOT} ENV H5hut_ROOT ${H5HUT_ROOT} ENV H5HUT_ROOT
        PATH_SUFFIXES include)
      find_library(H5hut_LIBRARY NAMES H5hut h5hut
        HINTS ${H5hut_ROOT} ENV H5hut_ROOT ${H5HUT_ROOT} ENV H5HUT_ROOT
        PATH_SUFFIXES lib lib64)
      mark_as_advanced(H5hut_INCLUDE_DIR H5hut_LIBRARY)
      if(H5hut_INCLUDE_DIR AND H5hut_LIBRARY)
        add_library(H5hut::H5hut UNKNOWN IMPORTED)
        set_target_properties(H5hut::H5hut PROPERTIES
          IMPORTED_LOCATION "${H5hut_LIBRARY}"
          INTERFACE_INCLUDE_DIRECTORIES "${H5hut_INCLUDE_DIR}"
          INTERFACE_LINK_LIBRARIES "${IPPL_IO_HDF5_TARGET};MPI::MPI_C")
        set(H5hut_FOUND TRUE)
      endif()
    endif()
  endif()

  if(TARGET H5hut::H5hut)
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_LIBRARIES H5hut::H5hut MPI::MPI_C ${IPPL_IO_HDF5_TARGET})
    unset(IPPL_H5HUT_MPI_API CACHE)
    check_c_source_compiles("#include <mpi.h>
#include <H5hut.h>
#ifndef H5_HAVE_PARALLEL
#error HDF5 must support MPI
#endif
int main(void) { MPI_Comm comm = MPI_COMM_WORLD; h5_prop_t p = H5CreateFileProp();
return H5SetPropFileMPIOCollective(p, &comm) + H5CloseProp(p); }" IPPL_H5HUT_MPI_API)
    if(NOT IPPL_H5HUT_MPI_API)
      message(FATAL_ERROR "H5hut does not provide the required parallel 2.0 API")
    endif()
    set(IPPL_H5HUT_PACKAGE_HINT "${H5hut_DIR}" PARENT_SCOPE)
    # Installed IPPL repeats header/library discovery when no config package is available.
    if(NOT H5hut_DIR OR H5hut_DIR MATCHES "NOTFOUND$")
      set(IPPL_H5HUT_HEADER_LIBRARY TRUE PARENT_SCOPE)
    endif()
    return()
  endif()

  if(NOT TARGET H5hut)
    if(NOT _revision)
      if(NOT H5hut_VERSION STREQUAL "2.0.0")
        message(FATAL_ERROR "No pinned H5hut source for ${H5hut_VERSION}; use git.<revision>")
      endif()
      set(_revision "b5c74f760bc9919d409f46b5472e9eedf9a764d5")
    endif()
    # Upstream expects these names rather than FindHDF5's imported target.
    if(NOT TARGET hdf5-static AND NOT TARGET hdf5-shared)
      add_library(hdf5-static ALIAS ${IPPL_IO_HDF5_TARGET})
    endif()
    set(HDF5_FOUND TRUE)
    set(HDF5_ENABLE_PARALLEL TRUE)
    set(HDF5_VERSION "${IPPL_IO_HDF5_VERSION}")
    set(H5hut_WITH_MPI ON CACHE BOOL "H5hut parallel output" FORCE)
    set(H5hut_WITH_FORTRAN OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(h5hut GIT_REPOSITORY https://github.com/H5hut/H5hut.git
      GIT_TAG "${_revision}")
    FetchContent_MakeAvailable(h5hut)
  endif()
  add_library(H5hut::H5hut ALIAS H5hut)
  # Normalize upstream's unqualified link names for both build/install exports.
  set_property(TARGET H5hut PROPERTY LINK_LIBRARIES "${IPPL_IO_HDF5_TARGET};MPI::MPI_C")
  set_property(TARGET H5hut PROPERTY INTERFACE_LINK_LIBRARIES "${IPPL_IO_HDF5_TARGET};MPI::MPI_C")
  set(_package "${CMAKE_CURRENT_BINARY_DIR}/ippl-h5hut-package")
  file(MAKE_DIRECTORY "${_package}")
  get_target_property(_h5hutBinary H5hut BINARY_DIR)
  file(WRITE "${_package}/h5hut-targets.cmake"
    "include(\"${_h5hutBinary}/h5hut-targets.cmake\")\nif(NOT TARGET H5hut::H5hut)\n  add_library(H5hut::H5hut ALIAS H5hut)\nendif()\n")
  # Upstream's config omits dependency discovery. This adapter is also installed
  # after upstream's rules, and refers only to normal package/target names.
  file(WRITE "${_package}/H5hutConfig.cmake" [=[
include(CMakeFindDependencyMacro)
find_dependency(MPI COMPONENTS C)
set(HDF5_PREFER_PARALLEL TRUE)
find_dependency(HDF5 COMPONENTS C)
if(NOT HDF5_IS_PARALLEL)
  set(H5hut_FOUND FALSE)
  set(H5hut_NOT_FOUND_MESSAGE "H5hut requires parallel HDF5")
  return()
endif()
if(NOT TARGET H5hut::H5hut)
  include("${CMAKE_CURRENT_LIST_DIR}/h5hut-targets.cmake")
endif()
]=])
  get_directory_property(_h5hutVersion DIRECTORY "${_h5hutBinary}" DEFINITION PROJECT_VERSION)
  write_basic_package_version_file("${_package}/H5hutConfigVersion.cmake"
    VERSION "${_h5hutVersion}" COMPATIBILITY SameMajorVersion)
  # Provide the standard package directory; upstream installs directly in lib/cmake,
  # which normal prefix-based find_package searches do not discover reliably.
  install(EXPORT h5hut-targets FILE h5hut-targets.cmake NAMESPACE H5hut::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/H5hut")
  install(FILES "${_package}/H5hutConfig.cmake" "${_package}/H5hutConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/H5hut")
  set(IPPL_H5HUT_FETCHED TRUE PARENT_SCOPE)
  set(IPPL_H5HUT_PACKAGE_HINT "${_package}" PARENT_SCOPE)
endfunction()
