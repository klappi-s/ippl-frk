"""! \file png_ext_sfield.py
\brief Catalyst PNG extractor for 3D scalar fields (e.g., ippl::Field<T,3>).
\details Performs volume rendering with adaptive camera and transfer functions.
Relies on pipeline_default.py for pipeline updates and can run with Catalyst Live.
"""

# script-version: 2.0
# Catalyst state generated using paraview version 5.12.0


########################################################
######################################################## 
# PNG extractor script for paraview catalyst. 
# Visualizes 3D vector fields. eg:
# ippl::field<double,3> 
# ippl::field<float,3> 
# 
# Currently hard coded to rely on attributes:
# - 'position'
# - label
# Is adaptive: Attempts to set Camera Angle and colouring 
# of paraviews Volume Rendering  (dependent on scalar
# field values) adaptive to current frame, range and 
# scale (every 10'th step).
# 
# Relies on pipeline_default.py to update pipeline else might
# cause errors (i think)
# 
# 
# Possible TODO:
#  - Customize extraction frequency
#  - Customize "rescale" frequency
#  - More
# 
# 
########################################################
########################################################


import paraview
from paraview.simple import *
# paraview.compatibility.major = 5
# paraview.compatibility.minor = 12
#### import the simple module from the paraview
from paraview.simple import *
from paraview.simple import (
    PVTrivialProducer,
    CreateView,
    GetMaterialLibrary,
    Show,
    GetTransferFunction2D,
    GetColorTransferFunction,
    GetOpacityTransferFunction,
    GetScalarBar,
    CreateExtractor,
    SetActiveView,
    SetActiveSource
)
from paraview import print_info
import argparse
import math

from paraview import print_info

from paraview import servermanager as sm
from vtkmodules.vtkParallelCore import vtkCommunicator, vtkMultiProcessController
# ----------------------------------------------------------------
# helpers used for adaptive visualization
# ----------------------------------------------------------------
from catalystSubroutines import (
    nice_bounds_sym,
    auto_camera_from_bounds,
    get_global_spatial_bounds,
    get_global_range,
    # print_info_,
)

def print_info_(s, level=0):
    global verbosity
    if verbosity>level:
        print_info(s)
# ----------------------------------------------------------------
# ----------------------------------------------------------------
paraview.simple._DisableFirstRenderCameraReset()
SetActiveView(None)
# ----------------------------------------------------------------
# Parse arguments received via conduit node
# ----------------------------------------------------------------
arg_list = paraview.catalyst.get_args()
# print_info_(f"Arguments received: {arg_list}")
parser = argparse.ArgumentParser()
parser.add_argument("--label", default="AAAAAA", help="Needed to correctly setup association between script name and conduti channel.")
parser.add_argument("--channel_name", default="DEFAULT_CHANNEL", help="Needed to correctly setup association between script name and conduti channel.")
parser.add_argument("--experiment_name", default="_", help="Needed to correctly for safe folder.")
parser.add_argument("--verbosity", type=int, default="1", help="Communicate the catalyst Output Level from the simulation")
parsed = parser.parse_args(arg_list)

label = parsed.label
exp_chann = parsed.channel_name
exp_string = parsed.experiment_name
verbosity = parsed.verbosity
print_info_("_global__scope__()::" + parsed.channel_name)
# ----------------------------------------------------------------
# create a new 'XML Partitioned Dataset Reader'
# ----------------------------------------------------------------
cname = parsed.channel_name
ippl_scalar_p = PVTrivialProducer(registrationName = cname)
associate = "CELLS"

dinfo = ippl_scalar_p.GetDataInformation()
cinfo = dinfo.GetCellDataInformation()
ghost_info = cinfo.GetArrayInformation("vtkGhostType") if cinfo else None


ippl_scalar = ippl_scalar_p

# # === FIX START ===
# # Filter to strip ghost arrays so the "cut out" stripe is rendered.
# # Check if the filter exists in your version, otherwise use RenameArrays.
# try:
#     ippl_scalar = RemoveGhostInformation(registrationName='NoGhosts', Input=ippl_scalar_p)
# except NameError:
#     # Fallback if RemoveGhostInformation is not loaded by default
#     print("RemoveGhostInformation not found, passing original data.")
#     ippl_scalar = ippl_scalar_p
# # === FIX END ===

# ippl_scalar = GhostCells(Input=ippl_scalar_p)



# ippl_scalar_renamed = RenameArrays(registrationName='HideGhosts', Input=ippl_scalar_p)
# # Map 'vtkGhostType' (the standard name) to 'IgnoredGhosts' (junk name)# Rename Cell Data ghost array (Most likely the one you need)
# ippl_scalar_renamed.CellArrays = ['vtkGhostType', 'IgnoredGhosts']
# ippl_scalar = ippl_scalar_renamed



# ippl_scalar = Threshold(registrationName='Threshold1', Input=ippl_scalar_p)
# ippl_scalar.Scalars = ['CELLS', 'vtkGhostType']


