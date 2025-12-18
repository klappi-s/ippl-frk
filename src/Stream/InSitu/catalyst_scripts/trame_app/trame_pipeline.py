# Pipeline interactions moved from trame_vis_app.py
from typing import Any
from paraview import simple

from catalystSubroutines import find_source_by_name, get_global_spatial_bounds
try:
    from . import trame_render_config as render_config
    from . import trame_logging as log
except ImportError:
    import trame_render_config as render_config
    import trame_logging as log


def get_display_proxy(ctx: Any, name: str):
    # Special handling for compound types where display proxy != logical proxy
    if name.startswith("ippl_sField"):
        resample = find_source_by_name(f"{name}_Resample")
        merged = find_source_by_name(f"{name}.MergedBlocks")
        
        # Return the visible one if possible
        view = simple.GetActiveView()
        if view:
            if resample:
                rep = simple.GetRepresentation(resample, view)
                if rep and rep.Visibility:
                    return resample
            if merged:
                rep = simple.GetRepresentation(merged, view)
                if rep and rep.Visibility:
                    return merged
        
        # Fallback preference
        if resample: return resample
        if merged: return merged
        if name in ctx.active_proxies: return ctx.active_proxies[name]
        return find_source_by_name(name)
        
    elif name.startswith("ippl_vField"):
        p = find_source_by_name(f"{name}_Glyph")
        if p: return p
        p = find_source_by_name(f"{name}.Glyph")
        if p: return p
        if name in ctx.active_proxies: return ctx.active_proxies[name]
        return find_source_by_name(name)

    if name in ctx.active_proxies:
        return ctx.active_proxies[name]
        
    if name.endswith(".bunch") or name.endswith(".box"):
        return find_source_by_name(name)
    elif name.startswith("ippl_particles"):
        return find_source_by_name(f"{name}.bunch")
    else:
        return find_source_by_name(name)


