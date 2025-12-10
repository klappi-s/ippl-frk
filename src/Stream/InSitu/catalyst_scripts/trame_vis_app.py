import asyncio
import time

from paraview import simple, live
from trame.app import get_server
from trame.widgets import vuetify3, vtk
from trame.ui.vuetify3 import SinglePageLayout

# Local modules
# Import render/steering config with fallback to absolute module when run as a script
try:
    # Prefer centralized modules under trame_app
    from trame_app import trame_render_config as render_config
    from trame_app import trame_steering_config as steering_config
    from trame_app import trame_connection as connection_api
except Exception:
    # Fallbacks for script execution without package context
    try:
        from .trame_app import trame_render_config as render_config
        from .trame_app import trame_steering_config as steering_config
        from .trame_app import trame_connection as connection_api
    except Exception:
        import trame_app.trame_render_config as render_config
        import trame_app.trame_steering_config as steering_config
        import trame_app.trame_connection as connection_api
from trame_app.trame_ctx import Ctx
from trame_app.trame_runtime import resume_polling

# Server and state
server = get_server()
state, ctrl = server.state, server.controller

# Helpers used across the file
def find_source_by_name(name):
    try:
        return simple.FindSource(name)
    except Exception:
        return None

# Legacy globals (kept for compatibility with existing code paths)
active_proxies = {}
view_update_enabled = True
last_view_update_ts = 0.0
view_update_pending_until = 0.0
camera_last_sig = None
camera_last_change_ts = 0.0
catalyst_link = None

def remove_proxy(name):
    from trame_app import trame_pipeline as pipe
    pipe.remove_proxy(_ctx, name)

def toggle_visibility(name):
    from trame_app import trame_pipeline as pipe
    pipe.toggle_visibility(_ctx, name)

def focus_camera_on(name):
    from trame_app import trame_pipeline as pipe
    pipe.focus_camera_on(_ctx, name)

def get_display_proxy(name):
    from trame_app import trame_pipeline as pipe
    return pipe.get_display_proxy(_ctx, name)

# Delegate color UI actions to color module
from trame_app.trame_color import ColorAPI

def open_edit_dialog(name):
    ColorAPI(_ctx).open_edit_dialog(name)

def update_color_by(value):
    ColorAPI(_ctx).update_color_by(value)

def apply_color_map(preset_key):
    ColorAPI(_ctx).apply_color_map(preset_key)

def set_scalar_bar_visibility(desired_vis):
    ColorAPI(_ctx).set_scalar_bar_visibility(desired_vis)

# Steering: reload/extract all steering proxies via connection module
def reload_steering_proxies():
    print("[UI] Reload Steering Proxies clicked")
    try:
        connection_api.reload_steering_proxies(_ctx)
    except Exception as e:
        print(f"[WARN] Reload steering proxies failed: {e}")

@ctrl.trigger("reload_steering")
def _reload_steering_trigger():
    reload_steering_proxies()

@state.change("scalar_bar_visible")
def _on_scalar_bar_visible_change(scalar_bar_visible=None, **kwargs):
    try:
        ColorAPI(_ctx).set_scalar_bar_visibility(bool(scalar_bar_visible))
    except Exception as e:
        print(f"[WARN] Failed to set scalar bar visibility: {e}")

@state.change("current_color_map")
def _on_color_map_change(current_color_map=None, **kwargs):
    print(f"[UI] Color Map preset changed: {current_color_map}")
    if not current_color_map:
        return
    try:
        ColorAPI(_ctx).apply_color_map(current_color_map)
    except Exception as e:
        print(f"[WARN] Failed to apply color map on change: {e}")

def apply_and_close():
    print(f"[UI] Apply and Close clicked for: {state.editing_source}")
    # Apply color change while respecting current scalar bar visibility preference
    update_color_by(state.current_color_array)
    state.show_edit_dialog = False

