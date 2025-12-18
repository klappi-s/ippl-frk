from paraview import simple
from catalystSubroutines import get_global_spatial_bounds, nice_bounds_sym, get_global_range, auto_camera_from_bounds, find_source_by_name

try:
    from . import trame_logging as log
except Exception:
    import trame_app.trame_logging as log

def setup_default_view(source_proxy, view):
    rep = simple.Show(source_proxy, view)
    info = source_proxy.GetDataInformation()
    if info.GetNumberOfCells() > 0:
        rep.SetRepresentationType('Surface')
    else:
        rep.SetRepresentationType('Points')
    return rep


def setup_particle_view(source_proxy, view, channel_name):    
    existing_rep = simple.GetRepresentation(source_proxy, view)
    if existing_rep:
        simple.Delete(existing_rep)

    found_source =  simple.FindSource(channel_name+".bunch")
    found_source.UpdatePipeline()
    input_for_particles = found_source

    part_rep = simple.Show(input_for_particles, view)
    part_rep.SetRepresentationType('Point Gaussian')

    info = input_for_particles.GetDataInformation()
    point_data = info.GetPointDataInformation()

    num_arrays = point_data.GetNumberOfArrays()
    log.debug("Available Point Arrays ({})", num_arrays)
    for i in range(num_arrays):
        array_info = point_data.GetArrayInformation(i)
        log.debug(" - {}", array_info.GetName() if array_info else f"[Array {i} info is None]")

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
        part_rep.ColorArrayName = [None, '']

    bounds = info.GetBounds()
    dx = bounds[1] - bounds[0]
    dy = bounds[3] - bounds[2]
    dz = bounds[5] - bounds[4]

    import math
    if dx < 0 or dy < 0 or dz < 0:
        diagonal = 0.0
    else:
        diagonal = math.hypot(dx, dy, dz)
    if diagonal > 0:
        part_rep.GaussianRadius = diagonal / 500.0
    else:
        part_rep.GaussianRadius = 0.05

    box_source = simple.FindSource(channel_name+".box")
    if box_source:
        existing_box_rep = simple.GetRepresentation(box_source, view)
        if existing_box_rep:
            simple.Delete(existing_box_rep)
        box_source.UpdatePipeline()
        info = box_source.GetDataInformation()
        log.debug("Box Data: {} points, {} cells", info.GetNumberOfPoints(), info.GetNumberOfCells())
        box_rep = simple.Show(box_source, view)
        box_rep.SetRepresentationType('Outline')
        box_rep.AmbientColor = [1.0, 1.0, 0.0]
        box_rep.DiffuseColor = [1.0, 1.0, 0.0]
        box_rep.LineWidth = 2.0
        box_rep.Opacity = 0.1 
        box_rep.Visibility = 1
        box_rep.ColorArrayName = [None, ''] 
        box_rep.MapScalars = 0
    else:
        log.warn("Warning: {}.box not found.", channel_name)

    return part_rep

def update_particle_view(source_proxy, view):
    reps = view.Representations
    part_rep = None
    for r in reps:
        if hasattr(r, 'GaussianRadius'):
            part_rep = r
            break
    if not part_rep:
        return
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

