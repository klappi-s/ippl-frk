# script-version: 2.0
# for more details check https://www.paraview.org/paraview-docs/latest/cxx/CatalystPythonScriptsV2.html


# Add this block to the top of your file
import sys
import time
import os
sys.path.append(os.path.dirname(__file__))

import paraview
from paraview import print_info
from paraview import catalyst
from paraview.simple import *
from paraview.simple import LoadPlugin, CreateSteerableParameters, PVTrivialProducer, FindSource
import paraview.simple as pvs
# from paraview.simple import GetActive


from paraview import servermanager as sm
from paraview import servermanager

# Import Catalyst utility subroutines
from catalystSubroutines import (
    print_proxy_overview,
    create_VTPD_extractor
)



#### disable automatic camera rest on 'Show'
paraview.simple._DisableFirstRenderCameraReset()


# print start marker
print("====================================>")
print("===EXECUTING CATALYST PIPELINE======>")
print("====================================>")
print_info("\nstart'%s'\n", __name__)





print("0===CREATING STEERABLES======0")
try:
    steerable_parameters = CreateSteerableParameters("SteerableParameters")
    # steerable_parameters = CreateSteerableParameters("STEERING_TYPE", "SteerableParameters")
    # steering_parameters = servermanager.ProxyManager().GetProxy("sources", "SteeringParameters")
    if steerable_parameters is None:
        print("Error: SteerableParameters proxy not found (CreateSteerableParameters returned None).")
    else:
                    print("SteerableParameters loaded successfully.")

                    # # --- Set initial value from incoming channel if available ---
                    # try:
                    #     # Access the incoming 'steerable' channel (PVTrivialProducer)
                    #     steerable_field_in = PVTrivialProducer(registrationName='steerable')
                    #     steerable_field_in.UpdatePipeline()
                    #     output = steerable_field_in.GetClientSideObject().GetOutput()
                    #     partition = output.GetPartition(0)
                    #     # partition = output.GetBlock(0)
                    #     # partition = output  # fallback, may not work
                        
                    #     if partition is not None and hasattr(partition, "GetPointData"):
                    #         data_info = partition.GetPointData()
                    #         if data_info is not None and data_info.GetNumberOfArrays() > 0:
                    #             # Find the 'steerable' array
                    #             for i in range(data_info.GetNumberOfArrays()):
                    #                 arr = data_info.GetArray(i)
                    #                 if arr.GetName() == "steerable":
                    #                     initial_value = arr.GetTuple1(0)
                    #                     # steerable_parameters.scaleFactor =
                    #                     print([initial_value])
                    #                     break
                    #     else:
                    #         print("Could not find a valid partition with point data for steerable channel.")
                    # except Exception as e:
                    #     print(f"Could not set initial steerable value from simulation: {e}")

except Exception as e:
    print(f"Exception while loading SteerableParameters: {e}")
print("1===CREATING STEERABLES======1")

print_proxy_overview()



# ----------------------------------------------------------------------
# -------------EXTRACTORS-----------------------------------------------
# ----------------------------------------------------------------------

print("0===SETTING TrivialProducers (Live)=======0")
# registrationName must match the channel name used in the 'CatalystAdaptor'.
ippl_field_v         = PVTrivialProducer(registrationName='ippl_E')
ippl_field_s         = PVTrivialProducer(registrationName='ippl_scalar')
ippl_particle_       = PVTrivialProducer(registrationName='ippl_particle')


steerable_field_in = PVTrivialProducer(registrationName='steerable')
print("1===SETTING TrivialProducers (Live)=======1")


print("0===SETTING DATA EXTRAXCTION=======0")
# """ this breaks live visualisation """
vTPD_particle = create_VTPD_extractor("particle", ippl_particle_)
vTPD_field_v  = create_VTPD_extractor("field_v",  ippl_field_v)
vTPD_field_s  = create_VTPD_extractor("field_s",  ippl_field_s)
print("1===SETTING DATA EXTRACTION=======1")







