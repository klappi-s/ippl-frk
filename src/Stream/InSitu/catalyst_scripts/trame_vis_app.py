import asyncio
import time
from trame.app import get_server
from trame.ui.vuetify3 import SinglePageLayout
from trame.widgets import vuetify3, vtk

from paraview import simple
from paraview import live

import trame_render_config as render_config
import trame_steering_config as steering_config
from catalystSubroutines import find_source_by_name, get_available_extract_names
import trame_steering_config as steering_config

# -----------------------------------------------------------------------------
# Trame Setup
# -----------------------------------------------------------------------------
server = get_server()
state = server.state
ctrl = server.controller

# Global variables
catalyst_link = None
active_proxies = {} # Map: name -> proxy object 
steerable_proxies = []

view_update_enabled = True
last_view_update_ts = 0.0

# -----------------------------------------------------------------------------
# Helper Functions
# -----------------------------------------------------------------------------

# Moved to catalystSubroutines.py:
# - find_source_by_name
# - get_available_extract_names

def remove_proxy(name):
    global active_proxies, view_update_enabled
    if name not in active_proxies: return
    
    print(f"Removing proxy: {name}")
    
    # 1. Clean up ParaView objects
    sel = name
    if sel.startswith("ippl_sField"):
         p1 = find_source_by_name(f"{sel}_Resample")
         p2 = find_source_by_name(sel)
         p3 = find_source_by_name(f"{sel}.MergedBlocks")
         if p1: simple.Delete(p1)
         if p2: simple.Delete(p2)
         if p3: simple.Delete(p3)
    elif sel.startswith("ippl_vField"):
         p1 = find_source_by_name(f"{sel}_Glyph")
         if p1: simple.Delete(p1)
         p2 = find_source_by_name(f"{sel}.Glyph")
         if p2: simple.Delete(p2)
         
         p = find_source_by_name(sel)
         if p: simple.Delete(p)
    elif sel.startswith("ippl_particles"):
         # Clean up filters created in render_config
         # Note: These names might conflict if multiple particle sources are loaded.
         # Ideally, render_config should use unique names.
         # For now, we assume one particle source or that names are unique enough.
         
         # Clean up box
         p_box = find_source_by_name(f"{sel}.box")
         if p_box: simple.Delete(p_box)
         
         # Clean up bunch
         p_bunch = find_source_by_name(f"{sel}.bunch")
         if p_bunch: simple.Delete(p_bunch)
         
         # Clean up container
         p_container = find_source_by_name(sel)
         if p_container: simple.Delete(p_container)
    else:
         p = find_source_by_name(sel)
         if p: simple.Delete(p)

    # 2. Remove from active_proxies
    del active_proxies[name]
    
    # 3. Update UI list
    new_items = [item for item in state.pipeline_items if item['id'] != name]
    state.pipeline_items = new_items
    
    if not active_proxies:
        state.has_data = False
        
    simple.Render()
    if hasattr(ctrl, 'view_update') and view_update_enabled:
        try:
            ctrl.view_update()
        except Exception as e:
            print(f"[WARN] view_update failed: {e}. Disabling further updates and live mode.")
            view_update_enabled = False
            state.live_mode = False
            state.status_text = "Live disabled: transport error"

def toggle_visibility(name):
    global view_update_enabled
    if name not in active_proxies: return
    
    # Find item in list to toggle state
    # Create new list with new dict for the modified item to ensure reactivity
    items = state.pipeline_items
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
            
    state.pipeline_items = new_items
    
    # Apply to ParaView
    # We need to find the representation. 
    # Since we don't store the rep, we iterate views or use FindSource logic.
    # A robust way is to find the "display" proxy (Resample, Glyph, etc)
    
    display_proxy = None
    if name.startswith("ippl_sField"):
        display_proxy = find_source_by_name(f"{name}_Resample")
        if not display_proxy: display_proxy = find_source_by_name(f"{name}.MergedBlocks")
    elif name.startswith("ippl_vField"):
        display_proxy = find_source_by_name(f"{name}_Glyph") # Local
        if not display_proxy: display_proxy = find_source_by_name(f"{name}.Glyph") # Extracted
        if not display_proxy: display_proxy = find_source_by_name(name)
    elif name.startswith("ippl_particles"):
        # Particles have multiple reps (particles + box)
        # We toggle both
        p_bunch = find_source_by_name(f"{name}.bunch")
        if p_bunch:
            rep = simple.GetRepresentation(p_bunch, simple.GetActiveView())
            if rep: rep.Visibility = 1 if is_visible else 0
            
        p_box = find_source_by_name(f"{name}.box")
        if p_box:
            rep = simple.GetRepresentation(p_box, simple.GetActiveView())
            if rep: rep.Visibility = 1 if is_visible else 0
        
        simple.Render()
        if hasattr(ctrl, 'view_update') and view_update_enabled:
            try:
                ctrl.view_update()
            except Exception as e:
                print(f"[WARN] view_update failed: {e}. Disabling further updates and live mode.")
                view_update_enabled = False
                state.live_mode = False
                state.status_text = "Live disabled: transport error"
        return

    if display_proxy:
        rep = simple.GetRepresentation(display_proxy, simple.GetActiveView())
        if rep:
            rep.Visibility = 1 if is_visible else 0
            simple.Render()
            if hasattr(ctrl, 'view_update') and view_update_enabled:
                try:
                    ctrl.view_update()
                except Exception as e:
                    print(f"[WARN] view_update failed: {e}. Disabling further updates and live mode.")
                    view_update_enabled = False
                    state.live_mode = False
                    state.status_text = "Live disabled: transport error"

def focus_camera_on(name):
    if name not in active_proxies: return
    proxy = active_proxies[name]
    render_config.reset_camera(simple.GetActiveView(), name, proxy)
    simple.Render()
    if hasattr(ctrl, 'view_update') and view_update_enabled:
        try:
            ctrl.view_update()
        except Exception as e:
            print(f"[WARN] view_update failed: {e}. Disabling further updates.")
            view_update_enabled = False