def remove_proxy(ctx: Any, name: str):
    log.ui("Remove Source clicked: {}", name)
    
    # Before deleting, hide scalar bar for this source if it was enabled
    # This prevents orphaned scalar bars when slices are removed
    try:
        view = simple.GetActiveView()
        display_proxy = get_display_proxy(ctx, name)
        if display_proxy and view:
            rep = simple.GetRepresentation(display_proxy, view)
            if rep:
                # Hide the scalar bar for this representation
                try:
                    rep.SetScalarBarVisibility(view, False)
                except Exception:
                    pass
                # Also try to hide via the LUT's scalar bar directly
                ca = getattr(rep, 'ColorArrayName', None)
                if ca and len(ca) > 1 and ca[1]:
                    try:
                        lut = simple.GetColorTransferFunction(ca[1])
                        sb = simple.GetScalarBar(lut, view)
                        if sb:
                            # Only hide if no other active source needs this scalar bar
                            # Check if any other source (besides this one) has scalar bar enabled for same array
                            should_hide = True
                            scalar_bar_per_source = getattr(ctx.state, 'scalar_bar_per_source', {})
                            for other_name, has_bar in scalar_bar_per_source.items():
                                if other_name != name and has_bar:
                                    # Check if this other source uses the same array
                                    other_proxy = get_display_proxy(ctx, other_name)
                                    if other_proxy:
                                        other_rep = simple.GetRepresentation(other_proxy, view)
                                        if other_rep:
                                            other_ca = getattr(other_rep, 'ColorArrayName', None)
                                            if other_ca and len(other_ca) > 1 and other_ca[1] == ca[1]:
                                                # Another source uses the same array and has scalar bar visible
                                                should_hide = False
                                                break
                            if should_hide:
                                sb.Visibility = 0
                    except Exception:
                        pass
        # Clean up per-source scalar bar tracking
        if hasattr(ctx.state, 'scalar_bar_per_source'):
            try:
                d = dict(ctx.state.scalar_bar_per_source)
                if name in d:
                    del d[name]
                    ctx.state.scalar_bar_per_source = d
            except Exception:
                pass
    except Exception as e:
        log.warn("Error cleaning up scalar bar for {}: {}", name, e)
    
    # Try to find proxy in active_proxies first
    p = ctx.active_proxies.get(name)
    
    sel = name

    # Always check for associated proxies to clean up (compound proxies)
    if sel.startswith("ippl_sField"):
        p1 = find_source_by_name(f"{sel}_Resample")
        p3 = find_source_by_name(f"{sel}.MergedBlocks")
        if p1: simple.Delete(p1)
        if p3: simple.Delete(p3)
    elif sel.startswith("ippl_vField"):
        p1 = find_source_by_name(f"{sel}_Glyph")
        if p1: simple.Delete(p1)
        p2 = find_source_by_name(f"{sel}.Glyph")
        if p2: simple.Delete(p2)
    elif sel.startswith("ippl_particles") and not (sel.endswith(".bunch") or sel.endswith(".box")):
        p_box = find_source_by_name(f"{sel}.box")
        if p_box: simple.Delete(p_box)
        p_bunch = find_source_by_name(f"{sel}.bunch")
        if p_bunch: simple.Delete(p_bunch)

    if not p:
        if sel.endswith(".bunch") or sel.endswith(".box"):
            p = find_source_by_name(sel)
        else:
            p = find_source_by_name(sel)

    if p:
        log.info("Deleting proxy object for {}", name)
        try:
            simple.Delete(p)
        except Exception as e:
            log.warn("Failed to delete proxy {}: {}", name, e)
    
    # Remove from active_proxies if present
    if name in ctx.active_proxies:
        del ctx.active_proxies[name]

    # Check for split particle sources cleanup
    if name.endswith(".bunch"):
        parent = name[:-6]
        sibling = f"{parent}.box"
        # If sibling is not active, remove parent key to allow re-adding
        if sibling not in ctx.active_proxies and parent in ctx.active_proxies:
            log.debug("Cleaning up parent key {} from active_proxies", parent)
            del ctx.active_proxies[parent]
    elif name.endswith(".box"):
        parent = name[:-4]
        sibling = f"{parent}.bunch"
        # If sibling is not active, remove parent key to allow re-adding
        if sibling not in ctx.active_proxies and parent in ctx.active_proxies:
            log.debug("Cleaning up parent key {} from active_proxies", parent)
            del ctx.active_proxies[parent]
    
    # Clean up per-source settings (color_map, rescale_settings)
    for attr_name in ['color_map_per_source', 'rescale_settings_per_source']:
        if hasattr(ctx.state, attr_name):
            try:
                d = dict(getattr(ctx.state, attr_name))
                if name in d:
                    del d[name]
                    setattr(ctx.state, attr_name, d)
            except Exception:
                pass
        
    new_items = [item for item in ctx.state.pipeline_items if item['id'] != name]
    ctx.state.pipeline_items = new_items
    if not ctx.active_proxies:
        ctx.state.has_data = False
    simple.Render()
    if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
        try:
            ctx.ctrl.view_update()
        except Exception as e:
            log.warn("view_update failed: {}. Disabling further updates and live mode.", e)
            ctx.view_update_enabled = False
            ctx.state.live_mode = False
            ctx.state.status_text = "Live disabled: transport error"


