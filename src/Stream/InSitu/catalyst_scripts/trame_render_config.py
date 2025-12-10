from paraview import simple
from catalystSubroutines import get_global_spatial_bounds, nice_bounds_sym, get_global_range, auto_camera_from_bounds, find_source_by_name

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


def setup_particle_view(source_proxy, view, channel_name):
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
    # particles_extract = simple.ExtractBlock(Input=source_proxy, registrationName="Particles_Only")
    # particles_extract.Selectors = ['//main'] 
    # particles_extract.UpdatePipeline()
    # input_for_particles = particles_extract

    found_source =  simple.FindSource(channel_name+".bunch")
    found_source.UpdatePipeline()
    input_for_particles = found_source

    # --- VISUALIZE PARTICLES ---
    # Show the particles using Point Gaussian
    part_rep = simple.Show(input_for_particles, view)
    part_rep.SetRepresentationType('Point Gaussian')
    
    # Configure Color: prefer existing selection; fallback to velocity; else first array
    info = input_for_particles.GetDataInformation()
    point_data = info.GetPointDataInformation()

    # Debug: Print available arrays
    num_arrays = point_data.GetNumberOfArrays()
    print(f"Available Point Arrays ({num_arrays}):")
    for i in range(num_arrays):
        array_info = point_data.GetArrayInformation(i)
        print(f" - {array_info.GetName()}" if array_info else f" - [Array {i} info is None]")

    # Determine target array
    current_ca = getattr(part_rep, 'ColorArrayName', None)
    target_assoc = None
    target_name = None
    if current_ca and isinstance(current_ca, list) and len(current_ca) >= 2 and current_ca[0] and current_ca[1]:
        target_assoc, target_name = current_ca[0], current_ca[1]
    elif point_data.GetArrayInformation('velocity'):
        target_assoc, target_name = 'POINTS', 'velocity'
    elif num_arrays > 0:
        ai0 = point_data.GetArrayInformation(0)
        target_assoc, target_name = 'POINTS', (ai0.GetName() if ai0 else None)

    if target_name:
        part_rep.ColorArrayName = [target_assoc, target_name]
        lut = simple.GetColorTransferFunction(target_name)
        r = point_data.GetArrayInformation(target_name).GetComponentRange(-1) if point_data.GetArrayInformation(target_name) else (0.0, 1.0)
        if r[1] > r[0]:
            lut.RescaleTransferFunction(r[0], r[1])
        part_rep.LookupTable = lut
        sb = simple.GetScalarBar(lut, view)
        sb.Title = target_name
        sb.ComponentTitle = 'Magnitude'
        sb.Visibility = 1
        part_rep.SetScalarBarVisibility(view, True)
    else:
        # Solid color fallback
        part_rep.ColorArrayName = [None, '']

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

    box_source = simple.FindSource(channel_name+".box")
    if box_source:
        # Clean existing rep to ensure fresh state
        existing_box_rep = simple.GetRepresentation(box_source, view)
        if existing_box_rep:
            simple.Delete(existing_box_rep)

        box_source.UpdatePipeline()
        
        # Debug info
        info = box_source.GetDataInformation()
        print(f"Box Data: {info.GetNumberOfPoints()} points, {info.GetNumberOfCells()} cells")

        box_rep = simple.Show(box_source, view)
        box_rep.SetRepresentationType('Outline')
        
        # Solid Color: Yellow
        box_rep.AmbientColor = [1.0, 1.0, 0.0]
        box_rep.DiffuseColor = [1.0, 1.0, 0.0]
        box_rep.LineWidth = 2.0
        box_rep.Opacity = 0.1 
        box_rep.Visibility = 1
        
        # Correct way to disable array coloring (Solid Color)
        box_rep.ColorArrayName = [None, ''] 
        box_rep.MapScalars = 0
    else:
        print(f"Warning: {channel_name}.box not found.")

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

    # 2. Update Transfer Function Range based on current ColorArrayName
    ca = getattr(part_rep, 'ColorArrayName', None)
    if ca and isinstance(ca, list) and len(ca) >= 2 and ca[0] and ca[1]:
        assoc, array_name = ca[0], ca[1]
        dinfo = part_rep.Input.GetDataInformation()
        data_info = dinfo.GetPointDataInformation() if assoc == 'POINTS' else dinfo.GetCellDataInformation()
        arr_info = data_info.GetArrayInformation(array_name) if data_info else None
        if arr_info:
            r = arr_info.GetComponentRange(-1)
            lut = part_rep.LookupTable
            if lut:
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
        
        # Define the specific color map from png_ext_sfield.py
        lut.RGBPoints = [-2.00, 0.231373, 0.298039, 0.752941, 
                         0.00, 0.865003, 0.865003, 0.865003, 
                         2.00, 0.705882, 0.0156863, 0.14902]
        lut.ScalarRangeInitialized = 1.0

        # Opacity Transfer Function
        pwf = simple.GetOpacityTransferFunction(array_name)
        
        # Define the specific opacity map from png_ext_sfield.py
        pwf.Points = [-2.00, 1.00, 0.5, 0.0, 
                     -1.20, 0.75, 0.5, 0.0, 
                     -0.80, 0.25, 0.5, 0.0, 
                     -0.01, 0.00, 0.5, 0.0, 
                      0.00, 1.00, 0.5, 0.0, 
                      0.01, 0.00, 0.5, 0.0, 
                      0.80, 0.25, 0.5, 0.0, 
                      1.20, 0.75, 0.5, 0.0, 
                      2.00, 1.00, 0.5, 0.0]
        pwf.ScalarRangeInitialized = 1

        # Rescale based on actual data
        local_min, local_max = point_data.GetArrayInformation(array_name).GetComponentRange(-1)
        global_min, global_max = get_global_range(local_min, local_max)
        nice_min, nice_max = nice_bounds_sym(global_min, global_max)
        
        lut.RescaleTransferFunction(nice_min, nice_max)
        pwf.RescaleTransferFunction(nice_min, nice_max)

        rep.ColorArrayName = [association, array_name]
        rep.LookupTable = lut
        rep.OpacityArrayName = [association, array_name]
        rep.OpacityTransferFunction = 'Piecewise Function'
        rep.ScalarOpacityFunction = pwf
        rep.ScalarOpacityUnitDistance = 4.00
        
        # Scalar Bar
        sb = simple.GetScalarBar(lut, view)
        sb.Title = array_name
        sb.ComponentTitle = 'Magnitude'
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
        local_min, local_max = array_info.GetComponentRange(-1)
        global_min, global_max = get_global_range(local_min, local_max)
        nice_min, nice_max = nice_bounds_sym(global_min, global_max)

        lut = rep.LookupTable
        if lut:
            lut.RescaleTransferFunction(nice_min, nice_max)
        pwf = rep.ScalarOpacityFunction
        if pwf:
            pwf.RescaleTransferFunction(nice_min, nice_max)

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

        # Fallback: choose the first 3-component vector array available
        if array_name is None:
            # Check point data first
            if p_info and p_info.GetNumberOfArrays() > 0:
                for i in range(p_info.GetNumberOfArrays()):
                    ai = p_info.GetArrayInformation(i)
                    if ai and ai.GetNumberOfComponents() in (3, 2):
                        array_name = ai.GetName()
                        association = 'POINTS'
                        break
            # Then cell data
            if array_name is None and c_info and c_info.GetNumberOfArrays() > 0:
                for i in range(c_info.GetNumberOfArrays()):
                    ai = c_info.GetArrayInformation(i)
                    if ai and ai.GetNumberOfComponents() in (3, 2):
                        array_name = ai.GetName()
                        association = 'CELLS'
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
    # Fallback to first available array if candidates absent
    if array_name is None:
        if p_info and p_info.GetNumberOfArrays() > 0:
            array_name = p_info.GetArrayInformation(0).GetName()
            association = 'POINTS'
        elif c_info and c_info.GetNumberOfArrays() > 0:
            array_name = c_info.GetArrayInformation(0).GetName()
            association = 'CELLS'
            
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