# # create a new 'Threshold'
# ippl_scalar_t = Threshold(registrationName='Threshold1', Input=ippl_scalar_p)
# ippl_scalar_t.Scalars = ['CELLS', 'vtkGhostType']

# ippl_scalar_t.ComponentMode = 'Any'
# # 'Selected'
# # 'Any'
# # 'All'

# ippl_scalar_t.ThresholdMethod = 'Below Lower Threshold'
# # ippl_scalar_t.ThresholdMethod = 'Between'
# # ippl_scalar_t.ThresholdMethod = 'Above Upper Threshold'


# ippl_scalar_t.LowerThreshold = 0.1
# # ippl_scalar_t.UpperThreshold = 0.0

# ippl_scalar_t.AllScalars = 0


# # Set the method to keep only cells where GhostType is exactly 0
# ippl_scalar_t.ThresholdMethod = 'Between'
# ippl_scalar_t.LowerThreshold = 0.0
# ippl_scalar_t.UpperThreshold = 0.0

# ippl_scalar_t.AllScalars = 1
# """ ??? """



# if ghost_info:
#     ippl_scalar_t = Threshold(registrationName='KeepRealCells', Input=ippl_scalar_p)
#     ippl_scalar_t.Scalars = ['CELLS', 'vtkGhostType']
#     ippl_scalar_t.AllScalars = 0                 # only use selected array
#     ippl_scalar_t.ComponentMode = 'Selected'     # scalar, single component
#     ippl_scalar_t.SelectedComponent = 0
#     ippl_scalar_t.ThresholdMethod = 'Between'
#     ippl_scalar_t.LowerThreshold = 0.0
#     ippl_scalar_t.UpperThreshold = 0.0
#     ippl_scalar = ippl_scalar_t
# else:
#     print_info_("vtkGhostType not found; skipping ghost filtering", level=1)
#     ippl_scalar = ippl_scalar_p


# ippl_scalar = GhostCells(Input=ippl_scalar_p)



# ippl_scalar = CellDatatoPointData(registrationName=cname[12:]+'_Cell2Point', Input=ippl_scalar_p)
# associate = "POINTS"
# ----------------------------------------------------------------
# setup visualisation view for extraction pipeline in renderView1
# ----------------------------------------------------------------
renderView1 = CreateView('RenderView')
materialLibrary1 = GetMaterialLibrary()
renderView1.BackEnd = 'OSPRay raycaster'
renderView1.OSPRayMaterialLibrary = materialLibrary1
renderView1.ViewSize = [2000, 1500]
renderView1.StereoType = 'Crystal Eyes'
renderView1.LegendGrid = 'Legend Grid Actor'
renderView1.AxesGrid = 'Grid Axes 3D Actor'
renderView1.AxesGrid.Visibility = 1

renderView1.UseColorPaletteForBackground = 0
renderView1.BackgroundColorMode = 'Gradient'
# renderView1.Background2 = [0.0, 0.6666666666666666, 1.0]
# renderView1.Background = [0.0, 0.0, 0.4980392156862750]
SetActiveView(renderView1)
# ----------------------------------------------------------------
# Initial adaptive Camera set
# ----------------------------------------------------------------
scalar_info = ippl_scalar_p.GetDataInformation()

# scalar_info = ippl_scalar.GetDataInformation()
local_bounds = scalar_info.GetBounds()

global_bounds = get_global_spatial_bounds(local_bounds)
auto_camera_from_bounds(renderView1, global_bounds)
# ----------------------------------------------------------------
# choose Data to visualize and show in renderView1
# ----------------------------------------------------------------
ippl_scalarDisplay = Show(ippl_scalar, renderView1)
# ----------------------------------------------------------------
# setup initial transfer function for colouring and opacity
# ----------------------------------------------------------------
# get 2D transfer function for label
densityTF2D = GetTransferFunction2D(label)
densityTF2D.Range = [-2.00, 2.00, 0.0, 1.0]
densityTF2D.ScalarRangeInitialized = 1
# get color transfer function/color map for label
densityLUT = GetColorTransferFunction(label)

densityLUT.TransferFunction2D = densityTF2D


densityLUT.RGBPoints = [-2.00, 0.231373, 0.298039, 0.752941, 
                         0.00, 0.865003, 0.865003, 0.865003, 
                         2.00, 0.705882, 0.0156863, 0.14902]
densityLUT.ScalarRangeInitialized = 1.0
# get opacity transfer function/opacity map for label
densityPWF = GetOpacityTransferFunction(label)
densityPWF.Points = [-2.00, 1.00, 0.5, 0.0, 
                     -1.20, 0.75, 0.5, 0.0, 
                     -0.80, 0.25, 0.5, 0.0, 
                     -0.01, 0.00, 0.5, 0.0, 
                      0.00, 1.00, 0.5, 0.0, 
                      0.01, 0.00, 0.5, 0.0, 
                      0.80, 0.25, 0.5, 0.0, 
                      1.20, 0.75, 0.5, 0.0, 
                      2.00, 1.00, 0.5, 0.0]
