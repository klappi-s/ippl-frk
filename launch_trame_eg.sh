# trame needs to be installed in the pvpython executable or used with environment
PV_PREFIX=/home/klappi/AddApp/paraview_versions/ParaView-5.13.2-MPI-Linux-Python3.10-x86_64
PVPYTHON=${PV_PREFIX}/bin/pvpython
CATALYST_FOLDER=${PWD}/src/Stream/InSitu/catalyst_scripts/
$PVPYTHON ${CATALYST_FOLDER}/trame_vis_app.py --server