def apply_color_map(preset_key):
    print(f"[UI] Apply Color Map clicked: {preset_key} (source: {state.editing_source})")
    """Apply a color map preset to the current LUT for the selected array."""
    global view_update_enabled
    name = state.editing_source
    if not name:
        return
    proxy = get_display_proxy(name)
    view = simple.GetActiveView()
    if not proxy or not view:
        return
    rep = simple.GetRepresentation(proxy, view)
    if not rep:
        return
    # Solid color has no LUT
    if state.current_color_array == 'SOLID':
        return
    ca = rep.ColorArrayName
    if not ca or len(ca) < 2 or not ca[1]:
        return
    array_name = ca[1]
    lut = simple.GetColorTransferFunction(array_name)

    # Map our keys to ParaView preset names
    preset_map = {
        'cool_to_warm': 'Cool to Warm',
        'cool_to_warm_ext': 'Cool to Warm (Extended)',
        'cold_and_hot': 'Cold and Hot',
        'viridis': 'Viridis (matplotlib)',
        'inferno': 'Inferno (matplotlib)',
        'black_body': 'Black-Body Radiation',
    }
    preset_name = preset_map.get(preset_key)
    if not preset_name:
        return
    try:
        # Use rescale to data range after applying preset
        lut.ApplyPreset(preset_name, True)
    except Exception as e:
        print(f"[WARN] Failed to apply preset '{preset_name}': {e}")
        return

    # Keep representation pointing to this lut
    rep.LookupTable = lut

    # Maintain scalar bar visibility state
    sb = simple.GetScalarBar(lut, view)
    if sb:
        sb.Visibility = 1 if state.scalar_bar_visible else 0
        sb.Title = array_name
        sb.ComponentTitle = 'Magnitude'
    try:
        rep.SetScalarBarVisibility(view, bool(state.scalar_bar_visible))
    except Exception:
        pass

    # Render/update
    simple.Render()
    if hasattr(ctrl, 'view_update') and view_update_enabled:
        try:
            ctrl.view_update()
        except Exception as e:
            print(f"[WARN] view_update failed: {e}. Disabling further updates and live mode.")
            view_update_enabled = False
            state.live_mode = False
            state.status_text = "Live disabled: transport error"
    # Remember selection for this source
    try:
        d = dict(state.color_map_per_source)
        d[name] = preset_key
        state.color_map_per_source = d
        state.current_color_map = preset_key
    except Exception:
        state.color_map_per_source = { name: preset_key }

def set_scalar_bar_visibility(desired_vis):
    print(f"[UI] Scalar Bar visibility toggled: {desired_vis} (source: {state.editing_source})")
    """Set the scalar bar visibility to desired_vis for the current selection."""
    name = state.editing_source
    proxy = get_display_proxy(name) if name else None
    view = simple.GetActiveView()
    if not proxy or not view:
        return
    rep = simple.GetRepresentation(proxy, view)
    if not rep:
        return

    cur_sel = state.current_color_array
    if cur_sel == 'SOLID':
        # Ensure hidden regardless of desired
        desired_vis = False
        try:
            rep.SetScalarBarVisibility(view, False)
        except Exception:
            pass
    else:
        try:
            rep.SetScalarBarVisibility(view, bool(desired_vis))
            ca = rep.ColorArrayName
            if ca and len(ca) > 1 and ca[1]:
                lut = simple.GetColorTransferFunction(ca[1])
                sb = simple.GetScalarBar(lut, view)
                if sb:
                    sb.Visibility = 1 if desired_vis else 0
        except Exception:
            pass
    state.scalar_bar_visible = bool(desired_vis)
    simple.Render()
    if hasattr(ctrl, 'view_update') and view_update_enabled:
        try:
            ctrl.view_update()
        except Exception:
            pass

# Moved to trame_steering_config.py:
# - load_steerable_proxies
# - get_steerable_proxy
# - update_steering_parameter
# - parse_steerable_parameters

def apply_steering():
    print("[UI] Apply Steering clicked")
    """Apply the current steering parameters."""
    print("[DEBUG] apply_steering called")
    link = getattr(_ctx, "catalyst_link", None)
    if not link:
        print("[WARN] Cannot apply steering: no Catalyst connection available in context.")
        return

    proxy_defs = getattr(state, "steerable_proxies", None) or steerable_proxies
    if not proxy_defs:
        print("[WARN] No steerable proxies defined; aborting apply.")
        return

    for proxy_def in proxy_defs:
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
            count_key = f"count_{proxy_safe_name}"
            try:
                num_rows = int(state[count_key])
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
                
                steering_config.update_steering_parameter(link, proxy_name, name, all_values)
                
            except Exception as e:
                print(f"[ERROR] Error applying parameter {name}: {e}")
                import traceback
                traceback.print_exc()

def connect_to_catalyst():
    # Delegated to connection module
    from trame_app import trame_connection as conn
    conn.connect_to_catalyst(_ctx)

