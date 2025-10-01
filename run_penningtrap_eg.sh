#!/bin/bash

export PV_PREFIX=/.../ParaView-5.12.0-MPI-Linux-Python3.10-x86_64
export IPPL_DIR=/.../ippl


export CATALYST_IMPLEMENTATION_PATHS="${PV_PREFIX}/lib/catalyst"
export CATALYST_IMPLEMENTATION_NAME="paraview"


# Ascent Adaptor will try fetch from environment, else swap to default
export ASCENT_ACTIONS_PATH=${IPPL_DIR}/src/Stream/InSitu/ascent_scripts/ascent_actions_default.yaml
# Catalyst Adaptor will fetch from environment, else swap to default
export PENNINGTRAP_BINDIR=${IPPL_DIR}/build/alpine
export CATALYST_PIPELINE_PATH=${IPPL_DIR}/src/Stream/InSitu/catalyst_scripts/pipeline_default.py
export CATALYST_PROXY_PATH=${IPPL_DIR}/src/Stream/InSitu/catalyst_scripts/proxy_default.xml
# Add default catalyst extractors...




cd ${PENNINGTRAP_BINDIR}
rm -rd data
mkdir data


# ./PenningTrap 4 4 4 512 21 FFT 0.05 LeapFrog --overallocate 1.0  --info 5
./PenningTrap 8 8 8 4096 21 FFT 0.05 LeapFrog --overallocate 1.0  --info 5
cd $IPPL_DIR