def setup_scalar_field_view(source_proxy, view, channel_name):
    resample_name = f"{channel_name}_Resample"
    existing_resample = simple.FindSource(resample_name)
    if existing_resample:
        simple.Delete(existing_resample)
    existing_rep = simple.GetRepresentation(source_proxy, view)
    if existing_rep:
        simple.Delete(existing_rep)

    # Ensure pipelines are updated before querying data info
    source_proxy.UpdatePipeline()
    
    base_proxy = simple.FindSource(channel_name)
    if base_proxy:
        base_proxy.UpdatePipeline()
        base_info = base_proxy.GetDataInformation()
    else:
        base_info = None
        
    info = source_proxy.GetDataInformation()
    cinfo = info.GetCellDataInformation()
    ghost_info = cinfo.GetArrayInformation("vtkGhostType") if cinfo else None

    # Get bounds - this should always be valid from the MergedBlocks
    global_bounds = info.GetBounds()
    log.debug("Initial global_bounds from source_proxy: {}", global_bounds)
    
    # Validate global bounds
    if global_bounds[0] > global_bounds[1] or abs(global_bounds[0]) > 1e100:
        log.warn("Invalid global bounds: {}. Cannot proceed.", global_bounds)
        return None
    
    # Try to get extent information to determine grid resolution
    # First try base_proxy (the original data before MergeBlocks)
    local_extent = None
    
    if base_info:
        # ATTENTION THIS IMPLEMENTATION RELIES ON THE FACT THAT SOURCE PROXIES ARE
        # NOT PROPERLY MANAGED FOR MULTIPLE NODES TO WE CAN RECEIVE LOCAL INFO
        # ELSE WE COULD SIMPLIFY THIS FUNCTION
        # SHOULD ADD TESTCASE LOCAL BOUND = GLOBAL BOUND ...

        local_bounds = base_info.GetBounds()
        local_extent = base_info.GetExtent()
        log.debug("Extent from base_proxy: {}", local_extent)
        # Validate - check for uninitialized values
        if local_extent[0] > local_extent[1] or local_extent[0] == 2147483647:
            log.debug("Invalid extent from base_proxy, trying source_proxy")
            local_extent = None
            local_bounds = None

    
    
    if local_extent is None or local_bounds is None:
        # Fallback: use source_proxy (MergedBlocks) info
        local_bounds = info.GetBounds()
        local_extent = info.GetExtent()
        
        # If still invalid, try to compute from bounds
        if local_extent[0] >= local_extent[1] or local_extent[0] == 2147483647:
            log.warn("Invalid extent from source_proxy too. Computing from global bounds. Using 8,8,8 and globald bounds")
            # Default to reasonable grid size
            local_extent = (0, 8, 0, 8, 0, 8)
            local_bounds = global_bounds


    # # If we still don't have valid extent, try to infer from number of cells
    # if local_extent is None:
    #     num_cells = info.GetNumberOfCells()
    #     num_points = info.GetNumberOfPoints()
    #     log.debug("No valid extent. num_cells={}, num_points={}", num_cells, num_points)
        
    #     if num_points > 0:
    #         # Assume cubic grid and estimate dimension from number of points
    #         import math
    #         est_dim = int(round(num_points ** (1.0/3.0)))
    #         est_dim = max(2, min(est_dim, 1000))  # Clamp to reasonable range
    #         local_extent = (0, est_dim - 1, 0, est_dim - 1, 0, est_dim - 1)
    #         log.debug("Estimated extent from num_points: {}", local_extent)
    #     elif num_cells > 0:
    #         import math
    #         est_dim = int(round(num_cells ** (1.0/3.0))) + 1
    #         est_dim = max(2, min(est_dim, 1000))
    #         local_extent = (0, est_dim - 1, 0, est_dim - 1, 0, est_dim - 1)
    #         log.debug("Estimated extent from num_cells: {}", local_extent)
    #     else:
    #         # Last resort: use a small default
    #         local_extent = (0, 9, 0, 9, 0, 9)  # 10x10x10 grid
    #         log.warn("Could not determine grid size, using default 10x10x10")


    # Now compute dimensions from extent (amount of nodes)

    # local cells per dim
    nx = local_extent[1] - local_extent[0]
    ny = local_extent[3] - local_extent[2]
    nz = local_extent[5] - local_extent[4]
    
    # local bounds 
    lx = local_bounds[1] - local_bounds[0]
    ly = local_bounds[3] - local_bounds[2]
    lz = local_bounds[5] - local_bounds[4]

    # Use global bounds for spacing calculation
    gx = global_bounds[1] - global_bounds[0]
    gy = global_bounds[3] - global_bounds[2]
    gz = global_bounds[5] - global_bounds[4]


    # spacing, cell width
    wx = lx / max(nx, 1)
    wy = ly / max(ny, 1)
    wz = lz / max(nz, 1)
    wx = max(wx, 1e-12)
    wy = max(wy, 1e-12)
    wz = max(wz, 1e-12)

    # global extent
    dx = gx/wx
    dy = gy/wy
    dz = gz/wz

    # For resampling, use the same resolution as the input (minus ghost layers)
    dim_x = dx - 2 if ghost_info else dx
    dim_y = dy - 2 if ghost_info else dy
    dim_z = dz - 2 if ghost_info else dz

    # Ensure minimum global extent dimensions
    dim_x = int(max(dim_x, 1))
    dim_y = int(max(dim_y, 1))
    dim_z = int(max(dim_z, 1))

    global_extent = [dim_x, dim_y, dim_z]
    
    if ghost_info:
        log.debug("Ghost Present - trimming 1 layer")
        cut_layers = 1
    else:
        log.debug("No Ghost Present")
        cut_layers = 0
    
    # Compute resampling bounds (trim ghost layers if present)
    resampling_bounds = (
        global_bounds[0] + cut_layers * wx,
        global_bounds[1] - cut_layers * wx,
        global_bounds[2] + cut_layers * wy,
        global_bounds[3] - cut_layers * wy,
        global_bounds[4] + cut_layers * wz,
        global_bounds[5] - cut_layers * wz,
    )
    log.debug("ResampleToImage: global_extent={}, loca_extent={}, spacing=({:.4f},{:.4f},{:.4f}), resampling_bounds={}", 
              global_extent, local_extent, wx, wy, wz, resampling_bounds)

    resample = simple.ResampleToImage(registrationName=resample_name, Input=source_proxy)
    resample.UseInputBounds = 0
    resample.SamplingBounds = resampling_bounds
    resample.SamplingDimensions = global_extent

    resample.UpdatePipeline()
    rep = simple.Show(resample, view)
    rep.SetRepresentationType('Volume')

    info = resample.GetDataInformation()
    point_data = info.GetPointDataInformation()
    array_name = None
    association = 'POINTS'
    suffix = channel_name.replace("ippl_sField_", "")
    if point_data.GetArrayInformation(suffix):
        array_name = suffix
    else:
        if point_data.GetNumberOfArrays() > 0:
            array_name = point_data.GetArrayInformation(0).GetName()
    if array_name:
        log.info("Visualizing Scalar Field: {} ({})", array_name, association)
        lut = simple.GetColorTransferFunction(array_name)
        lut.RGBPoints = [-2.00, 0.231373, 0.298039, 0.752941, 
                         0.00, 0.865003, 0.865003, 0.865003, 
                         2.00, 0.705882, 0.0156863, 0.14902]
        lut.ScalarRangeInitialized = 1.0
        pwf = simple.GetOpacityTransferFunction(array_name)
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
        # Use average spacing for unit distance to ensure visibility
        avg_spacing = (dx + dy + dz) / 3.0
        rep.ScalarOpacityUnitDistance = avg_spacing
        sb = simple.GetScalarBar(lut, view)
        sb.Title = array_name
        sb.ComponentTitle = 'Magnitude'
        sb.Visibility = 1
        rep.SetScalarBarVisibility(view, True)
    else:
        log.warn("No scalar array found for Volume rendering.")
    return rep