def extract_slice(ctx: Any, name: str):
    log.ui("Extract Slice for: {}", name)
    
    # Find the input proxy (MergedBlocks for sField)
    input_proxy = None
    # Only use MergedBlocks lookup for the base sField source, not derived ones like Ghosts
    if name.startswith("ippl_sField") and not any(x in name for x in [".Ghosts", ".Resample", ".Slice"]):
        input_proxy = find_source_by_name(f"{name}.MergedBlocks")
    else:
        # Fallback for other types if we ever support them
        input_proxy = get_display_proxy(ctx, name)
        if not input_proxy:
            input_proxy = find_source_by_name(name)
        
    if not input_proxy:
        log.warn("Could not find input proxy for slice extraction on {}", name)
        return

    # Get the parent's representation to copy color settings
    view = simple.GetActiveView()
    parent_rep = None
    parent_array_name = None
    parent_association = None
    parent_lut = None
    parent_pwf = None
    
    # For scalar fields, the parent representation is the Resample proxy
    if name.startswith("ippl_sField"):
        resample_proxy = find_source_by_name(f"{name}_Resample")
        if resample_proxy:
            parent_rep = simple.GetRepresentation(resample_proxy, view)
    
    if parent_rep:
        ca = getattr(parent_rep, 'ColorArrayName', None)
        if ca and len(ca) >= 2 and ca[1]:
            parent_association = ca[0]
            parent_array_name = ca[1]
            parent_lut = simple.GetColorTransferFunction(parent_array_name)
            parent_pwf = simple.GetOpacityTransferFunction(parent_array_name)

    # Generate unique name
    base_name = name
    if base_name.endswith(".MergedBlocks"):
        base_name = base_name[:-13]
    
    slice_count = 1
    while f"{base_name}.Slice{slice_count}" in ctx.active_proxies:
        slice_count += 1
    
    slice_name = f"{base_name}.Slice{slice_count}"
    
    # Create Slice Filter
    slice_filter = simple.Slice(Input=input_proxy)
    slice_filter.SliceType = 'Plane'
    slice_filter.SliceType.Normal = [1.0, 0.0, 0.0] # Default X normal
    
    # Center the slice
    info = input_proxy.GetDataInformation()
    bounds = info.GetBounds()
    center = [(bounds[0]+bounds[1])/2, (bounds[2]+bounds[3])/2, (bounds[4]+bounds[5])/2]
    slice_filter.SliceType.Origin = center
    
    slice_filter.UpdatePipeline()
    
    # Register proxy
    ctx.active_proxies[slice_name] = slice_filter
    
    # Add to pipeline items
    current_items = list(ctx.state.pipeline_items)
    current_items.append({"id": slice_name, "name": f"Slice {slice_count} ({base_name})", "visible": True})
    ctx.state.pipeline_items = current_items
    
    # Show it
    rep = simple.Show(slice_filter, view)
    rep.SetRepresentationType('Surface')
    
    # Apply parent's color settings if available
    if parent_array_name and parent_lut:
        # IMPORTANT: Enable separate color map FIRST, before any color operations.
        # This ensures the slice gets its own independent LUT.
        if hasattr(rep, 'UseSeparateColorMap'):
            rep.UseSeparateColorMap = 1
        
        # Create a truly separate LUT for this slice by using a unique registration name.
        # The key insight: GetColorTransferFunction(name) returns a global LUT keyed by 'name'.
        # By using a unique name like "density.Slice1", we get a brand new LUT.
        slice_lut_name = f"{parent_array_name}.{slice_name}"
        slice_lut = simple.GetColorTransferFunction(slice_lut_name)
        
        # Copy color transfer function settings from parent
        try:
            slice_lut.RGBPoints = list(parent_lut.RGBPoints)
            slice_lut.ColorSpace = parent_lut.ColorSpace
            slice_lut.NanColor = list(parent_lut.NanColor)
            if hasattr(parent_lut, 'VectorMode'):
                slice_lut.VectorMode = parent_lut.VectorMode
            if hasattr(parent_lut, 'VectorComponent'):
                slice_lut.VectorComponent = parent_lut.VectorComponent
        except Exception as e:
            log.warn("Failed to copy LUT settings to slice: {}", e)
        
        # Check if the array exists in the expected association
        # This handles cases where ResampleToImage (parent) has Points but MergedBlocks (slice input) has Cells
        slice_info = slice_filter.GetDataInformation()
        target_association = parent_association
        
        # Helper to check existence
        def has_array(info, assoc, name):
            if assoc == 'POINTS':
                return info.GetPointDataInformation().GetArrayInformation(name) is not None
            elif assoc == 'CELLS':
                return info.GetCellDataInformation().GetArrayInformation(name) is not None
            return False

        if not has_array(slice_info, target_association, parent_array_name):
            # Try swapping association
            alt_association = 'CELLS' if target_association == 'POINTS' else 'POINTS'
            if has_array(slice_info, alt_association, parent_array_name):
                log.info("Swapping association for slice color from {} to {}", target_association, alt_association)
                target_association = alt_association
            else:
                log.warn("Array {} not found in slice output", parent_array_name)
        
        # Set the color array and assign the slice's own LUT
        rep.ColorArrayName = [target_association, parent_array_name]
        rep.LookupTable = slice_lut
        
        # Copy the opacity transfer function normalized points
        if parent_pwf:
            # Get parent's opacity points and range
            parent_points = list(parent_pwf.Points)
            
            if parent_points and len(parent_points) >= 4:
                # Get the range from the parent LUT
                rgb_points = parent_lut.RGBPoints
                if rgb_points and len(rgb_points) >= 4:
                    min_val = rgb_points[0]
                    max_val = rgb_points[-4]
                else:
                    min_val = parent_points[0]
                    max_val = parent_points[-4]
                
                width = max_val - min_val
                if width == 0:
                    width = 1.0
                
                # Normalize parent opacity points to 0-1 range
                normalized_points = []
                for k in range(0, len(parent_points), 4):
                    val = parent_points[k]
                    opacity = parent_points[k + 1]
                    mid = parent_points[k + 2]
                    sharp = parent_points[k + 3]
                    norm_x = (val - min_val) / width
                    normalized_points.append({'norm_x': norm_x, 'opacity': opacity, 'mid': mid, 'sharp': sharp})
                
                # Get the slice's data range
                slice_info = slice_filter.GetDataInformation()
                slice_pinfo = slice_info.GetPointDataInformation()
                slice_cinfo = slice_info.GetCellDataInformation()
                
                slice_array_info = None
                if parent_association == 'POINTS' and slice_pinfo:
                    slice_array_info = slice_pinfo.GetArrayInformation(parent_array_name)
                elif parent_association == 'CELLS' and slice_cinfo:
                    slice_array_info = slice_cinfo.GetArrayInformation(parent_array_name)
                
                # Use parent range for consistency
                slice_min = min_val
                slice_max = max_val
                slice_width = slice_max - slice_min
                if slice_width == 0:
                    slice_width = 1.0
                
                # Note: Surface representations (used for slices) do NOT support
                # ScalarOpacityFunction - that's only for Volume representations.
                # The opacity TF code is kept here in case the slice is later
                # changed to Volume representation, but for Surface slices it's a no-op.
                # For now, we skip trying to set opacity TF for Surface reps.
                rep_type = str(getattr(rep, 'Representation', 'Surface')).strip("'\"")
                if rep_type == 'Volume':
                    # Build new opacity points for the slice in the same normalized range
                    new_pwf_points = []
                    for np in normalized_points:
                        new_val = slice_min + np['norm_x'] * slice_width
                        new_pwf_points.extend([new_val, np['opacity'], np['mid'], np['sharp']])
                    
                    # Create a NEW per-representation opacity transfer function for the slice.
                    try:
                        slice_pwf = simple.CreatePiecewiseFunction([0.0, 0.0, 1.0, 1.0])
                        slice_pwf.Points = new_pwf_points
                        rep.OpacityTransferFunction = 'Piecewise Function'
                        rep.ScalarOpacityFunction = slice_pwf
                    except Exception as e:
                        log.warn("Failed to set slice opacity function: {}", e)
        
        # Show scalar bar using the slice's own LUT (not parent's)
        slice_lut = rep.LookupTable
        if slice_lut:
            sb = simple.GetScalarBar(slice_lut, view)
            if sb:
                sb.Visibility = 1
        rep.SetScalarBarVisibility(view, True)
        
        # Track the slice's scalar bar visibility in per-source state
        try:
            d = dict(ctx.state.scalar_bar_per_source)
            d[slice_name] = True
            ctx.state.scalar_bar_per_source = d
        except Exception:
            ctx.state.scalar_bar_per_source = { slice_name: True }
    else:
        # Fallback: Apply default coloring if no parent settings available
        render_config.setup_default_view(slice_filter, view)
    
    simple.Render()
    if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
        try:
            ctx.ctrl.view_update()
        except Exception:
            pass