""" THIS IS DONE FROM CPP """
# print("===CREATING PNG EXTRACTORS==============")
            # these will directly create extractors without further use
            # these files are catalyst save files directly created from paraview -...
            # from catalyst_extractors import png_ext_particle
            # from catalyst_extractors import png_ext_sfield
            # from catalyst_extractors import png_ext_vfield
            # from catalyst_extractors.png_ext_particle import ippl_particle
            # from catalyst_extractors.png_ext_sfield   import ippl_scalar
            # from catalyst_extractors.png_ext_vfield   import ippl_E
            # from catalyst_extractors.png_ext_particle import renderView1
            # from catalyst_extractors.png_ext_sfield   import renderView1
            # from catalyst_extractors.png_ext_vfield   import renderView1
# print("===CREATING PNG EXTRACTORS==============DONE")




# print("===CHECKING IPPL DATA===================")
# # Add detailed data type checking
# field_output    = ippl_field_v.GetClientSideObject().GetOutput()
# scalar_output   = ippl_field_s.GetClientSideObject().GetOutput()
# particle_output = ippl_particle_.GetClientSideObject().GetOutput()
# particle_info   = ippl_particle_.GetDataInformation()
# field_info      = ippl_field_v.GetDataInformation()
# scalar_info     = ippl_field_s.GetDataInformation()
# # Debug: Check data availability (can be done at each cycle...)
# print(f"Data types:")
# print(f"  - ippl_field_v       : {type(field_output).__name__}")
# print(f"                       {field_info.GetNumberOfPoints()} points, {field_info.GetNumberOfCells()} cells")
# print(f"  - ippl_field_s:      {type(scalar_output).__name__}")
# print(f"                       {scalar_info.GetNumberOfPoints()} points, {scalar_info.GetNumberOfCells()} cells") 
# print(f"  - ippl_particle    : {type(particle_output).__name__}")
# print(f"                       {particle_info.GetNumberOfPoints()} points, {particle_info.GetNumberOfCells()} cells")
# print("===CHECKING IPPL DATA===================DONE")



# ------------------------------------------------------------------------------
# ------------------------------------------------------------------------------
# ------------------------------------------------------------------------------



print("0===SETING CATALYST OPTIONS for VTK extracts================0")
options = catalyst.Options()
options.GlobalTrigger = 'Time Step'
options.EnableCatalystLive = 1
options.CatalystLiveTrigger = 'Time Step'
options.ExtractsOutputDirectory = 'data_vtk_extracts'
print("1===SETING CATALYST OPTIONS for VTK extracts==================1")







# ------------------------------------------------------------------------------
# ------------------------------------------------------------------------------
# ------------------------------------------------------------------------------
print("0===DEFINING CATALYST_ init, exe, fini======================0")
def catalyst_initialize():
    print_info("in '%s::catalyst_initialize'", __name__)
    print("===CALLING catalyst_initialize()====>")

    print("===CALLING catalyst_initialize()====>DONE")



# ------------------------------------------------------------------------------
# ------------------------------------------------------------------------------