def get_display_proxy(name):
    if name.startswith("ippl_sField"):
        p = find_source_by_name(f"{name}_Resample")
        if not p: p = find_source_by_name(f"{name}.MergedBlocks")
        return p
    elif name.startswith("ippl_vField"):
        p = find_source_by_name(f"{name}_Glyph")
        if not p: p = find_source_by_name(f"{name}.Glyph")
        if not p: p = find_source_by_name(name)
        return p
    elif name.startswith("ippl_particles"):
        return find_source_by_name(f"{name}.bunch")
    else:
        return find_source_by_name(name)

def open_edit_dialog(name):
    state.editing_source = name
    state.show_edit_dialog = True
    
    proxy = get_display_proxy(name)
    if not proxy:
        state.color_arrays = []
        state.current_color_array = None
        return

    # Get Representation
    rep = simple.GetRepresentation(proxy, simple.GetActiveView())
    if not rep:
        state.color_arrays = []
        state.current_color_array = None
        return

    # Get Arrays
    info = proxy.GetDataInformation()
    arrays = []
    
    # Point Data
    pd = info.GetPointDataInformation()
    for i in range(pd.GetNumberOfArrays()):
        ai = pd.GetArrayInformation(i)
        arrays.append({'title': f"{ai.GetName()} (Points)", 'value': f"POINTS:{ai.GetName()}"})
        
    # Cell Data
    cd = info.GetCellDataInformation()
    for i in range(cd.GetNumberOfArrays()):
        ai = cd.GetArrayInformation(i)
        arrays.append({'title': f"{ai.GetName()} (Cells)", 'value': f"CELLS:{ai.GetName()}"})
        
    # Add Solid Color option
    arrays.insert(0, {'title': 'Solid Color', 'value': 'SOLID'})
    
    state.color_arrays = arrays
    
    # Get Current Color
    # ColorArrayName is usually ['POINTS', 'name'] or ['CELLS', 'name'] or [None, '']
    ca = rep.ColorArrayName
    if ca and len(ca) > 1 and ca[0] and ca[1]:
        state.current_color_array = f"{ca[0]}:{ca[1]}"
    else:
        state.current_color_array = 'SOLID'

def update_color_by(value):
    name = state.editing_source
    if not name: return
    
    proxy = get_display_proxy(name)
    if not proxy: return
    
    rep = simple.GetRepresentation(proxy, simple.GetActiveView())
    if not rep: return
    # Hide any existing scalar bar tied to previous array
    prev_ca = getattr(rep, 'ColorArrayName', None)
    if prev_ca and isinstance(prev_ca, list) and len(prev_ca) >= 2 and prev_ca[0] and prev_ca[1]:
        try:
            prev_lut = simple.GetColorTransferFunction(prev_ca[1])
            prev_sb = simple.GetScalarBar(prev_lut, simple.GetActiveView())
            if prev_sb:
                prev_sb.Visibility = 0
        except Exception:
            pass
    # Also ensure rep turns off previous scalar bar
    try:
        rep.SetScalarBarVisibility(simple.GetActiveView(), False)
    except Exception:
        pass

    if value == 'SOLID':
        rep.ColorArrayName = [None, '']
        rep.SetScalarBarVisibility(simple.GetActiveView(), False)
    else:
        # value is "ASSOC:NAME"
        parts = value.split(':')
        if len(parts) < 2: return
        assoc = parts[0]
        array_name = parts[1]
        
        rep.ColorArrayName = [assoc, array_name]
        
        # Rescale LUT
        lut = simple.GetColorTransferFunction(array_name)
        
        # Get Range
        info = proxy.GetDataInformation()
        if assoc == 'POINTS':
            dinfo = info.GetPointDataInformation()
        else:
            dinfo = info.GetCellDataInformation()
            
        ai = dinfo.GetArrayInformation(array_name)
        if ai:
            r = ai.GetComponentRange(-1) # Magnitude
            if r[1] > r[0]:
                lut.RescaleTransferFunction(r[0], r[1])
                
        rep.LookupTable = lut
        
        # Update Scalar Bar
        sb = simple.GetScalarBar(lut, simple.GetActiveView())
        sb.Title = array_name
        sb.ComponentTitle = 'Magnitude'
        sb.Visibility = 1
        rep.SetScalarBarVisibility(simple.GetActiveView(), True)

    simple.Render()
    if hasattr(ctrl, 'view_update') and view_update_enabled:
        try:
            ctrl.view_update()
        except Exception as e:
            print(f"[WARN] view_update failed: {e}. Disabling further updates.")
            # We don't disable global view_update_enabled here to be less aggressive in the UI interaction
            pass

def apply_and_close():
    update_color_by(state.current_color_array)
    state.show_edit_dialog = False

# Moved to trame_steering_config.py:
# - load_steerable_proxies
# - get_steerable_proxy
# - update_steering_parameter
# - parse_steerable_parameters

def apply_steering():
    """Apply the current steering parameters."""
    print("[DEBUG] apply_steering called")
    for proxy_def in steerable_proxies:
        proxy_name = proxy_def['name']
        proxy_safe_name = proxy_def['safe_name']
        
        # Flatten params for this proxy
        flat_params = []
        def collect_params(items):
            for item in items:
                if item['item_type'] == 'group':
                    collect_params(item['children'])
                else:
                    flat_params.append(item)
        collect_params(proxy_def['children'])
        
        # Determine number of items (rows)
        # For scalar proxy, it's 1. For array proxy, it's in state.
        num_rows = 1
        if proxy_def['type'] == 'array':
            try:
                num_rows = state[f"count_{proxy_safe_name}"]
            except Exception:
                num_rows = 0
            
        for param in flat_params:
            name = param['name']
            safe_name = param['safe_name']
            
            try:
                # Collect values for all rows
                all_values = []
                for row_idx in range(num_rows):
                    if param['num_elements'] > 1:
                        # Vector
                        row_val = []
                        for i in range(param['num_elements']):
                            key = f"steer_{safe_name}_{row_idx}_{i}"
                            try:
                                v = state[key]
                            except Exception:
                                v = param['default'][i] if i < len(param['default']) else 0
                            # Replace None with default 0 or parsed default
                            if v is None:
                                v = param['default'][i] if i < len(param['default']) else 0
                            row_val.append(v)
                        all_values.append(row_val)
                    else:
                        # Scalar
                        key = f"steer_{safe_name}_{row_idx}"
                        try:
                            v = state[key]
                        except Exception:
                            v = param['default'][0] if param['default'] else 0
                        if v is None:
                            v = param['default'][0] if param['default'] else 0
                        all_values.append(v)
                
                steering_config.update_steering_parameter(catalyst_link, proxy_name, name, all_values)
                
            except Exception as e:
                print(f"[ERROR] Error applying parameter {name}: {e}")
                import traceback
                traceback.print_exc()