def scan_sources_only():
    from trame_app import trame_connection as conn
    conn.scan_sources_only(_ctx)


def toggle_connection():
    from trame_app import trame_connection as conn
    conn.toggle_connection(_ctx)


def search_and_select_best():
    from trame_app import trame_connection as conn
    conn.search_and_select_best(_ctx)


def extract_data():
    from trame_app import trame_connection as conn
    conn.extract_data(_ctx)


def reset_visualization():
    from trame_app import trame_connection as conn
    conn.reset_visualization(_ctx)

# Removed reset_camera function as it is now in trame_render_config.py

# ... (Polling and Steering) ...
async def poll_catalyst():
    while True:
        if catalyst_link:
            try:
                live.ProcessServerNotifications()
                if state.live_mode and state.has_data:
                    # Capture camera signature to detect interaction / movement
                    view = simple.GetActiveView()
                    now = time.time()
                    camera_sig = None
                    try:
                        if view:
                            camera_sig = (
                                tuple(getattr(view, 'CameraPosition', []) or []),
                                tuple(getattr(view, 'CameraFocalPoint', []) or []),
                                tuple(getattr(view, 'CameraViewUp', []) or []),
                                getattr(view, 'CameraViewAngle', None),
                                getattr(view, 'CameraParallelScale', None),
                            )
                    except Exception:
                        camera_sig = None

                    # Update last change timestamp if camera properties have changed
                    global camera_last_sig, camera_last_change_ts
                    if camera_sig is not None and camera_sig != camera_last_sig:
                        camera_last_sig = camera_sig
                        camera_last_change_ts = now
                        # Log camera perspective change (debounced by natural loop frequency)
                        try:
                            pos, foc, up, ang, pscale = camera_sig
                            print(f"[UI] Camera changed: pos={pos}, focal={foc}, up={up}, angle={ang}, parallelScale={pscale}")
                        except Exception:
                            print("[UI] Camera changed")
                    
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
                                # For scalar fields, avoid over-updating the local resample filter.
                                # Only update/resample every few polling cycles to reduce payload size.
                                # Use a lightweight modulo gate based on time.
                                now_gate = time.time()
                                # Update at most every 0.5s for the resample proxy
                                if (now_gate - getattr(state, 'last_resample_update_ts', 0.0)) > 2.0:
                                    resample_proxy.UpdatePipeline()
                                    render_config.update_scalar_field_view(resample_proxy, simple.GetActiveView())
                                    state.last_resample_update_ts = now_gate
                                
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
                    global last_view_update_ts, view_update_enabled, view_update_pending_until
                    now = time.time()
                    # Heavier payloads for scalar fields: increase throttle interval
                    has_scalar_field = any(k.startswith("ippl_sField") for k in active_proxies.keys())
                    # Increase interval significantly to keep UI responsive when scalar fields are active
                    interval = 2.0 if has_scalar_field else 0.5
                    # Debounce camera motion: only send a frame once camera has been stable for a short period
                    camera_stable_for = (now - camera_last_change_ts) if camera_last_change_ts else 999
                    camera_stable = (camera_stable_for > 1.0)  # 400ms of no camera change
                    # Backpressure gate: skip triggering a new update if a previous one may still be flushing
                    if now < view_update_pending_until:
                        pass
                    # Skip sending intermediate frames while camera is moving, especially for scalar fields
                    elif has_scalar_field and not camera_stable:
                        # While interacting with camera, defer pushing frames to avoid flooding the socket
                        pass
                    elif hasattr(ctrl, 'view_update') and view_update_enabled and state.live_mode and (now - last_view_update_ts) > interval:
                        try:
                            ctrl.view_update()
                            last_view_update_ts = now
                            # Assume the transport may still be flushing large frames for ~1s when sFields are active
                            view_update_pending_until = now + (1.2 if has_scalar_field else 0.3)
                        except Exception as e:
                            print(f"[WARN] view_update failed during polling: {e}. Disabling further updates and live mode.")
                            view_update_enabled = False
                            state.live_mode = False
                            state.status_text = "Live disabled: transport error"
            except Exception:
                pass
        await asyncio.sleep(0.5)


        
def pause_sim():
    print("[UI] Pause clicked")
    link = getattr(_ctx, 'catalyst_link', None)
    try:
        if link:
            live.PauseCatalyst(link, pause=True)
            state.sim_paused = True
    except Exception as e:
        print(f"[WARN] Failed to pause simulation: {e}")

