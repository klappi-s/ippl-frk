from paraview import simple
from catalystSubroutines import get_global_spatial_bounds

def setup_default_view(source_proxy, view):
    """
    Fallback for non-particle sources (e.g. Fields).
    """
    rep = simple.Show(source_proxy, view)
    
    # Auto-detect type
    info = source_proxy.GetDataInformation()
    if info.GetNumberOfCells() > 0:
        rep.SetRepresentationType('Surface')
    else:
        rep.SetRepresentationType('Points')
        
    simple.ResetCamera()
    return rep


def setup_particle_view(source_proxy, view):
    """
    Applies the 'Point Gaussian' style with a yellow bounding box helper.
    Replicates the logic from the PNG extractor script.
    """
    
    # 1. Clean existing representations for this source to start fresh
    #    (Optional, but good for re-initialization)
    existing_rep = simple.GetRepresentation(source_proxy, view)
    if existing_rep:
        simple.Delete(existing_rep)

    # 2. Extract the 'Main' particles (Block 0 usually)
    #    We create a separate filter so we can style it differently
    particles_extract = simple.ExtractBlock(Input=source_proxy, registrationName="Particles_Only")
    particles_extract.Selectors = ['//block_main'] 
    particles_extract.UpdatePipeline()

    # Check if extraction worked
    input_for_particles = particles_extract
    if particles_extract.GetDataInformation().GetNumberOfPoints() == 0:
        print("Warning: Particle extraction empty. Attempting MergeBlocks...")
        # Fallback: Merge everything into one dataset
        merged = simple.MergeBlocks(Input=source_proxy, registrationName="Merged_Particles")
        merged.MergePartitionsOnly = 1
        merged.UpdatePipeline()
        if merged.GetDataInformation().GetNumberOfPoints() > 0:
             input_for_particles = merged
        else:
             print("Warning: MergeBlocks also empty. Using raw source.")
             input_for_particles = source_proxy

    # 3. Extract the 'Helper/Box' (Block 1 usually)
    # box_extract = simple.ExtractBlock(Input=source_proxy, registrationName="Box_Helper")
    # box_extract.Selectors = ['//block_help']
    # box_extract.UpdatePipeline()

    # --- VISUALIZE PARTICLES ---
    # Show the particles using Point Gaussian
    part_rep = simple.Show(input_for_particles, view)
    part_rep.SetRepresentationType('Point Gaussian')
    
    # Configure Color (Velocity Magnitude)
    # We try to find the velocity array, otherwise default to solid color
    info = input_for_particles.GetDataInformation()
    point_data = info.GetPointDataInformation()
    print(dir(point_data))
    # Debug: Print available arrays
    num_arrays = point_data.GetNumberOfArrays()
    print(f"Available Point Arrays ({num_arrays}):")

    for i in range(num_arrays):
        # Use GetArrayInformation(index) instead of GetArray(index)
        array_info = point_data.GetArrayInformation(i)
        if array_info:
            print(f" - {array_info.GetName()}")
        else:
            print(f" - [Array {i} info is None]")

    if point_data.GetArrayInformation('velocity'):
        part_rep.ColorArrayName = ['POINTS', 'velocity']
        
        # Setup Lookup Table
        lut = simple.GetColorTransferFunction('velocity')
        # Reset range based on current data
        import math
        r = point_data.GetArrayInformation('velocity').GetComponentRange(-1) # -1 = Magnitude
        if r[1] > r[0]:
            lut.RescaleTransferFunction(r[0], r[1])
            
        part_rep.LookupTable = lut
        
        # Scalar Bar (Legend)
        sb = simple.GetScalarBar(lut, view)
        sb.Title = 'Velocity'
        sb.ComponentTitle = 'Magnitude'
        sb.Visibility = 1
        part_rep.SetScalarBarVisibility(view, True)
    else:
        print(" 'velocity' array not found, using default coloring.")

    # Calculate Gaussian Radius based on bounding box
    bounds = info.GetBounds()
    # Diagonal length of the bounding box
    dx = bounds[1] - bounds[0]
    dy = bounds[3] - bounds[2]
    dz = bounds[5] - bounds[4]
    
    import math
    # Check for valid bounds
    if dx < 0 or dy < 0 or dz < 0:
        diagonal = 0.0
    else:
        diagonal = math.hypot(dx, dy, dz)
    
    # Heuristic from your script: diagonal / 500
    if diagonal > 0:
        part_rep.GaussianRadius = diagonal / 500.0
    else:
        part_rep.GaussianRadius = 0.05

    # --- VISUALIZE BOX HELPER ---
    # Show the box as a yellow outline
    # box_rep = simple.Show(box_extract, view)
    # box_rep.SetRepresentationType('Outline')
    
    # # Solid Color: Yellow
    # box_rep.ColorArrayName = ['POINTS', ''] # Disable array coloring
    # box_rep.AmbientColor = [1.0, 1.0, 0.0]
    # box_rep.DiffuseColor = [1.0, 1.0, 0.0]
    # box_rep.LineWidth = 2.0
    # box_rep.Opacity = 0.5 # Slightly transparent

    simple.ResetCamera()

    return part_rep



