#!/usr/bin/env bash
#
# ipplADIOS run script: AlpineSight with the AdiosCatalyst -> ADIOS2 (BP5) backend.
#
# The simulation publishes the usual Catalyst Conduit channels; at runtime we load the
# AdiosCatalyst implementation (CATALYST_IMPLEMENTATION_NAME=adios) instead of ParaView, and
# ADIOS2 writes a BP5 time-series to disk. No ParaView / Python LD_PRELOAD tricks are needed.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build}"
export ALPINE_BINDIR="${BUILD_DIR}/demos/alpine"
export FEL_BINDIR="${BUILD_DIR}/demos/fel"

# ---------------------------------------------------------------------------
# MPI: must match the toolchain ippl / ADIOS2 were built with. The binary links
# /usr/lib64/mpich/lib/libmpi.so.12, so use MPICH's mpiexec (NOT ParaView's).
# ---------------------------------------------------------------------------
export MPIEXEC="${MPIEXEC:-/usr/lib64/mpich/bin/mpiexec}"
if [[ -d /usr/lib64/mpich/bin ]]; then
    export PATH="/usr/lib64/mpich/bin:${PATH}"
fi

# ---------------------------------------------------------------------------
# AdiosCatalyst implementation (Catalyst 2). Prefer the in-tree build
# (-DIPPL_ENABLE_ADIOS2=ON); fall back to an external install.
# ---------------------------------------------------------------------------
IPPL_BUILD_ABS="$(cd "${BUILD_DIR}" && pwd)"
ADIOSCATALYST_EXTERNAL="${ADIOSCATALYST_EXTERNAL:-/home/klappi/Install/adioscatalyst/install}"

if [[ -f "${IPPL_BUILD_ABS}/lib64/catalyst/libcatalyst-adios.so" ]]; then
    export CATALYST_IMPLEMENTATION_PATHS="${IPPL_BUILD_ABS}/lib64/catalyst"
elif [[ -f "${ADIOSCATALYST_EXTERNAL}/lib64/catalyst/libcatalyst-adios.so" ]]; then
    export CATALYST_IMPLEMENTATION_PATHS="${ADIOSCATALYST_EXTERNAL}/lib64/catalyst"
else
    echo "run.sh: libcatalyst-adios.so not found; build with -DIPPL_ENABLE_ADIOS2=ON" >&2
    exit 1
fi
export CATALYST_IMPLEMENTATION_NAME=adios

# Fedora's catalyst-mpich package installs libcatalyst.so.3 into MPI's library
# directory, which sits on LD_LIBRARY_PATH and would outrank the binary's RUNPATH.
# Put the in-tree Catalyst (and ADIOS2) first so the loader we built against wins.
export LD_LIBRARY_PATH="${IPPL_BUILD_ABS}/lib64:${IPPL_BUILD_ABS}/_deps/adios2-build/lib64:${LD_LIBRARY_PATH:-}"

# No ParaView Catalyst / embedded-Python preloads for pure ADIOS disk output.
unset LD_PRELOAD || true

# ---------------------------------------------------------------------------
# ippl Catalyst backend selection (see CatalystAdaptor)
# ---------------------------------------------------------------------------
export IPPL_CATALYST_BACKEND=ADIOS
export IPPL_ADIOS2_XML="${IPPL_ADIOS2_XML:-${SCRIPT_DIR}/adios2.xml}"
export IPPL_ADIOS2_OUTPUT="${IPPL_ADIOS2_OUTPUT:-alpine.bp}"

# Visualization channels stay on (that is the data ADIOS2 writes); the ParaView-only
# live/steer/extract paths are not used by AdiosCatalyst.
export IPPL_CATALYST_VIS=ON
export IPPL_CATALYST_LIVE=OFF
export IPPL_CATALYST_STEER=OFF
export IPPL_CATALYST_PNG=OFF
export IPPL_CATALYST_VTK=OFF

# Conduit Relay HDF5 is the separate ipplIO track; keep it off here.
export IPPL_CONDUIT_HDF5=OFF

# Optional verbosity for the adaptor (0..5)
# export IPPL_CATALYST_VERBOSITY=4

# ===========================================================================
#  ParaView Catalyst backend (reference — not used for ADIOS runs)
# ===========================================================================
# export PV_PREFIX="/home/klappi/Install/kitware/ParaView-6.1.1-MPI-Linux-Python3.12-x86_64"
# export MPIEXEC=${PV_PREFIX}/lib/mpiexec
# export CATALYST_IMPLEMENTATION_PATHS="${PV_PREFIX}/lib/catalyst"
# export CATALYST_IMPLEMENTATION_NAME="paraview"
# export LD_PRELOAD="${PV_PREFIX}/lib/libcatalyst.so.3:${PV_PREFIX}/lib/libpython3.12.so.1.0"
# unset IPPL_CATALYST_BACKEND

# ===========================================================================
#  Run
# ===========================================================================
cd "${ALPINE_BINDIR}"

# Fresh output
rm -rf data "${IPPL_ADIOS2_OUTPUT}"
mkdir -p data

echo "run.sh: backend=${IPPL_CATALYST_BACKEND} impl=${CATALYST_IMPLEMENTATION_NAME}"
echo "run.sh: CATALYST_IMPLEMENTATION_PATHS=${CATALYST_IMPLEMENTATION_PATHS}"
echo "run.sh: IPPL_ADIOS2_XML=${IPPL_ADIOS2_XML}"
echo "run.sh: IPPL_ADIOS2_OUTPUT=${ALPINE_BINDIR}/${IPPL_ADIOS2_OUTPUT}"

$MPIEXEC -np 2 ./AlpineSight 8 8 8 4096 22 FFT 0.05 LeapFrog --overallocate 1.0 --info 4

echo
echo "run.sh: done. Inspect BP5 output with:"
echo "  bpls -l ${ALPINE_BINDIR}/${IPPL_ADIOS2_OUTPUT}"
echo "  bpls -D ${ALPINE_BINDIR}/${IPPL_ADIOS2_OUTPUT}"