def extract_ghosts(ctx: Any, name: str):
    log.ui("Extract Ghosts for: {}", name)
    
    # Find the input proxy (Resample for sField if available, else MergedBlocks)
    input_proxy = None
    
    # Priority: MergedBlocks -> Standard
    # User requested to use MergedBlocks for ghost extraction
    merged_proxy = find_source_by_name(f"{name}.MergedBlocks")
    
    if merged_proxy:
        input_proxy = merged_proxy
    else:
        # Fallback to standard display proxy lookup
        input_proxy = get_display_proxy(ctx, name)
        
    if not input_proxy:
        log.warn("Could not find input proxy for ghost extraction on {}", name)
        return

    # Check for ghost array
    info = input_proxy.GetDataInformation()
    cinfo = info.GetCellDataInformation()
    if not cinfo.GetArrayInformation("vtkGhostType"):
        log.warn("No vtkGhostType array found in {}", name)
        # We could alert the user here, but for now just log
        # return # Optional: abort if no ghosts

    # Generate unique name
    base_name = name
    if base_name.endswith(".MergedBlocks"):
        base_name = base_name[:-13]
    
    ghost_count = 1
    while f"{base_name}.Ghosts{ghost_count}" in ctx.active_proxies:
        ghost_count += 1
    
    ghost_name = f"{base_name}.Ghosts{ghost_count}"
    
    # Create Extract Ghost Cells Filter
    # Note: By default ExtractGhostCells removes ghost cells. 
    # We set OutputGhostCells=1 to extract ONLY the ghost cells.
    extract_ghosts = simple.ExtractGhostCells(Input=input_proxy)
    
    try:
        extract_ghosts.OutputGhostCells = 1
    except AttributeError:
        log.warn("Could not set OutputGhostCells on ExtractGhostCells filter via attribute.")
        try:
            # Try setting via property name directly
            extract_ghosts.SetPropertyWithName("OutputGhostCells", 1)
            log.info("Set OutputGhostCells via SetPropertyWithName.")
        except Exception as e:
            log.warn("Failed to set OutputGhostCells via SetPropertyWithName: {}", e)
            try:
                # Debug: print available properties from ListProperties() which is more reliable for proxies
                log.debug("ExtractGhostCells ListProperties: {}", extract_ghosts.ListProperties())
            except:
                pass

    extract_ghosts.UpdatePipeline()
    
    # Register proxy
    ctx.active_proxies[ghost_name] = extract_ghosts
    
    # Add to pipeline items
    current_items = list(ctx.state.pipeline_items)
    current_items.append({"id": ghost_name, "name": f"Ghosts {ghost_count} ({base_name})", "visible": True})
    ctx.state.pipeline_items = current_items
    
    # Capture visibility of the source before showing the new filter
    # This prevents the new Show() from permanently hiding the input if it was visible,
    # but also prevents forcing it visible if it was hidden.
    view = simple.GetActiveView()
    source_visible = 0
    source_proxy = get_display_proxy(ctx, name)
    if source_proxy:
        s_rep = simple.GetRepresentation(source_proxy, view)
        if s_rep: source_visible = s_rep.Visibility

    # Show it
    rep = simple.Show(extract_ghosts, view)
    
    # Default to Volume representation for Ghosts (as per user request/trace)
    # Note: Volume rendering requires Point Data. If input is Cell Data (GhostType),
    # we might need to resample or rely on ParaView's internal conversion if supported.
    # However, the trace shows SetRepresentationType('Volume') working on ExtractGhostCells.
    rep.SetRepresentationType('Volume')

    # Color by RankID if available (preferred), otherwise GhostType
    info = extract_ghosts.GetDataInformation()
    cinfo = info.GetCellDataInformation()
    pinfo = info.GetPointDataInformation()
    
    color_array_name = None
    color_association = None
    
    # Prefer RankID (Point or Cell data)
    if pinfo and pinfo.GetArrayInformation("RankID"):
        color_array_name = "RankID"
        color_association = 'POINTS'
    elif cinfo and cinfo.GetArrayInformation("RankID"):
        color_array_name = "RankID"
        color_association = 'CELLS'
    # Fallback to GhostType
    elif cinfo and cinfo.GetArrayInformation("GhostType"):
        color_array_name = "GhostType"
        color_association = 'CELLS'
    elif cinfo and cinfo.GetArrayInformation("vtkGhostType"):
        color_array_name = "vtkGhostType"
        color_association = 'CELLS'
        
    if color_array_name and color_association:
        simple.ColorBy(rep, (color_association, color_array_name))
        
        # Rescale for Volume
        rep.RescaleTransferFunctionToDataRange(True, True)
        rep.SetScalarBarVisibility(view, True)
        
        # Set default opacity transfer function for ghosts: full opacity (1.0) everywhere
        # This means opacity = 1 at position 0% and opacity = 1 at position 100%
        try:
            lut = rep.LookupTable
            if lut:
                rgb_points = lut.RGBPoints
                if rgb_points and len(rgb_points) >= 4:
                    min_val = rgb_points[0]
                    max_val = rgb_points[-4]
                else:
                    min_val = 0.0
                    max_val = 1.0
                
                # Create opacity function with full opacity at both ends
                # Format: [x, y, midpoint, sharpness, ...]
                pwf = simple.GetOpacityTransferFunction(color_array_name)
                pwf.Points = [min_val, 0.6, 0.5, 0.0, max_val, 0.6, 0.5, 0.0]
                
                # Also set on representation if it supports it
                if hasattr(rep, 'ScalarOpacityFunction'):
                    rep.ScalarOpacityFunction = pwf
        except Exception as e:
            log.warn("Failed to set ghost opacity function: {}", e)
        
        # Debug: Check range
        if color_association == 'CELLS':
            array_info = cinfo.GetArrayInformation(color_array_name)
        else:
            array_info = pinfo.GetArrayInformation(color_array_name)
        if array_info:
            rng = array_info.GetComponentRange(0)
            log.debug("{} range: {}", color_array_name, rng)
            
        # Also list other available arrays for debugging
        log.debug("Available Cell Arrays: {}", [cinfo.GetArrayInformation(i).GetName() for i in range(cinfo.GetNumberOfArrays())])
    else:
        render_config.setup_default_view(extract_ghosts, view)
    
    # Set per-source rescale settings: disable symmetric rescale for ghosts
    # (RankID values are typically 0, 1, 2, ... and symmetric around 0 doesn't make sense)
    try:
        d = dict(ctx.state.rescale_settings_per_source)
        d[ghost_name] = {'auto': True, 'symmetric': False, 'min': 0.0, 'max': 1.0}
        ctx.state.rescale_settings_per_source = d
    except Exception:
        ctx.state.rescale_settings_per_source = {ghost_name: {'auto': True, 'symmetric': False, 'min': 0.0, 'max': 1.0}}
    
    # Restore original source visibility state
    if source_proxy:
        s_rep = simple.GetRepresentation(source_proxy, view)
        if s_rep: s_rep.Visibility = source_visible

    simple.Render()
    if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
        try:
            ctx.ctrl.view_update()
        except Exception:
            pass