def update_particle_view(source_proxy, view):
    """
    Called every frame to adapt color range and gaussian radius 
    if the data bounds/values change drastically.
    """
    # 1. Find the representation for the particles
    # (We assume the 'Particles_Only' ExtractBlock we created earlier)
    # We can find it by name or just check the view's representations
    
    # Quick hack: Iterate reps to find the one with Gaussian Radius
    reps = view.Representations
    part_rep = None
    for r in reps:
        if hasattr(r, 'GaussianRadius'): # It's a PointGaussian representation
            part_rep = r
            break
            
    if not part_rep: return

    # 2. Update Transfer Function Range
    info = part_rep.Input.GetDataInformation().GetPointDataInformation()
    vel_array = info.GetArrayInformation('velocity')
    if vel_array:
        r = vel_array.GetComponentRange(-1)
        lut = part_rep.LookupTable
        if lut:
            # You might want to smooth this so it doesn't flicker, 
            # but here is the direct update:
            lut.RescaleTransferFunction(r[0], r[1])

    # 3. Update Radius (Optional, if box expands)
    # bounds = part_rep.Input.GetDataInformation().GetBounds()
    # ... recalc diagonal logic ..

def setup_scalar_field_view(source_proxy, view, channel_name):
    """
    Applies Volume rendering for scalar fields.
    Creates a local ResampleToImage filter.
    """
    # 1. Clean existing
    # Check for local resample filter
    resample_name = f"{channel_name}_Resample"
    existing_resample = simple.FindSource(resample_name)
    if existing_resample:
        simple.Delete(existing_resample)
        
    existing_rep = simple.GetRepresentation(source_proxy, view)
    if existing_rep:
        simple.Delete(existing_rep)

    # 2. Create ResampleToImage Filter
    base_proxy = simple.FindSource(channel_name)    
    base_info = base_proxy.GetDataInformation()
    info = source_proxy.GetDataInformation()
    cinfo = info.GetCellDataInformation()
    ghost_info = cinfo.GetArrayInformation("vtkGhostType") if cinfo else None

    local_bounds = base_info.GetBounds()
    local_extent = base_info.GetExtent()
    global_bounds = info.GetBounds()
    sampling_dims = [16, 16, 16]

    
    # Try to derive from extent if structured
    nx = local_extent[1] - local_extent[0] + 1
    ny = local_extent[3] - local_extent[2] + 1
    nz = local_extent[5] - local_extent[4] + 1
    
    lx = local_bounds[1] - local_bounds[0]
    ly = local_bounds[3] - local_bounds[2]
    lz = local_bounds[5] - local_bounds[4]
    
    # Avoid div by zero
    spacing_x = lx / max(nx - 1, 1)
    spacing_y = ly / max(ny - 1, 1)
    spacing_z = lz / max(nz - 1, 1)
    
    dx = max(spacing_x, 1e-12)
    dy = max(spacing_y, 1e-12)
    dz = max(spacing_z, 1e-12)
    
    gx = global_bounds[1] - global_bounds[0]
    gy = global_bounds[3] - global_bounds[2]
    gz = global_bounds[5] - global_bounds[4]
    
    dim_x = int(round(gx / dx)) - 2
    dim_y = int(round(gy / dy)) - 2
    dim_z = int(round(gz / dz)) - 2
    
    global_extent = [dim_x, dim_y, dim_z]
    # Optionally trim one layer if you want to exclude outer ghost ring from resampling
    if ghost_info:
        print("Ghost Present")
        cut_layers = 1  # set to 0 to keep full bounds
    else:
        print("No Ghost Present")
        cut_layers = 0  # set to 0 to keep full bounds
    
    global_bounds = (
        global_bounds[0] + cut_layers * dx,
        global_bounds[1] - cut_layers * dx,
        global_bounds[2] + cut_layers * dy,
        global_bounds[3] - cut_layers * dy,
        global_bounds[4] + cut_layers * dz,
        global_bounds[5] - cut_layers * dz,
    )
    print(f"ResampleToImage: Derived dims {global_extent} from extent {local_extent}, local bounds {local_bounds} and global bounds {global_bounds}")
    



    resample = simple.ResampleToImage(registrationName=resample_name, Input=source_proxy)
    resample.UseInputBounds = 0
    resample.SamplingBounds = global_bounds
    resample.SamplingDimensions = global_extent
    association = 'POINTS'

    # 3. Show Resampled Data
    # rep = simple.Show(source_proxy, view)
    resample.UpdatePipeline()
    rep = simple.Show(resample, view)
    rep.SetRepresentationType('Volume')

    info = resample.GetDataInformation()
    point_data = info.GetPointDataInformation()

    array_name = None
    association = 'POINTS' # ResampleToImage produces Point Data
    
    # Heuristic: Check if suffix of channel name exists as array
    suffix = channel_name.replace("ippl_sField_", "")
    
    if point_data.GetArrayInformation(suffix):
        array_name = suffix
    else:
        # Fallback: pick first available array
        if point_data.GetNumberOfArrays() > 0:
            array_name = point_data.GetArrayInformation(0).GetName()

    if array_name:
        print(f"Visualizing Scalar Field: {array_name} ({association})")
        
        # Color Transfer Function
        lut = simple.GetColorTransferFunction(array_name)
        r = point_data.GetArrayInformation(array_name).GetComponentRange(-1)
            
        if r[1] > r[0]:
            lut.RescaleTransferFunction(r[0], r[1])
            
        # Opacity Transfer Function
        pwf = simple.GetOpacityTransferFunction(array_name)
        pwf.RescaleTransferFunction(r[0], r[1])

        rep.ColorArrayName = [association, array_name]
        rep.LookupTable = lut
        rep.OpacityArrayName = [association, array_name]
        rep.OpacityTransferFunction = 'Piecewise Function'
        rep.ScalarOpacityFunction = pwf
        
        # Scalar Bar
        sb = simple.GetScalarBar(lut, view)
        sb.Title = array_name
        sb.Visibility = 1
        rep.SetScalarBarVisibility(view, True)
        
    else:
        print("No scalar array found for Volume rendering.")

    view.AxesGrid.Visibility = 1
    
    return rep