def update_scalar_field_view(source_proxy, view):
    rep = simple.GetRepresentation(source_proxy, view)
    if not rep:
        return
    if rep.Representation != 'Volume':
        return
    ca = rep.ColorArrayName
    if not ca or len(ca) < 2:
        return
    association = ca[0]
    array_name = ca[1]
    if not array_name:
        return
    info = rep.Input.GetDataInformation()
    data_info = info.GetPointDataInformation() if association == 'POINTS' else info.GetCellDataInformation()
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

def setup_vector_field_view(source_proxy, view, channel_name):
    """Setup vector field visualization using glyphs.
    
    IMPORTANT: We ALWAYS create a local glyph from MergedBlocks, regardless of
    whether an extracted glyph exists. This is because:
    1. Extracted glyphs can become corrupted after other field initializations
    2. Extracted glyphs may only contain data from a single rank
    3. Local glyphs from MergedBlocks ensure we get complete multi-rank data
    
    Args:
        source_proxy: The MergedBlocks proxy (or base proxy) containing the vector data
        view: The ParaView view to render in
        channel_name: The name of the vector field channel (e.g., "ippl_vField_E")
    """
    
    glyph_name = f"{channel_name}_Glyph"
    
    # Clean up any existing local glyph
    existing_glyph = simple.FindSource(glyph_name)
    if existing_glyph:
        existing_rep = simple.GetRepresentation(existing_glyph, view)
        if existing_rep:
            simple.Delete(existing_rep)
        simple.Delete(existing_glyph)
    
    # Also clean up representation of source_proxy if it exists
    existing_rep = simple.GetRepresentation(source_proxy, view)
    if existing_rep:
        simple.Delete(existing_rep)
    
    # Find the MergedBlocks source - this contains complete data from all ranks
    merged_name = f"{channel_name}.MergedBlocks"
    merged_proxy = simple.FindSource(merged_name)
    
    if not merged_proxy:
        # Try the channel name directly as fallback
        merged_proxy = simple.FindSource(channel_name)
        if merged_proxy:
            log.debug("Using parent source {} (no MergedBlocks found)", channel_name)
    else:
        log.debug("Using MergedBlocks source: {}", merged_name)
    
    if not merged_proxy:
        log.error("Cannot setup vector field: no valid source found for {}", channel_name)
        return None
    
    # Ensure pipeline is updated
    merged_proxy.UpdatePipeline()
    m_info = merged_proxy.GetDataInformation()
    
    # Debug: log source data information
    num_points = m_info.GetNumberOfPoints()
    num_cells = m_info.GetNumberOfCells()
    bounds = m_info.GetBounds()
    log.debug("Vector field MergedBlocks: points={}, cells={}, bounds={}", num_points, num_cells, bounds)
    
    if num_points == 0 and num_cells == 0:
        log.error("MergedBlocks source has no data for {}", channel_name)
        return None
    
    # Get global bounds for scale factor calculation
    global_bounds = get_global_spatial_bounds(bounds)
    dx = global_bounds[1] - global_bounds[0]
    dy = global_bounds[3] - global_bounds[2]
    dz = global_bounds[5] - global_bounds[4]
    import math
    diag = math.sqrt(dx*dx + dy*dy + dz*dz)
    scale_factor = diag / 30.0 if diag > 0 else 1.0
    
    # Find the vector array for glyph orientation
    m_p_info = m_info.GetPointDataInformation()
    m_c_info = m_info.GetCellDataInformation()
    
    # Debug: list available arrays
    point_arrays = [m_p_info.GetArrayInformation(i).GetName() for i in range(m_p_info.GetNumberOfArrays())] if m_p_info else []
    cell_arrays = [m_c_info.GetArrayInformation(i).GetName() for i in range(m_c_info.GetNumberOfArrays())] if m_c_info else []
    log.debug("MergedBlocks point arrays: {}, cell arrays: {}", point_arrays, cell_arrays)
    
    array_name = None
    association = 'POINTS'
    
    # Try to find vector array by expected names
    suffix = channel_name.replace("ippl_vField_", "")
    candidates = [channel_name, suffix]
    for name in candidates:
        if m_p_info and m_p_info.GetArrayInformation(name):
            array_name = name
            association = 'POINTS'
            log.debug("Found vector array by name: {} (POINTS)", array_name)
            break
        if m_c_info and m_c_info.GetArrayInformation(name):
            array_name = name
            association = 'CELLS'
            log.debug("Found vector array by name: {} (CELLS)", array_name)
            break
    
    # If not found by name, look for any 3-component array (vector)
    if array_name is None and m_p_info:
        for i in range(m_p_info.GetNumberOfArrays()):
            ai = m_p_info.GetArrayInformation(i)
            if ai and ai.GetNumberOfComponents() in (3, 2):
                array_name = ai.GetName()
                association = 'POINTS'
                log.debug("Found vector array by component count: {} ({} components)", array_name, ai.GetNumberOfComponents())
                break
    
    if array_name is None and m_c_info:
        for i in range(m_c_info.GetNumberOfArrays()):
            ai = m_c_info.GetArrayInformation(i)
            if ai and ai.GetNumberOfComponents() in (3, 2):
                array_name = ai.GetName()
                association = 'CELLS'
                log.debug("Found vector array by component count: {} ({} components)", array_name, ai.GetNumberOfComponents())
                break
    
    if not array_name:
        log.warn("No suitable vector array found for glyph orientation in {}", channel_name)
    
    # Create the local glyph filter
    log.info("Creating local glyph for {} from MergedBlocks", channel_name)
    glyph = simple.Glyph(registrationName=glyph_name, Input=merged_proxy, GlyphType='Arrow')
    
    if array_name:
        glyph.OrientationArray = [association, array_name]
        log.debug("Set glyph OrientationArray to [{}, {}]", association, array_name)
    
    glyph.ScaleFactor = scale_factor
    glyph.UpdatePipeline()
    
    # Verify glyph has data
    glyph_info = glyph.GetDataInformation()
    glyph_points = glyph_info.GetNumberOfPoints()
    glyph_cells = glyph_info.GetNumberOfCells()
    glyph_bounds = glyph_info.GetBounds()
    log.debug("Created glyph: points={}, cells={}, bounds={}", glyph_points, glyph_cells, glyph_bounds)
    
    if glyph_points == 0 and glyph_cells == 0:
        log.error("Created glyph has no data!")
    
    display_proxy = glyph
            
    rep = simple.Show(display_proxy, view)
    rep.SetRepresentationType('Surface')
    
    # Ensure we have fresh data info
    display_proxy.UpdatePipeline()
    info = display_proxy.GetDataInformation()
    
    # Debug: Check bounds and data
    bounds = info.GetBounds()
    num_pts = info.GetNumberOfPoints()
    num_cls = info.GetNumberOfCells()
    log.debug("Vector field display_proxy: bounds={}, points={}, cells={}", bounds, num_pts, num_cls)
    
    c_info = info.GetCellDataInformation()
    p_info = info.GetPointDataInformation()
    
    # Debug: List available arrays
    point_arrays = [p_info.GetArrayInformation(i).GetName() for i in range(p_info.GetNumberOfArrays())] if p_info else []
    cell_arrays = [c_info.GetArrayInformation(i).GetName() for i in range(c_info.GetNumberOfArrays())] if c_info else []
    log.debug("Vector field point arrays: {}, cell arrays: {}", point_arrays, cell_arrays)
    
    array_name = None
    association = 'POINTS'
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
    return rep