def connect_to_catalyst():
    global catalyst_link
    cat_host = "localhost"
    cat_port = 22222
    
    try:
        # Preload definitions to avoid errors during handshake (best effort)
        steering_config.preload_steerable_proxies()

        print(f"Connecting to {cat_host}:{cat_port}...")
        catalyst_link = live.ConnectToCatalyst(ds_host=cat_host, ds_port=cat_port)
        
        if catalyst_link:
            state.connected = True
            state.status_text = f"Connected"
            
            # Load steerable proxies directly into insitu proxy manager
            try:
                ipm = catalyst_link.GetInsituProxyManager()
                if ipm:
                    print("[DEBUG] connect_to_catalyst: Loading proxies into insitu PM")
                    steering_config.load_steerable_proxies(proxy_manager=ipm)
                    state.insitu_proxies_loaded = True
            except Exception as e:
                print(f"[DEBUG] connect_to_catalyst: Failed to load insitu proxies: {e}")
            
            ctrl.resume_polling()
            
            # Auto-scan list on connect, but don't auto-select
            scan_sources_only()
        else:
            state.status_text = "Connection returned None"
            
    except Exception as e:
        print(f"[Error] Connect: {e}")
        state.status_text = f"Error: {e}"

def scan_sources_only():
    if not catalyst_link: return
    print("Scanning for available channels...")
    
    msgs_processed = 0
    while live.ProcessServerNotifications():
        msgs_processed += 1
        if msgs_processed > 50: break
    
    names = get_available_extract_names(catalyst_link)
    # Exclude steering control channels from visualization list
    names = [n for n in names if not (n.startswith("Steering"))]
    
    if names:
        print(f"Scan complete. Found: {names}")
        state.available_sources = names
        # Explicitly do NOT touch state.selected_source here
    else:
        print("Scan returned empty.")


def toggle_connection():
    if state.connected:
        global catalyst_link
        print("Disconnecting from Catalyst...")
        if state.has_data:
            reset_visualization()
        
        catalyst_link = None
        state.connected = False
        state.status_text = "Disconnected"
        state.available_sources = []
        state.selected_source = None
    else:
        connect_to_catalyst()


def search_and_select_best():
    current_input = state.selected_source
    available = state.available_sources
    
    if not available:
        scan_sources_only()
        available = state.available_sources
    
    # If still no data or NO INPUT, do nothing.
    # This fixes the issue of auto-selecting the first alphabetical item.
    if not available or not current_input: 
        print("Search skipped: No input provided or no sources found.")
        return

    # 1. Exact Match
    if current_input in available:
        print(f"Exact match found: {current_input}. Initializing...")
        extract_data()
        return 
        
    # 2. Case-insensitive substring match
    best_match = None
    for name in available:
        if current_input.lower() in name.lower():
            best_match = name
            break 
    
    if best_match:
        print(f"Auto-switching selection to: {best_match}")
        state.selected_source = best_match
    else:
        print(f"No match found for '{current_input}'")


def extract_data():
    global active_proxies, view_update_enabled
    if not catalyst_link: return
    
    channel_name = state.selected_source 
    if not channel_name: return

    if channel_name in active_proxies:
         print(f"Source {channel_name} is already active.")
         return

    # Determine proxies to extract
    proxies_to_extract = [channel_name]
    if channel_name.startswith("ippl_sField"):
        proxies_to_extract.append(f"{channel_name}.MergedBlocks")
    elif channel_name.startswith("ippl_vField"):
        proxies_to_extract.append(f"{channel_name}.Glyph")
    elif channel_name.startswith("ippl_particles"):
        proxies_to_extract.append(f"{channel_name}.bunch")
        proxies_to_extract.append(f"{channel_name}.box")

    print(f"Requesting {proxies_to_extract}...")
    
    try:
        for p_name in proxies_to_extract:
            live.ExtractCatalystData(catalyst_link, p_name)
    except Exception as e:
        print(f"Extract call failed: {e}")
        return
    
    # Determine primary proxy to wait for
    primary_proxy_name = channel_name
    if channel_name.startswith("ippl_sField"):
        primary_proxy_name = f"{channel_name}.MergedBlocks"
    elif channel_name.startswith("ippl_particles"):
        primary_proxy_name = f"{channel_name}.bunch"

    # Wait Loop
    found_data = False
    new_proxy = None
    
    for i in range(50):
        live.ProcessServerNotifications()
        
        # Check the primary proxy
        p_proxy = find_source_by_name(primary_proxy_name)
        
        if p_proxy:
            try:
                p_proxy.UpdatePipeline()
                info = p_proxy.GetDataInformation()
                if info.GetNumberOfPoints() > 0 or info.GetNumberOfCells() > 0:
                    found_data = True
                    new_proxy = p_proxy
                    break
            except:
                pass
        time.sleep(0.1)

    if found_data and new_proxy:
        # Add to active proxies
        active_proxies[channel_name] = new_proxy
        
        # Update UI list
        current_items = list(state.pipeline_items)
        current_items.append({
            "id": channel_name,
            "name": channel_name,
            "visible": True
        })
        state.pipeline_items = current_items
        
        # Get the active view
        view = simple.GetActiveView()
        
        # --- Dispatch based on name ---
        if channel_name.startswith("ippl_particles"):
            print(f"Applying Custom Particle Render for {channel_name}...")
            render_config.setup_particle_view(new_proxy, view, channel_name)
        elif channel_name.startswith("ippl_sField"):
            print(f"Applying Scalar Field Render for {channel_name}...")
            # Use MergedBlocks as source
            merged_proxy = find_source_by_name(f"{channel_name}.MergedBlocks")
            if merged_proxy:
                render_config.setup_scalar_field_view(merged_proxy, view, channel_name)
        elif channel_name.startswith("ippl_vField"):
            print(f"Applying Vector Field Render for {channel_name}...")
            glyph_proxy = find_source_by_name(f"{channel_name}.Glyph")
            if glyph_proxy:
                print(f"Found extracted Glyph filter: {channel_name}.Glyph")
                render_config.setup_vector_field_view(glyph_proxy, view, channel_name, is_extracted=True)
            else:
                print(f"Extracted Glyph filter not found. Creating local Glyph filter.")
                render_config.setup_vector_field_view(new_proxy, view, channel_name, is_extracted=False)
        else:
            print(f"Applying Default Render for {channel_name}...")
            render_config.setup_default_view(new_proxy, view)

        # Trigger update
        render_config.reset_camera(simple.GetActiveView(), channel_name, new_proxy)
        simple.Render()
        if hasattr(ctrl, 'view_update') and view_update_enabled:
            try:
                ctrl.view_update()
            except Exception as e:
                print(f"[WARN] view_update failed: {e}. Disabling further updates and live mode.")
                view_update_enabled = False
                state.live_mode = False
                state.status_text = "Live disabled: transport error"
            
        state.has_data = True
        state.selected_source = None # Clear search bar on success
        print(f"Extraction successful: {channel_name}")