def toggle_visibility(ctx: Any, name: str):
    log.ui("Toggle Visibility clicked: {}", name)
    
    items = ctx.state.pipeline_items
    new_items = []
    is_visible = True
    for item in items:
        if item['id'] == name:
            new_item = dict(item)
            new_item['visible'] = not new_item['visible']
            is_visible = new_item['visible']
            new_items.append(new_item)
        else:
            new_items.append(item)
    ctx.state.pipeline_items = new_items
    
    view = simple.GetActiveView()

    def _set_vis(proxy_name, vis):
        p = find_source_by_name(proxy_name)
        if p:
            rep = simple.GetRepresentation(p, view)
            if rep: 
                rep.Visibility = 1 if vis else 0
                # Toggle scalar bar if coloring is active
                ca = getattr(rep, 'ColorArrayName', None)
                if ca and len(ca) > 1 and ca[1]:
                    rep.SetScalarBarVisibility(view, vis)

    def _set_vis_proxy(proxy, vis):
        if proxy:
            rep = simple.GetRepresentation(proxy, view)
            if rep: 
                rep.Visibility = 1 if vis else 0
                # Toggle scalar bar if coloring is active
                ca = getattr(rep, 'ColorArrayName', None)
                if ca and len(ca) > 1 and ca[1]:
                    rep.SetScalarBarVisibility(view, vis)

    # Handle compound types
    if name.startswith("ippl_sField"):
        # If Resample proxy exists (Volume), toggle it and ensure raw data (Surface) is hidden
        # to prevent it from blocking the volume view.
        if find_source_by_name(f"{name}_Resample"):
            _set_vis(f"{name}_Resample", is_visible)
            _set_vis(f"{name}.MergedBlocks", False)
            if name in ctx.active_proxies:
                _set_vis_proxy(ctx.active_proxies[name], False)
        else:
            _set_vis(f"{name}.MergedBlocks", is_visible)
            # Also toggle base proxy if it exists in active_proxies
            if name in ctx.active_proxies:
                _set_vis_proxy(ctx.active_proxies[name], is_visible)
            
    elif name.startswith("ippl_vField"):
        _set_vis(f"{name}_Glyph", is_visible)
        _set_vis(f"{name}.Glyph", is_visible)
        # Also toggle base proxy (sometimes used for surface view)
        if name in ctx.active_proxies:
            _set_vis_proxy(ctx.active_proxies[name], is_visible)
            
    elif name.startswith("ippl_particles"):
        # Handle both the base particle source and the individual .bunch/.box components
        if name.endswith(".bunch") or name.endswith(".box"):
            # Direct component - just toggle that specific proxy
            proxy = ctx.active_proxies.get(name)
            if proxy:
                _set_vis_proxy(proxy, is_visible)
            else:
                _set_vis(name, is_visible)
        else:
            # Base particle source - toggle both bunch and box
            _set_vis(f"{name}.bunch", is_visible)
            _set_vis(f"{name}.box", is_visible)
        
    else:
        # Standard proxy
        display_proxy = ctx.active_proxies.get(name)
        if not display_proxy:
            display_proxy = find_source_by_name(name)
            
        if display_proxy:
            _set_vis_proxy(display_proxy, is_visible)
        else:
            log.warn("Could not find proxy for {} to toggle visibility", name)

    simple.Render()
    if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
        try:
            ctx.ctrl.view_update()
        except Exception as e:
            log.warn("view_update failed: {}. Disabling further updates and live mode.", e)
            ctx.view_update_enabled = False
            ctx.state.live_mode = False
            ctx.state.status_text = "Live disabled: transport error"