def update_scalar_field_view(source_proxy, view):
    """
    Updates color range for scalar fields.
    """
    rep = simple.GetRepresentation(source_proxy, view)
    if not rep: return
    
    # Check if it is a Volume representation
    if rep.Representation != 'Volume': return

    # Get array name
    # rep.ColorArrayName is usually ['POINTS', 'name']
    ca = rep.ColorArrayName
    if not ca or len(ca) < 2: return
    
    association = ca[0]
    array_name = ca[1]
    
    if not array_name: return
    
    info = rep.Input.GetDataInformation()
    if association == 'POINTS':
        data_info = info.GetPointDataInformation()
    else:
        data_info = info.GetCellDataInformation()
        
    array_info = data_info.GetArrayInformation(array_name)
    if array_info:
        r = array_info.GetComponentRange(-1)
        lut = rep.LookupTable
        if lut:
            lut.RescaleTransferFunction(r[0], r[1])
        pwf = rep.ScalarOpacityFunction
        if pwf:
            pwf.RescaleTransferFunction(r[0], r[1])

def setup_vector_field_view(source_proxy, view, channel_name, is_extracted=False):
    """
    Applies Glyph (Arrow) rendering for vector fields.
    """
    # 1. Clean existing
    glyph_name = f"{channel_name}_Glyph"
    existing_glyph = simple.FindSource(glyph_name)
    if existing_glyph:
        simple.Delete(existing_glyph)
        
    existing_rep = simple.GetRepresentation(source_proxy, view)
    if existing_rep:
        simple.Delete(existing_rep)

    display_proxy = None

    if not is_extracted:
        # 2. Calculate Scale Factor
        info = source_proxy.GetDataInformation()
        bounds = info.GetBounds()
        global_bounds = get_global_spatial_bounds(bounds)
        
        dx = global_bounds[1] - global_bounds[0]
        dy = global_bounds[3] - global_bounds[2]
        dz = global_bounds[5] - global_bounds[4]
        
        import math
        diag = math.sqrt(dx*dx + dy*dy + dz*dz)
        scale_factor = diag / 30.0 if diag > 0 else 1.0

        # 3. Create Glyph Filter
        glyph = simple.Glyph(registrationName=glyph_name, Input=source_proxy, GlyphType='Arrow')
        
        # Determine Array Name
        c_info = info.GetCellDataInformation()
        p_info = info.GetPointDataInformation()
        
        array_name = None
        association = 'CELLS'
        
        candidates = [channel_name, channel_name.replace("ippl_vField_", "")]
        
        for name in candidates:
            if c_info.GetArrayInformation(name):
                array_name = name
                association = 'CELLS'
                break
            if p_info.GetArrayInformation(name):
                array_name = name
                association = 'POINTS'
                break
                
        if array_name:
            glyph.OrientationArray = [association, array_name]
            # glyph.ScaleArray = [association, array_name] 
        
        glyph.ScaleFactor = scale_factor
        glyph.UpdatePipeline()
        display_proxy = glyph
    else:
        display_proxy = source_proxy

    # 4. Show
    rep = simple.Show(display_proxy, view)
    rep.SetRepresentationType('Surface')
    
    # Coloring Logic
    info = display_proxy.GetDataInformation()
    c_info = info.GetCellDataInformation()
    p_info = info.GetPointDataInformation()
    
    array_name = None
    association = 'POINTS' # Glyphs usually have point data
    
    # Try to find the array to color by
    candidates = [channel_name, channel_name.replace("ippl_vField_", "")]
    
    for name in candidates:
        if p_info.GetArrayInformation(name):
            array_name = name
            association = 'POINTS'
            break
        if c_info.GetArrayInformation(name):
            array_name = name
            association = 'CELLS'
            break
            
    if array_name:
        rep.ColorArrayName = [association, array_name]
        
        lut = simple.GetColorTransferFunction(array_name)
        
        if association == 'CELLS':
            r = c_info.GetArrayInformation(array_name).GetComponentRange(-1)
        else:
            r = p_info.GetArrayInformation(array_name).GetComponentRange(-1)
            
        if r[1] > r[0]:
            lut.RescaleTransferFunction(r[0], r[1])
            
        rep.LookupTable = lut
        
        sb = simple.GetScalarBar(lut, view)
        sb.Title = array_name
        sb.ComponentTitle = 'Magnitude'
        sb.Visibility = 1
        rep.SetScalarBarVisibility(view, True)

    view.AxesGrid.Visibility = 1
    simple.ResetCamera()
    
    return rep

def update_vector_field_view(source_proxy, view):
    """
    Updates color range for vector fields.
    """
    rep = simple.GetRepresentation(source_proxy, view)
    if not rep: return
    
    ca = rep.ColorArrayName
    if ca and len(ca) >= 2:
        array_name = ca[1]
        if array_name:
            info = rep.Input.GetDataInformation()
            data_info = info.GetPointDataInformation()
            array_info = data_info.GetArrayInformation(array_name)
            if array_info:
                r = array_info.GetComponentRange(-1)
                lut = rep.LookupTable
                if lut:
                    lut.RescaleTransferFunction(r[0], r[1])