def reset_visualization():
    global active_proxies, view_update_enabled
    state.live_mode = False
    state.has_data = False
    
    # Hide all scalar bars
    view = simple.GetActiveView()
    if view:
        for rep in view.Representations:
            try:
                rep.SetScalarBarVisibility(view, False)
            except:
                pass

    # Iterate over a copy of keys since we modify the dict
    for name in list(active_proxies.keys()):
        remove_proxy(name)
    
    active_proxies = {}
    state.pipeline_items = []
        
    simple.Render()
    if hasattr(ctrl, 'view_update') and view_update_enabled:
        try:
            ctrl.view_update()
        except Exception as e:
            print(f"[WARN] view_update failed: {e}. Disabling further updates and live mode.")
            view_update_enabled = False
            state.live_mode = False
            state.status_text = "Live disabled: transport error"

# Removed reset_camera function as it is now in trame_render_config.py

# ... (Polling and Steering) ...
async def poll_catalyst():
    while True:
        if catalyst_link:
            try:
                live.ProcessServerNotifications()
                if state.live_mode and state.has_data:
                    
                    # Iterate over all active proxies
                    for sel, proxy in active_proxies.items():
                        
                        # --- CALL UPDATE LOGIC ---
                        if sel.startswith("ippl_sField"):
                            # Update base proxy
                            if proxy: proxy.UpdatePipeline()
                            
                            # Update MergedBlocks
                            merged_proxy = find_source_by_name(f"{sel}.MergedBlocks")
                            if merged_proxy: merged_proxy.UpdatePipeline()

                            # Update local filter if exists
                            resample_proxy = find_source_by_name(f"{sel}_Resample")
                            if resample_proxy:
                                resample_proxy.UpdatePipeline()
                                render_config.update_scalar_field_view(resample_proxy, simple.GetActiveView())
                                
                        elif sel.startswith("ippl_vField"):
                            if proxy: proxy.UpdatePipeline()
                            
                            # Update extracted Glyph if exists
                            glyph_proxy = find_source_by_name(f"{sel}.Glyph")
                            if glyph_proxy:
                                glyph_proxy.UpdatePipeline()
                                render_config.update_vector_field_view(glyph_proxy, simple.GetActiveView())
                            else:
                                # Update local Glyph if exists
                                glyph_proxy = find_source_by_name(f"{sel}_Glyph")
                                if glyph_proxy:
                                    glyph_proxy.UpdatePipeline()
                                    render_config.update_vector_field_view(glyph_proxy, simple.GetActiveView())

                        else:
                            if proxy: 
                                proxy.UpdatePipeline()
                                if sel.startswith("ippl_particles"):
                                    # Update box as well
                                    box_proxy = find_source_by_name(f"{sel}.box")
                                    if box_proxy: box_proxy.UpdatePipeline()
                                    
                                    render_config.update_particle_view(proxy, simple.GetActiveView())
                    
                    simple.Render()
                    # Throttle view updates
                    global last_view_update_ts, view_update_enabled
                    now = time.time()
                    if hasattr(ctrl, 'view_update') and view_update_enabled and state.live_mode and (now - last_view_update_ts) > 0.3:
                        try:
                            ctrl.view_update()
                            last_view_update_ts = now
                        except Exception as e:
                            print(f"[WARN] view_update failed during polling: {e}. Disabling further updates and live mode.")
                            view_update_enabled = False
                            state.live_mode = False
                            state.status_text = "Live disabled: transport error"
            except Exception:
                pass
        await asyncio.sleep(0.1)


        
def pause_sim():
    if catalyst_link: live.PauseCatalyst(catalyst_link, pause=True); state.sim_paused = True 
def unpause_sim():
    if catalyst_link: live.PauseCatalyst(catalyst_link, pause=False); state.sim_paused = False 
def toggle_simulation():
    try:
        if catalyst_link and catalyst_link.GetProperty('SimulationPaused').GetElement(0): unpause_sim()
        else: pause_sim()
    except: pass

def toggle_axes_grid():
    view = simple.GetActiveView()
    if not view: return
    
    # Toggle state
    new_val = 0 if view.AxesGrid.Visibility else 1
    view.AxesGrid.Visibility = new_val
    state.axes_grid_visible = (new_val == 1)
    
    simple.Render()
    if hasattr(ctrl, 'view_update') and view_update_enabled:
        try:
            ctrl.view_update()
        except Exception as e:
            print(f"[WARN] view_update failed: {e}. Disabling further updates and live mode.")
            view_update_enabled = False
            state.live_mode = False
            state.status_text = "Live disabled: transport error"