def reset_camera(view, selection_name, source_proxy):
    if not view: return
    
    # Identify the actual proxy being visualized to get correct bounds
    target_proxy = source_proxy
    sel = selection_name
    
    if sel:
        if sel.startswith("ippl_vField"):
            # Priority: Local Glyph -> Extracted Glyph -> Source
            p = find_source_by_name(f"{sel}_Glyph")
            if not p: p = find_source_by_name(f"{sel}.Glyph")
            if p: target_proxy = p
            
        elif sel.startswith("ippl_sField"):
            # Priority: Local Resample -> Merged -> Source
            p = find_source_by_name(f"{sel}_Resample")
            if not p: p = find_source_by_name(f"{sel}.MergedBlocks")
            if p: target_proxy = p
            
        elif sel.startswith("ippl_particles"):
             # For particles, use the bunch proxy
             p = find_source_by_name(f"{sel}.bunch")
             if p: target_proxy = p

    if not target_proxy: return

    # Get bounds
    target_proxy.UpdatePipeline()
    info = target_proxy.GetDataInformation()
    bounds = info.GetBounds()
    
    # If bounds are invalid (e.g. uninitialized), try to fetch from representation
    if bounds[0] > bounds[1]:
        rep = simple.GetRepresentation(target_proxy, view)
        if rep:
            info = rep.Input.GetDataInformation()
            bounds = info.GetBounds()
            
    # If still invalid, return
    if bounds[0] > bounds[1]:
        print(f"Cannot reset camera: Invalid bounds for {target_proxy.GetLogName()}")
        return

    auto_camera_from_bounds(view, bounds)