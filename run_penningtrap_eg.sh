#!/bin/bash

export IPPL_DIR=${PWD}
export PENNINGTRAP_BINDIR=./build/alpine

# #####################################################################
#  (Important) CONFIGURE PARAVIEW CATALYST VERSION
# #####################################################################
# automatically set on juelich clusters
PV_PREFIX="/.../ParaView-5.XX.X-MPI-Linux-Python3.10-x86_64"

export CATALYST_IMPLEMENTATION_PATHS="${PV_PREFIX}/lib/catalyst"
export CATALYST_IMPLEMENTATION_NAME="paraview"


# #####################################################################
#  (Optional) CONFIGURE CATALYST OPTIONS
# #####################################################################
# any invalid input will switch to default case, check output during initialisation for parsed settings
# TODO overwrite proxy path:

# does nothing atm
# "ON":
#  any/"OFF": (default)
export IPPL_CATALYST_VIS=ON

# "ON":
#  any/"OFF": (default)
export IPPL_CATALYST_STEER=ON


# "ON":
#  any/"OFF": (default)
export IPPL_CATALYST_PNG=OFF

# "ON":
#  any/"OFF": (default)
export IPPL_CATALYST_VTK=OFF

# any/"ON":(default)    writes catalyst_scripts/catalyst_proxy.xml and continues simulation
# "PRODUCE_ONLY":       writes and throws exception
# "OFF":                doesn't write but (still tries to access old catalyst_scripts/catalyst_proxy.xml 
#                       if CATALYST_PROXY_PATH is not set) and runs simulation
export IPPL_CATALYST_PROXY_OPTION=ON

# "ON":                 <-> masking GHOST_MASKS <-> not finally tested 
#  any/"OFF": (default) <-> cutting GHOST_MASKS <->  works
export IPPL_CATALYST_GHOST_MASKS=OFF




# #####################################################################
# Catalyst Adaptor will try to fetch paths via these environment variables
#  else switch to # harcoded preconfigured defaults inside IPPL src directory
#  (helped via cmake environment options)
# #####################################################################

# overwrites catalyst main script/pipeline (for Live vis and vtk file extractor):
# export CATALYST_PIPELINE_PATH=${IPPL_DIR}/src/Stream/InSitu/catalyst_scripts/pipeline_default.py

# overwrites catalyst script (png extraction) for arbitrary vis channels
# export CATALYST_EXTRACTOR_SCRIPT_<registry_label>

# overwrite steering proxies completeley by referencing different file:
# export CATALYST_PROXYS_PATH = 

# change default ranges for stering channels:
# export IPPL_PROXY_CONFIG_YAML





cd ${PENNINGTRAP_BINDIR}

rm -rd data
mkdir data

#####################################################################################
# when running with MPI this might be needed to guarantee compatibility (openMPI vs MPIch)
# export MPIEXEC=$PV_PREFIX/lib/mpiexec
# exec $MPIEXEC -np 1 ...
#####################################################################################
# slurm:
# srun  .... 
# ###################################################################################


./AlpineSight 8 8 8 4096 20 FFT 0.05 LeapFrog --overallocate 1.0  --info 5



cd $IPPL_DIR