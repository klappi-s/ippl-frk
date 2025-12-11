# Pipeline interactions moved from trame_vis_app.py
from typing import Any
from paraview import simple

from catalystSubroutines import find_source_by_name
try:
    from . import trame_render_config as render_config
except ImportError:
    import trame_render_config as render_config


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
    print(f"[UI] Remove Source clicked: {name}")
    
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
        print(f"Deleting proxy object for {name}")
        try:
            simple.Delete(p)
        except Exception as e:
            print(f"[WARN] Failed to delete proxy {name}: {e}")
    
    # Remove from active_proxies if present
    if name in ctx.active_proxies:
        del ctx.active_proxies[name]

    # Check for split particle sources cleanup
    if name.endswith(".bunch"):
        parent = name[:-6]
        sibling = f"{parent}.box"
        # If sibling is not active, remove parent key to allow re-adding
        if sibling not in ctx.active_proxies and parent in ctx.active_proxies:
            print(f"Cleaning up parent key {parent} from active_proxies")
            del ctx.active_proxies[parent]
    elif name.endswith(".box"):
        parent = name[:-4]
        sibling = f"{parent}.bunch"
        # If sibling is not active, remove parent key to allow re-adding
        if sibling not in ctx.active_proxies and parent in ctx.active_proxies:
            print(f"Cleaning up parent key {parent} from active_proxies")
            del ctx.active_proxies[parent]
        
    new_items = [item for item in ctx.state.pipeline_items if item['id'] != name]
    ctx.state.pipeline_items = new_items
    if not ctx.active_proxies:
        ctx.state.has_data = False
    simple.Render()
    if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
        try:
            ctx.ctrl.view_update()
        except Exception as e:
            print(f"[WARN] view_update failed: {e}. Disabling further updates and live mode.")
            ctx.view_update_enabled = False
            ctx.state.live_mode = False
            ctx.state.status_text = "Live disabled: transport error"


def extract_slice(ctx: Any, name: str):
    print(f"[UI] Extract Slice for: {name}")
    
    # Find the input proxy (MergedBlocks for sField)
    input_proxy = None
    if name.startswith("ippl_sField"):
        input_proxy = find_source_by_name(f"{name}.MergedBlocks")
    else:
        # Fallback for other types if we ever support them
        input_proxy = get_display_proxy(ctx, name)
        
    if not input_proxy:
        print(f"[WARN] Could not find input proxy for slice extraction on {name}")
        return

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
    view = simple.GetActiveView()
    rep = simple.Show(slice_filter, view)
    rep.SetRepresentationType('Surface')
    
    # Apply default coloring if possible
    render_config.setup_default_view(slice_filter, view)
    
    simple.Render()
    if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
        try:
            ctx.ctrl.view_update()
        except Exception:
            pass


def extract_ghosts(ctx: Any, name: str):
    print(f"[UI] Extract Ghosts for: {name}")
    
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
        print(f"[WARN] Could not find input proxy for ghost extraction on {name}")
        return

    # Check for ghost array
    info = input_proxy.GetDataInformation()
    cinfo = info.GetCellDataInformation()
    if not cinfo.GetArrayInformation("vtkGhostType"):
        print(f"[WARN] No vtkGhostType array found in {name}")
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
        print(f"[WARN] Could not set OutputGhostCells on ExtractGhostCells filter via attribute.")
        try:
            # Try setting via property name directly
            extract_ghosts.SetPropertyWithName("OutputGhostCells", 1)
            print(f"[INFO] Set OutputGhostCells via SetPropertyWithName.")
        except Exception as e:
            print(f"[WARN] Failed to set OutputGhostCells via SetPropertyWithName: {e}")
            try:
                # Debug: print available properties from ListProperties() which is more reliable for proxies
                print(f"[DEBUG] ExtractGhostCells ListProperties: {extract_ghosts.ListProperties()}")
            except:
                pass

    extract_ghosts.UpdatePipeline()
    
    # Register proxy
    ctx.active_proxies[ghost_name] = extract_ghosts
    
    # Add to pipeline items
    current_items = list(ctx.state.pipeline_items)
    current_items.append({"id": ghost_name, "name": f"Ghosts {ghost_count} ({base_name})", "visible": True})
    ctx.state.pipeline_items = current_items
    
    # Show it
    view = simple.GetActiveView()
    rep = simple.Show(extract_ghosts, view)
    
    # Default to Volume representation for Ghosts (as per user request/trace)
    # Note: Volume rendering requires Point Data. If input is Cell Data (GhostType),
    # we might need to resample or rely on ParaView's internal conversion if supported.
    # However, the trace shows SetRepresentationType('Volume') working on ExtractGhostCells.
    rep.SetRepresentationType('Volume')

    # Color by GhostType if available
    info = extract_ghosts.GetDataInformation()
    cinfo = info.GetCellDataInformation()
    ghost_array_name = None
    if cinfo.GetArrayInformation("GhostType"):
        ghost_array_name = "GhostType"
    elif cinfo.GetArrayInformation("vtkGhostType"):
        ghost_array_name = "vtkGhostType"
        
    if ghost_array_name:
        # Trace uses ColorBy with CELLS association
        simple.ColorBy(rep, ('CELLS', ghost_array_name))
        
        # Rescale for Volume
        rep.RescaleTransferFunctionToDataRange(True, True)
        rep.SetScalarBarVisibility(view, True)
        
        # Debug: Check range
        array_info = cinfo.GetArrayInformation(ghost_array_name)
        if array_info:
            rng = array_info.GetComponentRange(0)
            print(f"[DEBUG] {ghost_array_name} range: {rng}")
            
        # Also list other available arrays for debugging
        print(f"[DEBUG] Available Cell Arrays: {[cinfo.GetArrayInformation(i).GetName() for i in range(cinfo.GetNumberOfArrays())]}")
    else:
        render_config.setup_default_view(extract_ghosts, view)
    
    # Ensure original source remains visible (in case Show() hid it)
    original_proxy = get_display_proxy(ctx, name)
    if original_proxy and original_proxy != extract_ghosts:
        orig_rep = simple.Show(original_proxy, view)
        orig_rep.Visibility = 1

    simple.Render()
    if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
        try:
            ctx.ctrl.view_update()
        except Exception:
            pass


def toggle_visibility(ctx: Any, name: str):
    print(f"[UI] Toggle Visibility clicked: {name}")
    
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
            if rep: rep.Visibility = 1 if vis else 0

    def _set_vis_proxy(proxy, vis):
        if proxy:
            rep = simple.GetRepresentation(proxy, view)
            if rep: rep.Visibility = 1 if vis else 0

    # Handle compound types
    if name.startswith("ippl_sField"):
        _set_vis(f"{name}_Resample", is_visible)
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
            print(f"[WARN] Could not find proxy for {name} to toggle visibility")

    simple.Render()
    if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
        try:
            ctx.ctrl.view_update()
        except Exception as e:
            print(f"[WARN] view_update failed: {e}. Disabling further updates and live mode.")
            ctx.view_update_enabled = False
            ctx.state.live_mode = False
            ctx.state.status_text = "Live disabled: transport error"


def focus_camera_on(ctx: Any, name: str):
    print(f"[UI] Focus Camera clicked: {name}")
    if name not in ctx.active_proxies: return
    proxy = ctx.active_proxies[name]
    render_config.reset_camera(simple.GetActiveView(), name, proxy)
    simple.Render()
    if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
        try:
            ctx.ctrl.view_update()
        except Exception as e:
            print(f"[WARN] view_update failed: {e}. Disabling further updates.")
            ctx.view_update_enabled = False
