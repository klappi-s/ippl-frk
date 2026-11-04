# openPMD dependency setup, called from Dependencies.cmake when I/O is enabled.
# Installed packages are preferred; missing packages or git revisions use FetchContent.
# Function scope keeps temporary variables and non-cache options out of the caller.

include(ADIOS2Dependency)
include(HDF5Dependency)

# ------------------------------------------------------------------------------
# openPMD: validate backend selection, then find or build the matching package.
# Return package hints/components for IPPL's build-tree and installed exports.
# ------------------------------------------------------------------------------
function(ippl_setup_openpmd)
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
    set(IPPL_IO_HDF5_FETCHED "${IPPL_IO_HDF5_FETCHED}" PARENT_SCOPE)
    set(IPPL_IO_HDF5_PACKAGE_HINT "${IPPL_IO_HDF5_PACKAGE_HINT}" PARENT_SCOPE)
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