densityPWF.ScalarRangeInitialized = 1
# ----------------------------------------------------------------
# Initial adaptive colouring of scale
# ----------------------------------------------------------------
# vmin, vmax = density_array_info.GetComponentRange(-1)
# nice_min, nice_max = nice_bounds_sym(vmin, vmax)
# densityLUT.RescaleTransferFunction(nice_min, nice_max)
# densityPWF.RescaleTransferFunction(nice_min, nice_max)
# ----------------------------------------------------------------
# configure displayed data
# ----------------------------------------------------------------
ippl_scalarDisplay.Representation = 'Volume'
ippl_scalarDisplay.LookupTable = densityLUT
ippl_scalarDisplay.OSPRayScaleFunction = 'Piecewise Function'
ippl_scalarDisplay.ScaleTransferFunction = 'Piecewise Function'
ippl_scalarDisplay.Assembly = 'Hierarchy'
ippl_scalarDisplay.ScaleFactor = 2.0
ippl_scalarDisplay.GaussianRadius = 0.1
ippl_scalarDisplay.DataAxesGrid = 'Grid Axes Representation'


ippl_scalarDisplay.TransferFunction2D = densityTF2D




ippl_scalarDisplay.ColorArrayName = [associate, label]
ippl_scalarDisplay.ColorArray2Name = [associate, label]
ippl_scalarDisplay.OpacityArrayName = [associate, label]
ippl_scalarDisplay.OpacityTransferFunction = 'Piecewise Function'
ippl_scalarDisplay.ScalarOpacityUnitDistance = 4.00
ippl_scalarDisplay.ScalarOpacityFunction = densityPWF

densityLUTColorBar = GetScalarBar(densityLUT, renderView1)
densityLUTColorBar.Title = label
densityLUTColorBar.ComponentTitle = 'Magnitude'
densityLUTColorBar.Visibility = 1
densityLUTColorBar.DrawAnnotations = 1 
densityLUT.EnableOpacityMapping = True
ippl_scalarDisplay.SetScalarBarVisibility(renderView1, True)
# ----------------------------------------------------------------
# setup extractors
# ----------------------------------------------------------------
pNG1 = CreateExtractor('PNG', renderView1, registrationName='PNG1')
pNG1.Trigger = 'Time Step'
pNG1.Trigger.Frequency = 1
pNG1.Writer.FileName = label+'_ScalarField_{timestep:06d}{camera}.png'
pNG1.Writer.ImageResolution = [2000, 1500]
pNG1.Writer.Format = 'PNG'
SetActiveSource(pNG1)
# ------------------------------------------------------------------------------
# Catalyst options
# ------------------------------------------------------------------------------
from paraview import catalyst
options = catalyst.Options()
options.GlobalTrigger = 'Time Step'
options.EnableCatalystLive = 1
options.CatalystLiveTrigger = 'Time Step'
options.ExtractsOutputDirectory = 'data_png_extracts_' + exp_string 
# ------------------------------------------------------------------------------
if __name__ == '__main__':
    from paraview.simple import SaveExtractsUsingCatalystOptions
    # Code for non in-situ environments; if executing in post-processing
    # i.e. non-Catalyst mode, let's generate extracts using Catalyst options
    SaveExtractsUsingCatalystOptions(options)



# ------------------------------------------------------------------------------
def catalyst_execute(info):
    # print_info_((parsed.channel_name+"::%s::catalyst_execute()")[0:50], __name__)
    print_info_("catalyst_execute()::"+parsed.channel_name)

    global ippl_scalar, ippl_scalar_p
    global densityLUT
    global densityPWF



    if info.cycle % 10 == 0:
    # if info.cycle % 10 + 1 == 10:
        # Get scalar field bounds
        scalar_info = ippl_scalar_p.GetDataInformation()
        local_bounds = scalar_info.GetBounds()
        cell_data_info = scalar_info.GetCellDataInformation()
        density_array_info = cell_data_info.GetArrayInformation(label)
        # print(bounds)
        # print(cell_data_info)


        local_vmin, local_vmax = density_array_info.GetComponentRange(-1)

        # --------------------------------------------------------
        # 2. PERFORM MPI REDUCTION (New Logic)
        # --------------------------------------------------------
        global_bounds = get_global_spatial_bounds(local_bounds)
        global_vmin, global_vmax = get_global_range(local_vmin, local_vmax)
        # --------------------------------------------------------

        nice_min, nice_max = nice_bounds_sym(global_vmin, global_vmax)


        # if for any reason this changes, but unlikely...
        # bounds for fields dont vary normally...
        # Adjust camera dynamically;
        auto_camera_from_bounds(renderView1, global_bounds)
        # Adjust grid bounds dynamically, should happen automaically..
        # renderView1.AxesGrid.UseCustomBounds = 1
        # renderView1.AxesGrid.CustomBounds = bounds
        

        # Update color and opacity transfer function
        # print_info_("==RESCALING COLOUR AND OPACITY BAR")
        densityLUT.RescaleTransferFunction(nice_min, nice_max)
        densityPWF.RescaleTransferFunction(nice_min, nice_max)

        # print_info_(f"Updated Opacity Map at cycle {info.cycle}: {densityPWF.Points}")