## when multiple mpi version installed guarantee correct used version like:
export PV_PREFIX="/home/klappi/Install/kitware/ParaView-6.1.1-MPI-Linux-Python3.12-x86_64"
export MPIEXEC=${PV_PREFIX}/lib/mpiexec

INSTALL_BASE="${INSTALL_BASE:-/home/klappi/Install}"



# Resolve CMake package dirs from install prefixes (Fedora uses lib64/).
_ippl_cmake_pkg_dir() {
  local prefix="$1"
  shift
  local sub
  for sub in "$@"; do
    if [[ -f "${prefix}/${sub}" ]]; then
      dirname "${prefix}/${sub}"
      return 0
    fi
  done
  echo "${prefix}"
}

# ADIOS2_CMAKE_DIR="$(_ippl_cmake_pkg_dir "${INSTALL_BASE}/adios2/install" \
#   lib64/cmake/adios2/adios2-config.cmake lib/cmake/adios2/adios2-config.cmake)"
# CATALYST_CMAKE_DIR="$(ls -d "${INSTALL_BASE}/catalyst/install"/lib64/cmake/catalyst-* \
#   "${INSTALL_BASE}/catalyst/install"/lib/cmake/catalyst-* 2>/dev/null | head -1)"
# KOKKOS_CMAKE_DIR="$(_ippl_cmake_pkg_dir "${INSTALL_BASE}/kokkos/install" \
#   lib64/cmake/Kokkos/KokkosConfig.cmake lib/cmake/Kokkos/KokkosConfig.cmake)"
# HEFFTE_CMAKE_DIR="$(_ippl_cmake_pkg_dir "${INSTALL_BASE}/heffte/install" \
#   lib64/cmake/Heffte/HeffteConfig.cmake lib/cmake/Heffte/HeffteConfig.cmake)"

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_CXX_STANDARD=20
  -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON
  -DMPIEXEC_EXECUTABLE="${MPIEXEC}"
  -DIPPL_PLATFORMS=SERIAL
  -DIPPL_ENABLE_FFT=ON
  -DIPPL_ENABLE_SOLVERS=ON
  -DIPPL_ENABLE_ALPINE=ON
  -DIPPL_ENABLE_TESTS=OFF
  -DIPPL_ENABLE_UNIT_TESTS=OFF
  -DIPPL_ENABLE_CONDUIT_IO=OFF
  -DIPPL_ENABLE_CATALYST=ON
  -DIPPL_ENABLE_ADIOS2=ON
  # -DKokkos_DIR="${KOKKOS_CMAKE_DIR}"
  # -DHeffte_DIR="${HEFFTE_CMAKE_DIR}"
  # -DADIOS2_DIR="${ADIOS2_CMAKE_DIR}"
  # -DAdiosCatalyst_DIR="${INSTALL_BASE}/adioscatalyst/install"
  # -Dcatalyst_DIR="${CATALYST_CMAKE_DIR}"
)



# Fresh build
rm -rf build
mkdir build
cd build
cmake "${CMAKE_ARGS[@]}" ../ippl
# cmake --build . -j 8
cd ..