def focus_camera_on(ctx: Any, name: str):
    log.ui("Focus Camera clicked: {}", name)
    if name not in ctx.active_proxies: return
    proxy = ctx.active_proxies[name]
    render_config.reset_camera(simple.GetActiveView(), name, proxy)
    simple.Render()
    if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
        try:
            ctx.ctrl.view_update()
        except Exception as e:
            log.warn("view_update failed: {}. Disabling further updates.", e)
            ctx.view_update_enabled = False


def extract_scalar_field(ctx: Any, name: str):
    log.ui("Extract Scalar Field for: {}", name)
    
    # Find the input proxy (MergedBlocks for vField)
    input_proxy = None
    
    # If the user selected the MergedBlocks directly
    if name.endswith(".MergedBlocks"):
        input_proxy = find_source_by_name(name)
    else:
        # Try to find the associated MergedBlocks
        merged_proxy = find_source_by_name(f"{name}.MergedBlocks")
        if merged_proxy:
            input_proxy = merged_proxy
        else:
            # Fallback to the source itself (e.g. TrivialProducer)
            # We explicitly avoid get_display_proxy() here because for vField 
            # it returns the Glyph filter, but we want the field data.
            log.info("find source by name .MergeBlocks failed, source not catalyst extracted ...")
            input_proxy = find_source_by_name(name)
        
    if not input_proxy:
        log.warn("Could not find input proxy for scalar extraction on {}", name)
        return

    # Generate unique name
    base_name = name
    if base_name.endswith(".MergedBlocks"):
        base_name = base_name[:-13]
    
    resample_count = 1
    while f"{base_name}.Resample{resample_count}" in ctx.active_proxies:
        resample_count += 1
    
    resample_name = f"{base_name}.Resample{resample_count}"
    
    # Logic adapted from setup_scalar_field_view to determine bounds and dimensions
    base_proxy = simple.FindSource(base_name)
    if base_proxy:
        base_proxy.UpdatePipeline()
        base_info = base_proxy.GetDataInformation()
        
        input_proxy.UpdatePipeline()
        info = input_proxy.GetDataInformation()
        cinfo = info.GetCellDataInformation()
        ghost_info = cinfo.GetArrayInformation("vtkGhostType") if cinfo else None

        local_bounds = base_info.GetBounds()
        local_extent = base_info.GetExtent()
        global_bounds = info.GetBounds()

        nx = local_extent[1] - local_extent[0] + 1
        ny = local_extent[3] - local_extent[2] + 1
        nz = local_extent[5] - local_extent[4] + 1

        lx = local_bounds[1] - local_bounds[0]
        ly = local_bounds[3] - local_bounds[2]
        lz = local_bounds[5] - local_bounds[4]

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
        
        if ghost_info:
            log.debug("Ghost Present")
            cut_layers = 1
        else:
            log.debug("No Ghost Present")
            cut_layers = 0
            
        sampling_bounds = (
            global_bounds[0] + cut_layers * dx,
            global_bounds[1] - cut_layers * dx,
            global_bounds[2] + cut_layers * dy,
            global_bounds[3] - cut_layers * dy,
            global_bounds[4] + cut_layers * dz,
            global_bounds[5] - cut_layers * dz,
        )
        log.debug("ResampleToImage: Derived dims {} from extent {}, local bounds {} and global bounds {}", global_extent, local_extent, local_bounds, global_bounds)
        
        resample = simple.ResampleToImage(registrationName=f"{resample_name}_Internal", Input=input_proxy)
        resample.UseInputBounds = 0
        resample.SamplingBounds = sampling_bounds
        resample.SamplingDimensions = global_extent
    else:
        log.warn("Base proxy {} not found. Using default resampling.", base_name)
        info = input_proxy.GetDataInformation()
        bounds = info.GetBounds()
        dx = bounds[1] - bounds[0]
        dy = bounds[3] - bounds[2]
        dz = bounds[5] - bounds[4]
        max_dim = 100
        max_len = max(dx, dy, dz)
        if max_len > 0:
            nx = int(max_dim * dx / max_len)
            ny = int(max_dim * dy / max_len)
            nz = int(max_dim * dz / max_len)
        else:
            nx, ny, nz = 100, 100, 100
        resample = simple.ResampleToImage(registrationName=f"{resample_name}_Internal", Input=input_proxy)
        resample.UseInputBounds = 1
        resample.SamplingDimensions = [nx, ny, nz]
    
    resample.UpdatePipeline()
    
    # Rename array using Calculator to force separate LUT
    final_proxy = resample
    
    info = resample.GetDataInformation()
    pd = info.GetPointDataInformation()
    if pd.GetNumberOfArrays() > 0:
        array_name = pd.GetArrayInformation(0).GetName()
        new_array_name = f"{array_name}_extracted"
        
        calc = simple.Calculator(registrationName=resample_name, Input=resample)
        calc.AttributeType = 'Point Data'
        calc.ResultArrayName = new_array_name
        calc.Function = f'"{array_name}"'
        calc.UpdatePipeline()
        final_proxy = calc
    else:
        # Fallback: rename internal to public if no array to rename
        simple.RenameSource(resample_name, resample)
        final_proxy = resample

    # Register proxy
    ctx.active_proxies[resample_name] = final_proxy
    
    # Add to pipeline items
    current_items = list(ctx.state.pipeline_items)
    current_items.append({"id": resample_name, "name": f"Resample {resample_count} ({base_name})", "visible": True})
    ctx.state.pipeline_items = current_items
    
    # Capture visibility of the source (Glyphs)
    view = simple.GetActiveView()
    source_visible = 0
    source_proxy = get_display_proxy(ctx, name)
    if source_proxy:
        s_rep = simple.GetRepresentation(source_proxy, view)
        if s_rep: source_visible = s_rep.Visibility

    # Show it
    rep = simple.Show(final_proxy, view)
    rep.SetRepresentationType('Volume')
    
    # Explicitly configure coloring for Volume
    info = final_proxy.GetDataInformation()
    pd = info.GetPointDataInformation()
    if pd.GetNumberOfArrays() > 0:
        # Prefer the renamed array if available
        target_array = new_array_name if final_proxy == calc else pd.GetArrayInformation(0).GetName()
        log.info("Coloring Resample Volume by {} (Magnitude)", target_array)
        simple.ColorBy(rep, ('POINTS', target_array, 'Magnitude'))
        rep.RescaleTransferFunctionToDataRange(True, True)
        rep.SetScalarBarVisibility(view, True)
    else:
        render_config.setup_default_view(final_proxy, view)
    
    # Restore original source visibility state
    if source_proxy:
        s_rep = simple.GetRepresentation(source_proxy, view)
        if s_rep: s_rep.Visibility = source_visible

    simple.Render()
    if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
        try:
            ctx.ctrl.view_update()
        except Exception:
            pass