# Initialize steerable proxies
steerable_proxies = steering_config.parse_steerable_parameters()

# -----------------------------------------------------------------------------
# GUI Layout
# -----------------------------------------------------------------------------
with SinglePageLayout(server) as layout:
    layout.title.set_text("Catalyst Control Center")

    # --- DRAWER (Pipeline Browser + Steering) ---
    with vuetify3.VNavigationDrawer(width=400, permanent=True):
        with vuetify3.VRow(classes="fill-height ma-0", no_gutters=True):
            # --- LEFT COLUMN: CONTENT ---
            with vuetify3.VCol(classes="d-flex flex-column pa-0", style="height: 100%; width: calc(100% - 24px);"):
                
                # Top Half: Pipeline Browser
                with vuetify3.VContainer(classes="pa-0 d-flex flex-column", style=("`height: calc(100% - ${split_pos}%); border-bottom: 1px solid #555; transition: height 0.1s;`",)):
                    vuetify3.VListSubheader("Pipeline Browser")
                    vuetify3.VDivider()
                    
                    with vuetify3.VList(lines="one", density="compact", style="overflow-y: auto;"):
                        with vuetify3.VListItem(
                            v_for="(item, i) in pipeline_items",
                            key="item.id",
                            value="item.id",
                        ):
                            # Item Title
                            vuetify3.VListItemTitle("{{ item.name }}")
                            
                            # Actions
                            with vuetify3.VListItemAction(end=True):
                                with vuetify3.VSheet(classes="d-flex align-center bg-transparent"):
                                     # Visibility Toggle
                                     with vuetify3.VBtn(
                                        icon=True, 
                                        variant="text", 
                                        density="compact",
                                        click=(toggle_visibility, "[item.id]"),
                                        color=("item.visible ? 'grey-darken-3' : 'grey-lighten-1'",)
                                     ):
                                        vuetify3.VIcon("{{ item.visible ? 'mdi-eye' : 'mdi-eye-off' }}")
                                        vuetify3.VTooltip(activator="parent", location="bottom", text="Toggle Visibility")

                                    # Focus Camera
                                     with vuetify3.VBtn(
                                        icon=True, 
                                        variant="text", 
                                        density="compact",
                                        click=(focus_camera_on, "[item.id]"),
                                        color="primary"
                                    ):
                                        vuetify3.VIcon("mdi-crosshairs-gps")
                                        vuetify3.VTooltip(activator="parent", location="bottom", text="Focus Camera")

                                    # Settings Button
                                     with vuetify3.VBtn(
                                        icon=True, 
                                        variant="text", 
                                        density="compact",
                                        click=(open_edit_dialog, "[item.id]"),
                                        color="secondary"
                                    ):
                                        vuetify3.VIcon("mdi-palette")
                                        vuetify3.VTooltip(activator="parent", location="bottom", text="Edit Visualization")
                                        
                                    # Delete
                                     with vuetify3.VBtn(
                                        icon=True, 
                                        variant="text", 
                                        density="compact",
                                        click=(remove_proxy, "[item.id]"),
                                        color="error"
                                    ):
                                        vuetify3.VIcon("mdi-delete")
                                        vuetify3.VTooltip(activator="parent", location="bottom", text="Remove Source")

                # Bottom Half: Steering Controls
                with vuetify3.VContainer(classes="pa-4 d-flex flex-column", style=("`height: ${split_pos}%; overflow-y: auto; transition: height 0.1s;`",)):
                    # Sticky Header with Apply Button
                    with vuetify3.VRow(classes="align-center mb-2", no_gutters=True, style="position: sticky; top: 0; z-index: 10; background: #000; color: #fff; backdrop-filter: blur(2px); padding-top: 8px; padding-bottom: 8px;"):
                        vuetify3.VCardTitle("Steering Controls", classes="py-0 pl-0 text-white")
                        vuetify3.VSpacer()
                        vuetify3.VBtn("Apply", click=apply_steering, disabled=("!connected",), color="primary", size="small", variant="flat", classes="text-white")
                    
                    vuetify3.VDivider(classes="mb-4")

                    # Helper to render a single parameter control
                    def render_control(param, array_mode=False, strip_prefix=None):
                        name = param['name']
                        safe_name = param['safe_name']
                        label = param['label']
                        widget = param['widget']
                        # Avoid repeating the array/proxy name in every item label
                        if strip_prefix and isinstance(label, str):
                            lp = strip_prefix.strip()
                            if lp and label.lower().startswith(lp.lower()):
                                label = label[len(lp):].lstrip(" :.-")
                        
                        # Determine v_model suffix/access
                        def get_model(base, comp=None):
                            if array_mode:
                                if comp is not None:
                                    return (f"{base}_{comp}[row_idx]",)
                                return (f"{base}[row_idx]",)
                            else:
                                if comp is not None:
                                    return (f"{base}_{comp}",)
                                return (f"{base}",)

                        if widget == 'checkbox':
                            vuetify3.VCheckbox(
                                v_model=get_model(f"steer_{safe_name}"),
                                label=label,
                                disabled=("!connected",),
                                density="compact"
                            )
                        elif widget == 'select':
                            items_key = f"items_{safe_name}"
                            state[items_key] = param['domain']['items']
                            vuetify3.VSelect(
                                v_model=get_model(f"steer_{safe_name}"),
                                items=(items_key, param['domain']['items']),
                                label=label,
                                disabled=("!connected",),
                                density="compact",
                                item_title="title",
                                item_value="value",
                            )
                        elif widget == 'slider':
                            step_val = 1 if 'Int' in param['type'] else 0.1
                            step_any = "1" if 'Int' in param['type'] else "any"
                            
                            if param['num_elements'] == 1:
                                # Scalar Slider + Text
                                if param.get('function', '').startswith('vec1'):
                                    vuetify3.VLabel(text=label, classes="text-caption")
                                    slider_label = "" 
                                    text_label = "Comp 0"
                                else:
                                    slider_label = label
                                    text_label = ""
                                
                                vuetify3.VSlider(
                                    v_model=get_model(f"steer_{safe_name}"),
                                    min=param['domain']['min'],
                                    max=param['domain']['max'],
                                    step=step_val,
                                    label=slider_label,
                                    disabled=("!connected",),
                                    density="compact",
                                    hide_details=True
                                )
                                vuetify3.VTextField(
                                    v_model=get_model(f"steer_{safe_name}"),
                                    label=text_label,
                                    type="number",
                                    step=step_any,
                                    disabled=("!connected",),
                                    density="compact",
                                    hide_details=True,
                                    classes="mb-2 mt-1"
                                )
                            else:
                                # Vector Sliders + Text
                                vuetify3.VLabel(text=label, classes="text-caption")
                                for i in range(param['num_elements']):
                                    with vuetify3.VRow(dense=True, classes="ma-0"):
                                        with vuetify3.VCol(cols=8):
                                            vuetify3.VSlider(
                                                v_model=get_model(f"steer_{safe_name}", i),
                                                min=param['domain']['min'],
                                                max=param['domain']['max'],
                                                step=step_val,
                                                disabled=("!connected",),
                                                density="compact",
                                                hide_details=True
                                            )
                                        with vuetify3.VCol(cols=4):
                                            vuetify3.VTextField(
                                                v_model=get_model(f"steer_{safe_name}", i),
                                                label=f"Comp {i}",
                                                type="number",
                                                step=step_any,
                                                disabled=("!connected",),
                                                density="compact",
                                                hide_details=True
                                            )
                                vuetify3.VSpacer(classes="mb-2")
                        else:
                            # Text fields
                            step_val = "1" if 'Int' in param['type'] else "any"

                            if param['num_elements'] == 1:
                                if param.get('function', '').startswith('vec1'):
                                    vuetify3.VLabel(text=label, classes="text-caption")
                                    vuetify3.VTextField(
                                        v_model=get_model(f"steer_{safe_name}"),
                                        label="Comp 0",
                                        type="number",
                                        step=step_val,
                                        disabled=("!connected",),
                                        density="compact",
                                        hide_details=True
                                    )
                                    vuetify3.VSpacer(classes="mb-2")
                                else:
                                    vuetify3.VTextField(
                                        v_model=get_model(f"steer_{safe_name}"),
                                        label=label,
                                        type="number",
                                        step=step_val,
                                        disabled=("!connected",),
                                        density="compact"
                                    )
                            else:
                                vuetify3.VLabel(text=label, classes="text-caption")
                                with vuetify3.VRow(dense=True):
                                    for i in range(param['num_elements']):
                                        with vuetify3.VCol(cols=12 // param['num_elements']):
                                            vuetify3.VTextField(
                                                v_model=get_model(f"steer_{safe_name}", i),
                                                label=f"Comp {i}",
                                                type="number",
                                                step=step_val,
                                                disabled=("!connected",),
                                                density="compact",
                                                hide_details=True
                                            )
                                vuetify3.VSpacer(classes="mb-2")

                    # Proxy Selection
                    if steerable_proxies:
                        proxy_options = [{"title": p['label'], "value": p['name']} for p in steerable_proxies]
                        state.proxy_options = proxy_options
                        
                        vuetify3.VSelect(
                            v_model="selected_steering_proxy",
                            items=("proxy_options", proxy_options),
                            label="Select Parameter Set",
                            density="compact",
                            variant="outlined",
                            classes="mb-2",
                            item_title="title",
                            item_value="value",
                            hide_details=True,
                            disabled=("!connected",)
                        )
                        vuetify3.VDivider(classes="mb-4")

                        # Dynamically generate controls
                        for proxy_def in steerable_proxies:
                            with vuetify3.VContainer(classes="pa-0", v_if=f"selected_steering_proxy === '{proxy_def['name']}'"):
                                proxy_safe_name = proxy_def['safe_name']
                                is_array = proxy_def['type'] == 'array'
                                
                                if is_array:
                                    # Array Proxy UI
                                    with vuetify3.VRow(classes="ma-0 mb-2"):
                                        vuetify3.VSpacer()
                                        vuetify3.VBtn("Add Entry", click=f"trigger('add_entry', ['{proxy_safe_name}'])", color="success", size="small", variant="tonal")

                                    with vuetify3.VContainer(v_for=f"(row_idx, i) in indices_{proxy_safe_name}", key="row_idx", classes="pa-0"):
                                        with vuetify3.VCard(classes="mb-3 pa-2", variant="outlined"):
                                            with vuetify3.VRow(dense=True, classes="align-center mb-1"):
                                                vuetify3.VCardTitle("Item {{ i }}", classes="text-subtitle-2 py-0")
                                                vuetify3.VSpacer()
                                                vuetify3.VBtn(icon="mdi-delete", size="x-small", color="error", variant="text", 
                                                            click=f"trigger('remove_entry', ['{proxy_safe_name}', row_idx])")
                                            
                                            vuetify3.VDivider(classes="mb-2")
                                            
                                            for item in proxy_def['children']:
                                                if item.get('item_type') == 'group':
                                                    # Strip prefix from group label to avoid repetition
                                                    grp_label = item['label']
                                                    if isinstance(grp_label, str) and proxy_def.get('label'):
                                                        lp = proxy_def['label'].strip()
                                                        if lp and grp_label.lower().startswith(lp.lower()):
                                                            grp_label = grp_label[len(lp):].lstrip(" :.-")
                                                    vuetify3.VCardTitle(grp_label, classes="text-caption font-weight-bold")
                                                    with vuetify3.VContainer(classes="pa-0 pl-2"):
                                                        for param in item['children']:
                                                            render_control(param, array_mode=True, strip_prefix=proxy_def.get('label'))
                                                else:
                                                    render_control(item, array_mode=True, strip_prefix=proxy_def.get('label'))
                                else:
                                    # Scalar Proxy UI
                                    for item in proxy_def['children']:
                                        if item.get('item_type') == 'group':
                                            vuetify3.VCardTitle(item['label'], classes="text-subtitle-2 mt-2 font-weight-bold")
                                            vuetify3.VDivider(classes="mb-2")
                                            with vuetify3.VContainer(classes="pa-0 pl-2"):
                                                for param in item['children']:
                                                    render_control(param, array_mode=False)
                                            vuetify3.VSpacer(classes="mb-4")
                                        else:
                                            render_control(item, array_mode=False)
                        # Sticky footer Apply bar
                        # with vuetify3.VRow(classes="align-center mt-2", no_gutters=True, style="position: sticky; bottom: 0; z-index: 10; background: rgba(30,30,30,0.9); backdrop-filter: blur(2px); padding-top: 8px; padding-bottom: 8px; border-top: 1px solid #555;"):
                        #     vuetify3.VSpacer()
                        #     vuetify3.VBtn("Apply", click=apply_steering, disabled=("!connected",), color="primary", variant="elevated")
                    else:
                        vuetify3.VAlert(type="warning", text="No steerable parameters found.")

            # --- RIGHT COLUMN: SLIDER ---
            with vuetify3.VCol(classes="flex-grow-0 pa-0 d-flex justify-center bg-grey-darken-3", style="width: 24px; min-width: 24px;"):
                vuetify3.VSlider(
                    v_model=("split_pos", 50),
                    direction="vertical",
                    min=20, max=80,
                    density="compact",
                    hide_details=True,
                    track_color="grey-darken-1",
                    thumb_color="grey-lighten-1",
                    thumb_size=12,
                    track_size=4
                )

    with layout.toolbar:
        vuetify3.VSpacer()
        
        # Status & Play/Pause
        vuetify3.VChip("{{ status_text }}", color=("status_color", "grey"), text_color="white", classes="mr-4")
        with vuetify3.VBtn(click=toggle_simulation, disabled=("!connected",), color=("sim_paused ? 'green' : 'orange'",), icon=True, variant="tonal", classes="mr-2"):
            vuetify3.VIcon("{{ sim_paused ? 'mdi-play' : 'mdi-pause' }}")
        
        vuetify3.VDivider(vertical=True, classes="mx-4")
        vuetify3.VBtn("{{ connected ? 'Disconnect' : 'Connect' }}", click=toggle_connection, variant="text")
        
        # --- SCAN BUTTON (Refresh List Only) ---
        with vuetify3.VBtn(
            icon=True, 
            click=scan_sources_only, 
            disabled=("!connected",), 
            variant="text", 
            color="grey-darken-1"
        ):
            vuetify3.VIcon("mdi-database-refresh")
            vuetify3.VTooltip(activator="parent", location="bottom", text="Scan/Refresh List")

        # --- COMBOBOX ---
        vuetify3.VCombobox(
            v_model=("selected_source", ""),
            items=("available_sources", []),
            label="Source Channel",
            disabled=("!connected",),
            density="compact",
            hide_details=True,
            style="max-width: 250px;",
            classes="mx-1",
            # keyup_enter=search_and_select_best
            # keydown_enter=(search_and_select_best, "[$event.target.value]")
            keydown_enter=search_and_select_best
        )
        
        # --- SEARCH BUTTON (Pick Best Match) ---
        with vuetify3.VBtn(
            icon=True, 
            click=search_and_select_best, 
            disabled=("!connected",), 
            variant="text", 
            color="primary"
        ):
            # vuetify3.VIcon("mdi-crosshairs-gps")
            vuetify3.VIcon("mdi-magnify")
            vuetify3.VTooltip(activator="parent", location="bottom", text="[Enter] Find Best Match")

        # Initialize & Reset
        vuetify3.VBtn("Initialize", click=extract_data, disabled=("!connected || !selected_source",), variant="text")
        with vuetify3.VBtn(icon=True, click=reset_visualization, disabled=("!has_data",), color="error", variant="text"):
            vuetify3.VIcon("mdi-refresh")
        
        # --- AXES GRID TOGGLE ---
        with vuetify3.VBtn(
            icon=True, 
            click=toggle_axes_grid, 
            disabled=("!has_data",), 
            variant="text", 
            color=("axes_grid_visible ? 'primary' : 'grey'",)
        ):
            vuetify3.VIcon("{{ axes_grid_visible ? 'mdi-grid' : 'mdi-grid-off' }}")
            vuetify3.VTooltip(activator="parent", location="bottom", text="Toggle Axes Grid")
        
        vuetify3.VDivider(vertical=True, classes="mx-4")
        
        # --- LIVE TOGGLE (Green Success Color) ---
        vuetify3.VSwitch(
            v_model=("live_mode", False), 
            label="Live", 
            color="success", # <--- Requested Color Change
            hide_details=True, 
            disabled=("!has_data",), 
            density="compact"
        )

    with layout.content:
        with vuetify3.VContainer(fluid=True, classes="fill-height pa-0"):
            view = vtk.VtkRemoteView(simple.GetRenderView())
            ctrl.view_update = view.update
            
        # Edit Dialog
        with vuetify3.VDialog(v_model=("show_edit_dialog", False), max_width=500):
            with vuetify3.VCard():
                vuetify3.VCardTitle("Edit Visualization: {{ editing_source }}")
                with vuetify3.VCardText():
                    vuetify3.VSelect(
                        v_model=("current_color_array",),
                        items=("color_arrays", []),
                        label="Color By",
                        item_title="title",
                        item_value="value",
                        density="compact",
                        variant="outlined",
                    )
                with vuetify3.VCardActions():
                    vuetify3.VSpacer()
                    vuetify3.VBtn("Cancel", click="show_edit_dialog = False")
                    vuetify3.VBtn("OK", click=apply_and_close, color="primary", variant="elevated")

state.connected = False
state.has_data = False
state.live_mode = False 
state.status_text = "Disconnected"
state.sim_paused = False
state.available_sources = [] 
state.selected_source = None
state.pipeline_items = []
state.axes_grid_visible = True
state.split_pos = 50
state.show_edit_dialog = False
state.editing_source = ""
state.color_arrays = []
state.current_color_array = None

# Initialize state for steerable parameters
state.selected_steering_proxy = steerable_proxies[0]['name'] if steerable_proxies else None

for proxy_def in steerable_proxies:
    proxy_safe_name = proxy_def['safe_name']
    is_array = proxy_def['type'] == 'array'
    
    # Flatten params
    flat_params = []
    def collect_params(items):
        for item in items:
            if item['item_type'] == 'group':
                collect_params(item['children'])
            else:
                flat_params.append(item)
    collect_params(proxy_def['children'])
    
    # Determine size
    num_rows = 1
    if flat_params:
        p0 = flat_params[0]
        defaults = p0['default']
        # For array proxies, defaults contain all values for all rows
        # Assuming all params are consistent in length
        if is_array:
            num_rows = len(defaults) // p0['num_elements']
            # Special case: if exactly one element (per vector) and it is zero, treat as empty
            if num_rows == 1:
                try:
                    if all(float(x) == 0 for x in defaults):
                        num_rows = 0
                except: pass
        else:
            num_rows = 1
    
    if is_array:
        state[f"count_{proxy_safe_name}"] = num_rows
        state[f"indices_{proxy_safe_name}"] = list(range(num_rows))
        state[f"next_idx_{proxy_safe_name}"] = num_rows # For unique keys
    
    for param in flat_params:
        safe_name = param['safe_name']
        defaults = param['default']
        is_double = 'Double' in param['type']
        
        if is_array:
            # Initialize lists
            if param['num_elements'] > 1:
                for i in range(param['num_elements']):
                    key = f"steer_{safe_name}_{i}"
                    vals = []
                    for row_idx in range(num_rows):
                        def_idx = row_idx * param['num_elements'] + i
                        val = defaults[def_idx] if def_idx < len(defaults) else "0"
                        vals.append(float(val) if is_double else int(val))
                    state[key] = vals
            else:
                key = f"steer_{safe_name}"
                vals = []
                for row_idx in range(num_rows):
                    def_idx = row_idx
                    val = defaults[def_idx] if def_idx < len(defaults) else "0"
                    if param['widget'] == 'checkbox':
                        vals.append(bool(int(val)))
                    else:
                        vals.append(float(val) if is_double else int(val))
                state[key] = vals
        else:
            # Scalar Proxy (Single values)
            if param['num_elements'] > 1:
                for i in range(param['num_elements']):
                    val = defaults[i] if i < len(defaults) else "0"
                    state[f"steer_{safe_name}_{i}"] = float(val) if is_double else int(val)
            else:
                val = defaults[0] if defaults else "0"
                if param['widget'] == 'checkbox':
                    state[f"steer_{safe_name}"] = bool(int(val))
                else:
                    state[f"steer_{safe_name}"] = float(val) if is_double else int(val)

@ctrl.trigger("add_entry")
def add_entry(proxy_safe_name):
    # Find proxy def
    proxy_def = next((p for p in steerable_proxies if p['safe_name'] == proxy_safe_name), None)
    if not proxy_def: return
    
    # Update indices
    next_idx = state[f"next_idx_{proxy_safe_name}"]
    state[f"indices_{proxy_safe_name}"] = state[f"indices_{proxy_safe_name}"] + [next_idx]
    state[f"next_idx_{proxy_safe_name}"] = next_idx + 1
    state[f"count_{proxy_safe_name}"] += 1
    
    # Flatten params
    flat_params = []
    def collect_params(items):
        for item in items:
            if item['item_type'] == 'group':
                collect_params(item['children'])
            else:
                flat_params.append(item)
    collect_params(proxy_def['children'])
    
    # Append defaults
    for param in flat_params:
        safe_name = param['safe_name']
        is_double = 'Double' in param['type']
        # Use first default value as template for new entry
        defaults = param['default']
        
        if param['num_elements'] > 1:
            for i in range(param['num_elements']):
                key = f"steer_{safe_name}_{i}"
                val = defaults[i] if i < len(defaults) else "0"
                current_list = state[key]
                state[key] = current_list + [float(val) if is_double else int(val)]
        else:
            key = f"steer_{safe_name}"
            val = defaults[0] if defaults else "0"
            current_list = state[key]
            if param['widget'] == 'checkbox':
                state[key] = current_list + [bool(int(val))]
            else:
                state[key] = current_list + [float(val) if is_double else int(val)]

@ctrl.trigger("remove_entry")
def remove_entry(proxy_safe_name, row_idx):
    # row_idx is the unique key, we need to find its position in the list
    indices = state[f"indices_{proxy_safe_name}"]
    try:
        pos = indices.index(row_idx)
    except ValueError:
        return
    
    # Remove from indices
    new_indices = list(indices)
    new_indices.pop(pos)
    state[f"indices_{proxy_safe_name}"] = new_indices
    state[f"count_{proxy_safe_name}"] -= 1
    
    # Find proxy def
    proxy_def = next((p for p in steerable_proxies if p['safe_name'] == proxy_safe_name), None)
    if not proxy_def: return
    
    # Flatten params
    flat_params = []
    def collect_params(items):
        for item in items:
            if item['item_type'] == 'group':
                collect_params(item['children'])
            else:
                flat_params.append(item)
    collect_params(proxy_def['children'])
    
    # Remove from state lists
    for param in flat_params:
        safe_name = param['safe_name']
        if param['num_elements'] > 1:
            for i in range(param['num_elements']):
                key = f"steer_{safe_name}_{i}"
                current_list = list(state[key])
                if pos < len(current_list):
                    current_list.pop(pos)
                    state[key] = current_list
        else:
            key = f"steer_{safe_name}"
            current_list = list(state[key])
            if pos < len(current_list):
                current_list.pop(pos)
                state[key] = current_list

@state.change("connected")
def reset_view_update_on_connect(connected, **kwargs):
    global view_update_enabled
    view_update_enabled = True
    if not connected:
        state.live_mode = False

@state.change("connected")
def update_color(connected, **kwargs):
    state.status_color = "success" if connected else "grey"

ctrl.resume_polling = lambda: asyncio.create_task(poll_catalyst())

if __name__ == "__main__":
    simple.GetRenderView()
    server.start()