def catalyst_execute(info):
    # print_info("in '%s::catalyst_execute'", __name__)
    print(f"---Cycle {info.cycle}:----catalyst_execute()---------------START")
    print("executing (cycle={}, time={})".format(info.cycle, info.time))

    

    # import from other file has eerors ..
    # global ippl_particle
    # global ippl_scalar
    # global ippl_E    
    # ippl_particle_.UpdatePipeline()
    # ippl_scalar.UpdatePipeline()
    # ippl_E.UpdatePipeline()





    global ippl_particle_
    global ippl_field_s
    global ippl_field_v
    
    ippl_field_v.UpdatePipeline()
    ippl_field_s.UpdatePipeline()
    ippl_particle_.UpdatePipeline()
    # print("field bounds   :", ippl_field_v.GetDataInformation().GetBounds())
    # print("field bounds   :", ippl_field_s.GetDataInformation().GetBounds())
    # print("particle bounds:", ippl_particle.GetDataInformation().GetBounds())




    global steerable_parameters
    global steerable_field_in
    steerable_field_in.UpdatePipeline()



    # steer=servermanager.Fetch(FindSource("steerable")).GetPartition(0,0)
    # steer_array = steer.GetPointData().GetArray("steerable")
    # # steer_array = steer.GetFieldData().GetArray("steerable")
    # steer_atm = 1
    # if steer_array:
    #       steer_atm = steer_array.GetTuple1(0)

    # steerable_parameters.scaleFactor[0] = steer_atm





    #  works...
    if steerable_parameters is None:
        print("Error: SteerableParameters proxy not found (CreateSteerableParameters returned None).")
    else:
                    # --- Set initial value from incoming channel if available ---
                    try:
                        # Access the incoming 'steerable' channel (PVTrivialProducer)
                        steerable_field_in.UpdatePipeline()
                        output = steerable_field_in.GetClientSideObject().GetOutput()
                        partition = output.GetPartition(0)
                        # partition = output.GetBlock(0)
                        # partition = output  # fallback, may not work
                        
                        if partition is not None and hasattr(partition, "GetPointData"):
                            data_info = partition.GetPointData()
                            # data_info = partition.GetFieldData()
                            if data_info is not None and data_info.GetNumberOfArrays() > 0:
                                    initial_value= data_info.GetArray("steerable").GetTuple1(0)
                                    steerable_parameters.scaleFactor[0] =  initial_value
                        else:
                            print("Could not find a valid partition with point data for steerable channel.")
                    except Exception as e:
                        print(f"Could not set initial steerable value from simulation: {e}")



    
    # steerable_parameters.scaleFactor[0] = 31 + info.cycle
    print(f"SteerableParameter: {steerable_parameters.scaleFactor[0]}")


    


    if options.EnableCatalystLive:
        time.sleep(0.2)

    print(f"---Cycle {info.cycle}:----catalyst_execute()---------------DONE")

# ------------------------------------------------------------------------------
# ------------------------------------------------------------------------------



def catalyst_finalize():
    print_info("in '%s::catalyst_finalize'", __name__)
    print("==================================|")
    print("===CALLING catalyst_finalize()====|")


    
    print("===CALLING catalyst_finalize()====|DONE")
    print("==================================|")

print("1===DEFINING CATALYST_ init, exe, fini=====================")
# ------------------------------------------------------------------------------
# ------------------------------------------------------------------------------
# ------------------------------------------------------------------------------



print("\n\n")
print_info("end '%s'", __name__)
print("====================================|")
print("===END OF CATALYST PIPELINE=========|")
print("====================================|")
print("\n\n")
# print end marker