def unpause_sim():
    print("[UI] Unpause clicked")
    link = getattr(_ctx, 'catalyst_link', None)
    try:
        if link:
            live.PauseCatalyst(link, pause=False)
            state.sim_paused = False
    except Exception as e:
        print(f"[WARN] Failed to unpause simulation: {e}")

def toggle_simulation():
    print("[UI] Toggle Simulation clicked")
    link = getattr(_ctx, 'catalyst_link', None)
    try:
        if link:
            paused_prop = link.GetProperty('SimulationPaused')
            is_paused = False
            try:
                is_paused = bool(paused_prop.GetElement(0))
            except Exception:
                # Fallback: use our UI state if property access fails
                is_paused = bool(state.sim_paused)
            if is_paused:
                unpause_sim()
            else:
                pause_sim()
    except Exception as e:
        print(f"[WARN] Failed to toggle simulation: {e}")

def toggle_axes_grid():
    print("[UI] Toggle Axes Grid clicked")
    global view_update_enabled
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
                
                # Top Half: Pipeline Browser (fixed header height ~56px + controls row ~44px)
                # Make the top half manage its own scroll; prevent the entire left column from scrolling at once
                with vuetify3.VContainer(classes="pa-0 d-flex flex-column", style=("`height: calc(100% - ${split_pos}%); border-bottom: 1px solid #555; transition: height 0.1s; overflow: hidden;`",)):
                    # Sticky title row (keep compact: title + refresh)
                    with vuetify3.VRow(classes="align-center", no_gutters=True, style="position: sticky; top: 0; z-index: 10; background: #000; color: #fff; height: 56px; padding: 10px;"):
                        vuetify3.VCardTitle("Pipeline Browser", classes="py-0 pl-0 text-white")
                        vuetify3.VSpacer()
                        # Scan/Refresh list (stay on title row)
                        with vuetify3.VBtn(icon=True, click=scan_sources_only, disabled=("!connected",), variant="text", color="grey-lighten-1"):
                            vuetify3.VIcon("mdi-database-refresh")
                            vuetify3.VTooltip(activator="parent", location="bottom", text="Scan/Refresh List")
                        # Reset visualization (moved from toolbar)
                        with vuetify3.VBtn(icon=True, click=reset_visualization, disabled=("!has_data",), color="error", variant="text", classes="ml-1"):
                            vuetify3.VIcon("mdi-refresh")
                            vuetify3.VTooltip(activator="parent", location="bottom", text="Reset Visualization")

                    # Secondary control row under title (combobox + search + initialize)
                    # Make it part of the sticky header block (separate line) to avoid crowding
                    with vuetify3.VRow(classes="align-center px-2", no_gutters=True, style="position: sticky; top: 56px; z-index: 9; background: #000; color: #fff; height: 44px;"):
                        vuetify3.VCombobox(
                            v_model=("selected_source", ""),
                            items=("available_sources", []),
                            label="Source Channel",
                            disabled=("!connected",),
                            density="compact",
                            hide_details=True,
                            color="cyan-accent-3",
                            variant="outlined",
                            style="max-width: 260px;",
                            classes="mr-2",
                            keydown_enter=search_and_select_best,
                        )
                        vuetify3.VBtn("Init", click=extract_data, disabled=("!connected || !selected_source",), variant="tonal", color="cyan-accent-3", classes="text-white")

                    vuetify3.VDivider(classes="mt-1")
                    
                    # Empty state when no sources are extracted yet
                    with vuetify3.VContainer(v_if="pipeline_items.length === 0", classes="pa-2", style="overflow-y: auto; height: calc(100% - 101px);"):
                        vuetify3.VAlert(type="info", text="No sources extracted yet")

                    # List area scrolls under fixed header block
                    with vuetify3.VList(lines="one", density="compact", style="overflow-y: auto; height: calc(100% - 101px);", v_if="pipeline_items && pipeline_items.length > 0"):
                        with vuetify3.VListItem(
                            v_for="(item, i) in pipeline_items",
                            key=("item.id",),
                            value=("item.id",),
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

                # Bottom Half: Steering Controls (fixed header height ~48px)
                # Give the steering half its own scroll area; the header stays sticky within this half only
                with vuetify3.VContainer(classes="pa-0 d-flex flex-column", style=("`height: ${split_pos}%; transition: height 0.1s; overflow: hidden;`",)):
                    # Sticky Header with Apply Button
                    with vuetify3.VRow(classes="align-center", no_gutters=True, style="position: sticky; top: 0; z-index: 10; background: #000; color: #fff; height: 48px; padding: 8px;"):
                        vuetify3.VCardTitle("Steering Controls", classes="py-0 pl-0 text-white")
                        vuetify3.VSpacer()
                        # Reload/Rescan steering proxies button (diagnostics & refresh)
                        with vuetify3.VBtn(
                            icon=True,
                            click=reload_steering_proxies,
                            disabled=("!connected",),
                            color="grey-lighten-1",
                            size="small",
                            variant="tonal",
                            classes="mr-2 text-white"
                        ):
                            vuetify3.VIcon("mdi-database-refresh")
                            vuetify3.VTooltip(activator="parent", location="bottom", text="Scan/Refresh List")



                        vuetify3.VBtn("Apply", click=apply_steering, disabled=("!connected",), color="primary", size="small", variant="flat", classes="text-white")

                    vuetify3.VDivider(classes="mb-2")

                    # Scrollable content area inside the Steering half (below the sticky header)
                    # Move dynamic controls inside this container so they scroll independently of the header
                    with vuetify3.VContainer(classes="pa-4", style="overflow-y: auto; height: calc(100% - 58px);"):
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
                            # Sticky footer Apply bar (optional)
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
        
        # Reverse the order of GUI elements (left-to-right): live switch, play/pause, grid toggle, separator, connect toggle, connected status
        vuetify3.VSwitch(
            v_model=("live_mode", False), 
            label="Live", 
            color="success",
            hide_details=True, 
            disabled=("!connected",), 
            density="compact",
            classes="mr-3"
        )
        with vuetify3.VBtn(click=toggle_simulation, disabled=("!connected",), color=("sim_paused ? 'green' : 'orange'",), icon=True, variant="tonal", classes="mr-2"):
            vuetify3.VIcon("{{ sim_paused ? 'mdi-play' : 'mdi-pause' }}")
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
        vuetify3.VBtn("{{ connected ? 'Disconnect' : 'Connect' }}", click=toggle_connection, variant="text", classes="mr-2")
        vuetify3.VChip("{{ status_text }}", color=("status_color", "grey"), text_color="white", classes="mr-4")

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
                    vuetify3.VDivider(classes="my-2")
                    vuetify3.VSwitch(
                        v_model=("scalar_bar_visible", False),
                        label="Show Colour Bar",
                        inset=True,
                        density="comfortable",
                        color=("scalar_bar_visible ? 'green' : 'grey'",),
                        # change handled via state.change hook
                    )
                    vuetify3.VDivider(classes="my-2")
                    vuetify3.VSelect(
                        v_model=("current_color_map",),
                        items=("available_color_maps", []),
                        label="Color Map",
                        item_title="title",
                        item_value="value",
                        density="compact",
                        variant="outlined",
                        # Rely on state change hook to apply preset
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
state.scalar_bar_visible = False
state.available_color_maps = []
state.current_color_map = None
state.color_map_per_source = {}

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
    print(f"[STATE] Connected changed: {connected}")
    global view_update_enabled
    view_update_enabled = True
    # Reset throttling/backpressure gates on (re-)connect
    try:
        global last_view_update_ts, view_update_pending_until, camera_last_sig, camera_last_change_ts
        last_view_update_ts = 0.0
        view_update_pending_until = 0.0
        camera_last_sig = None
        camera_last_change_ts = 0.0
        # Reset resample cadence marker
        state.last_resample_update_ts = 0.0
    except Exception:
        pass
    if not connected:
        state.live_mode = False

@state.change("connected")
def update_color(connected, **kwargs):
    print(f"[STATE] Status color update on connected={connected}")
    state.status_color = "success" if connected else "grey"

# Log Live mode toggles
@state.change("live_mode")
def _on_live_mode_change(live_mode=None, **kwargs):
    try:
        print(f"[UI] Live mode toggled: {bool(live_mode)}")
    except Exception:
        pass

# Initialize shared context and wire runtime resume
_ctx = Ctx(server=server, state=state, ctrl=ctrl)
resume_polling(_ctx)

if __name__ == "__main__":
    simple.GetRenderView()
    server.start()