def update_vector_field_view(source_proxy, view):
    rep = simple.GetRepresentation(source_proxy, view)
    if not rep:
        return
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
    if not view:
        return
    target_proxy = source_proxy
    sel = selection_name
    if sel:
        if sel.endswith(".bunch") or sel.endswith(".box"):
            target_proxy = source_proxy
        elif sel.startswith("ippl_vField"):
            p = find_source_by_name(f"{sel}_Glyph")
            if not p: p = find_source_by_name(f"{sel}.Glyph")
            if p: target_proxy = p
        elif sel.startswith("ippl_sField"):
            p = find_source_by_name(f"{sel}_Resample")
            if not p: p = find_source_by_name(f"{sel}.MergedBlocks")
            if p: target_proxy = p
        elif sel.startswith("ippl_particles"):
            p = find_source_by_name(f"{sel}.bunch")
            if p: target_proxy = p
    if not target_proxy:
        return
    target_proxy.UpdatePipeline()
    info = target_proxy.GetDataInformation()
    bounds = info.GetBounds()
    if bounds[0] > bounds[1]:
        rep = simple.GetRepresentation(target_proxy, view)
        if rep:
            info = rep.Input.GetDataInformation()
            bounds = info.GetBounds()
    if bounds[0] > bounds[1]:
        log.warn("Cannot reset camera: Invalid bounds for {}", target_proxy.GetLogName())
        return
    auto_camera_from_bounds(view, bounds)