""" even if we set up the default this value will not be returned druing the first 2 steps ...
because i guess on initilialisation is set to 0....so we will pass back 0 and cpp will pick up on it ...

but then 2steps latert the default seems to be rememebered?

I guess when catalyst  live connection is established the initial value for the steering proxy is also passed on,
 and set inside the paraview client but this takes a few steps to set up, after everything is in order
  the backward channel will always be overwritten from he last registered entry inside the GUI ....
  But these first 2 steps with a zero scaleFactor are bad
  further if no client is open then0 will always be passed back to the model,
  which we definitely want to avoid



LeapFrogStep> currently scaleFactor set as:30
scatter > 1.45519e-16
====================================>
===EXECUTING CATALYST PIPELINE======>
====================================>
(   1.741s) [pvbatch         ]    pipeline_default.py:39    INFO| 
start'pipeline_default'

0===CREATING STEERABLES======0
SteerableParameters loaded successfully.
1===CREATING STEERABLES======1
0====Printing Proxy Overview  ===========0
Available 'sources' proxies:
 - ProxyPrint: <paraview.servermanager.SteerableParameters object at 0x7efecb3ee0e0>)
 - Proxy Name: SteeringParameters
   - XML Label: Steerable Parameters
   - XML Group: sources
   - Class Name: SteerableParameters
   - Properties:
     - FieldAssociation
     - PartitionType
     - scaleFactor
 - ProxyPrint: <paraview.servermanager.Conduit object at 0x7efec96a5f00>)
 - Proxy Name: ippl_E
   - XML Label: Conduit
   - XML Group: sources
   - Class Name: Conduit
   - Properties:
     - TimestepValues
 - ProxyPrint: <paraview.servermanager.Conduit object at 0x7efec96a61d0>)
 - Proxy Name: ippl_particle
   - XML Label: Conduit
   - XML Group: sources
   - Class Name: Conduit
   - Properties:
     - TimestepValues
 - ProxyPrint: <paraview.servermanager.Conduit object at 0x7efec96a61a0>)
 - Proxy Name: ippl_scalar
   - XML Label: Conduit
   - XML Group: sources
   - Class Name: Conduit
   - Properties:
     - TimestepValues
 - ProxyPrint: <paraview.servermanager.Conduit object at 0x7efec96a6200>)
 - Proxy Name: steerable
   - XML Label: Conduit
   - XML Group: sources
   - Class Name: Conduit
   - Properties:
     - TimestepValues
1===Printing Proxy Overview==============1
0===SETTING TrivialProducers (Live)=======0
1===SETTING TrivialProducers (Live)=======1
0===SETTING DATA EXTRAXCTION=======0
1===SETTING DATA EXTRACTION=======1
0===SETING CATALYST OPTIONS for VTK extracts================0
1===SETING CATALYST OPTIONS for VTK extracts==================1
0===DEFINING CATALYST_ init, exe, fini======================0
1===DEFINING CATALYST_ init, exe, fini=====================



(   1.775s) [pvbatch         ]    pipeline_default.py:285   INFO| end 'pipeline_default'
====================================|
===END OF CATALYST PIPELINE=========|
====================================|



(   1.775s) [pvbatch         ]    pipeline_default.py:177   INFO| in 'pipeline_default::catalyst_initialize'
===CALLING catalyst_initialize()====>
===CALLING catalyst_initialize()====>DONE
---Cycle 0:----catalyst_execute()---------------START
executing (cycle=0, time=0.0)
SteerableParameter: 30.0
---Cycle 0:----catalyst_execute()---------------DONE
====================================>
===EXECUTING CATALYST SFIELD EXTRACTOR======>
====================================>
====================================>
===EXECUTING CATALYSt VFIELD EXTRACTOR======>
====================================>
====================================>
===EXECUTING CATALYSt PARTICLES EXTRACTOR======>
====================================>
Result Node dump:
Post-step:> Finished time step: 1 time: 0.05
Pre-step> Done
LeapFrogStep> currently scaleFactor set as:0
scatter > 5.82077e-16
---Cycle 1:----catalyst_execute()---------------START
executing (cycle=1, time=0.05)
SteerableParameter: 0.0
---Cycle 1:----catalyst_execute()---------------DONE
Result Node dump:
Post-step:> Finished time step: 2 time: 0.1
Pre-step> Done
LeapFrogStep> currently scaleFactor set as:0
scatter > 1.45519e-16
---Cycle 2:----catalyst_execute()---------------START
executing (cycle=2, time=0.1)
SteerableParameter: 0.0
---Cycle 2:----catalyst_execute()---------------DONE
Result Node dump:
Post-step:> Finished time step: 3 time: 0.15
Pre-step> Done
LeapFrogStep> currently scaleFactor set as:30
scatter > 4.36557e-16
---Cycle 3:----catalyst_execute()---------------START
executing (cycle=3, time=0.15000000000000002)
SteerableParameter: 30.0
---Cycle 3:----catalyst_execute()---------------DONE
Result Node dump:
Post-step:> Finished time step: 4 time: 0.2
Pre-step> Done
LeapFrogStep> currently scaleFactor set as:30
scatter > 2.91038e-16
 """