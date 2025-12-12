import os
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
    from trame_app import trame_logging as log
except Exception:
    # Fallbacks for script execution without package context
    try:
        from .trame_app import trame_render_config as render_config
        from .trame_app import trame_steering_config as steering_config
        from .trame_app import trame_connection as connection_api
        from .trame_app import trame_logging as log
    except Exception:
        import trame_app.trame_render_config as render_config
        import trame_app.trame_steering_config as steering_config
        import trame_app.trame_connection as connection_api
        import trame_app.trame_logging as log
from trame_app.trame_ctx import Ctx
from trame_app.trame_runtime import resume_polling

# Server and state
server = get_server()
state, ctrl = server.state, server.controller

# Connection defaults
state.catalyst_host = "localhost"
state.catalyst_port = 22222

# Helpers used across the file
# (find_source_by_name removed as it was only used in legacy poll_catalyst)

# Legacy globals removed (active_proxies, view_update_enabled, etc.)


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

def update_representation(value):
    ColorAPI(_ctx).set_representation(value)

def update_solid_color(value):
    ColorAPI(_ctx).set_solid_color(value)

def apply_color_map(preset_key):
    ColorAPI(_ctx).apply_color_map(preset_key)

def apply_custom_range():
    ColorAPI(_ctx).apply_custom_range()

def set_scalar_bar_visibility(desired_vis):
    ColorAPI(_ctx).set_scalar_bar_visibility(desired_vis)

# Steering: reload/extract all steering proxies via connection module
def reload_steering_proxies():
    from trame_app.trame_logging import ui, warn
    ui("Reload Steering Proxies clicked")
    try:
        connection_api.reload_steering_proxies(_ctx)
    except Exception as e:
        warn("Reload steering proxies failed: {}", e)

@ctrl.trigger("reload_steering")
def _reload_steering_trigger():
    reload_steering_proxies()

@state.change("scalar_bar_visible")
def _on_scalar_bar_visible_change(scalar_bar_visible=None, **kwargs):
    try:
        ColorAPI(_ctx).set_scalar_bar_visibility(bool(scalar_bar_visible))
    except Exception as e:
        log.warn("Failed to set scalar bar visibility: {}", e)

@state.change("slice_normal_x", "slice_normal_y", "slice_normal_z")
def _on_slice_normal_change(slice_normal_x=None, slice_normal_y=None, slice_normal_z=None, **kwargs):
    # Reconstruct list from scalar states
    nx = float(state.slice_normal_x)
    ny = float(state.slice_normal_y)
    nz = float(state.slice_normal_z)
    update_slice_normal([nx, ny, nz])

@state.change("slice_origin_x", "slice_origin_y", "slice_origin_z")
def _on_slice_origin_change(slice_origin_x=None, slice_origin_y=None, slice_origin_z=None, **kwargs):
    # Reconstruct list from scalar states
    ox = float(state.slice_origin_x)
    oy = float(state.slice_origin_y)
    oz = float(state.slice_origin_z)
    update_slice_origin([ox, oy, oz])

@state.change("current_color_map")
def _on_color_map_change(current_color_map=None, **kwargs):
    log.ui("Color Map preset changed: {}", current_color_map)
    if not current_color_map:
        return
    try:
        ColorAPI(_ctx).apply_color_map(current_color_map)
    except Exception as e:
        log.warn("Failed to apply color map on change: {}", e)

@state.change("current_representation")
def _on_representation_change(current_representation=None, **kwargs):
    if current_representation:
        update_representation(current_representation)

@state.change("solid_color")
def _on_solid_color_change(solid_color=None, **kwargs):
    if solid_color and state.current_color_array == 'SOLID':
        update_solid_color(solid_color)




def extract_slice():
    log.ui("Extract Slice clicked for: {}", state.editing_source)
    from trame_app import trame_pipeline as pipe
    pipe.extract_slice(_ctx, state.editing_source)
    state.show_edit_dialog = False

def extract_ghosts():
    log.ui("Extract Ghosts clicked for: {}", state.editing_source)
    from trame_app import trame_pipeline as pipe
    pipe.extract_ghosts(_ctx, state.editing_source)
    state.show_edit_dialog = False

def extract_scalar_field():
    log.ui("Extract Scalar Field clicked for: {}", state.editing_source)
    from trame_app import trame_pipeline as pipe
    pipe.extract_scalar_field(_ctx, state.editing_source)
    state.show_edit_dialog = False

def update_slice_normal(normal):
    log.ui("Update Slice Normal: {}", normal)
    ColorAPI(_ctx).set_slice_normal(normal)

def update_slice_origin(origin):
    # Debounce could be useful here, but for now direct update
    ColorAPI(_ctx).set_slice_origin(origin)

def update_opacity():
    # This function is now a wrapper around the change handler logic
    # to ensure consistency (sorting, etc.)
    _on_opacity_points_change()

def reset_opacity():
    state.opacity_points = [
        {'x': 0.0, 'y': 0.0, 'id': 0},
        {'x': 100.0, 'y': 1.0, 'id': 1}
    ]
    state.opacity_next_id = 2
    update_opacity()

def apply_and_close():
    log.ui("Apply and Close clicked for: {}", state.editing_source)
    # Only close the dialog; color changes are applied immediately via state triggers
    state.show_edit_dialog = False

def apply_color_map(preset_key):
    log.ui("Apply Color Map clicked: {} (source: {})", preset_key, state.editing_source)
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
        log.warn("Failed to apply preset '{}': {}", preset_name, e)
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
            log.warn("view_update failed: {}. Disabling further updates and live mode.", e)
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
    log.ui("Scalar Bar visibility toggled: {} (source: {})", desired_vis, state.editing_source)
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



def _get_workspace_parent_dir():
    try:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        # .../ippl-frk/src/Stream/InSitu/catalyst_scripts -> go up 4 to ippl-frk, 5 to parent workspace
        repo_root = os.path.abspath(os.path.join(script_dir, "..", "..", "..", ".."))
        parent_dir = os.path.abspath(os.path.join(repo_root, ".."))
        return parent_dir
    except Exception:
        return os.getcwd()


def take_screenshot():
    log.ui("Screenshot requested")
    view = simple.GetRenderView()
    if not view:
        log.warn("No active view; cannot capture screenshot")
        return
    raw_dir = getattr(state, 'screenshot_save_path', '') or _get_workspace_parent_dir()
    dest_dir = os.path.abspath(os.path.expanduser(raw_dir))
    try:
        os.makedirs(dest_dir, exist_ok=True)
    except Exception as e:
        log.warn("Could not create directory '{}': {}. Falling back to default path.", dest_dir, e)
        dest_dir = _get_workspace_parent_dir()
        os.makedirs(dest_dir, exist_ok=True)
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    filename = f"ippl_view_{timestamp}.png"
    file_path = os.path.join(dest_dir, filename)
    try:
        simple.SaveScreenshot(file_path, view=view)
        log.info("Screenshot saved to {}", file_path)
    except Exception as e:
        log.error("Failed to save screenshot: {}", e)


def reset_screenshot_path():
    log.ui("Reset screenshot path to default")
    try:
        state.screenshot_save_path = state.default_screenshot_path
    except Exception:
        state.screenshot_save_path = _get_workspace_parent_dir()

# Moved to trame_steering_config.py:
# - load_steerable_proxies
# - get_steerable_proxy
# - update_steering_parameter
# - parse_steerable_parameters

def apply_steering():
    from trame_app.trame_logging import ui, info, debug, warn
    ui("Apply Steering clicked")
    """Apply the current steering parameters."""
    debug("apply_steering called")
    link = getattr(_ctx, "catalyst_link", None)
    if not link:
        warn("Cannot apply steering: no Catalyst connection available in context.")
        return

    proxy_defs = getattr(state, "steerable_proxies", None) or steerable_proxies
    if not proxy_defs:
        warn("No steerable proxies defined; aborting apply.")
        return
    
    info("Applying steering to {} proxies", len(proxy_defs))

    for proxy_def in proxy_defs:
        proxy_name = proxy_def['name']
        proxy_safe_name = proxy_def['safe_name']
        is_array = proxy_def['type'] == 'array'
        
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
        num_rows = 1
        if is_array:
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
                            if is_array:
                                # Array proxy: state is a list, access via index
                                key = f"steer_{safe_name}_{i}"
                                try:
                                    v = state[key][row_idx]
                                except Exception:
                                    v = param['default'][i] if i < len(param['default']) else 0
                            else:
                                # Scalar proxy: state is a single value
                                key = f"steer_{safe_name}_{i}"
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
                        if is_array:
                            # Array proxy: state is a list, access via index
                            key = f"steer_{safe_name}"
                            try:
                                v = state[key][row_idx]
                            except Exception:
                                v = param['default'][0] if param['default'] else 0
                        else:
                            # Scalar proxy: state is a single value
                            key = f"steer_{safe_name}"
                            try:
                                v = state[key]
                            except Exception:
                                v = param['default'][0] if param['default'] else 0
                        if v is None:
                            v = param['default'][0] if param['default'] else 0
                        all_values.append(v)
                
                from trame_app.trame_logging import debug, error
                debug("Sending steering parameter {}.{} = {}", proxy_name, name, all_values)
                steering_config.update_steering_parameter(link, proxy_name, name, all_values)
                
            except Exception as e:
                error("Error applying parameter {}: {}", name, e)
                import traceback
                traceback.print_exc()

@ctrl.trigger("flush_and_apply_steering")
def flush_and_apply_steering(array_data=None):
    """Apply steering with array values passed directly from client.
    
    This is needed because Vue's in-place array mutations don't trigger Trame's
    change detection. The client passes the current array values directly.
    """
    log.debug("flush_and_apply_steering called")
    log.debug("Received array_data: {}", array_data)
    
    if array_data:
        # Update state with the values passed from the client
        # AND update the count for each array proxy based on array length
        proxy_counts = {}  # Track new counts per proxy
        
        for key, values in array_data.items():
            try:
                state[key] = list(values)
                log.debug("Updated {}: {}", key, state[key])
                
                # Extract proxy name from key to update count
                # Keys are like: steer_{safe_name} or steer_{safe_name}_{i}
                # We need to find which proxy this belongs to and update its count
                for proxy_def in steerable_proxies:
                    if proxy_def['type'] == 'array':
                        proxy_safe_name = proxy_def['safe_name']
                        # Check if this key belongs to this proxy
                        if key.startswith(f"steer_"):
                            # Get the parameter name part
                            key_suffix = key[6:]  # Remove "steer_" prefix
                            
                            # Check if this key matches any parameter in this proxy
                            flat_params = []
                            def collect_params(items):
                                for item in items:
                                    if item['item_type'] == 'group':
                                        collect_params(item['children'])
                                    else:
                                        flat_params.append(item)
                            collect_params(proxy_def['children'])
                            
                            for param in flat_params:
                                safe_name = param['safe_name']
                                # Check if key matches this param (with or without component suffix)
                                if key == f"steer_{safe_name}" or key.startswith(f"steer_{safe_name}_"):
                                    # This key belongs to this proxy
                                    new_count = len(values)
                                    if proxy_safe_name not in proxy_counts:
                                        proxy_counts[proxy_safe_name] = new_count
                                    # All arrays in the same proxy should have the same length
                                    if proxy_counts[proxy_safe_name] != new_count:
                                        log.warn("Inconsistent array lengths for proxy {}: {} vs {}", proxy_safe_name, proxy_counts[proxy_safe_name], new_count)
                                    break
                            
            except Exception as e:
                log.debug("Error updating {}: {}", key, e)
        
        # Update counts for all affected array proxies
        for proxy_safe_name, new_count in proxy_counts.items():
            count_key = f"count_{proxy_safe_name}"
            old_count = state.get(count_key, 0)
            if old_count != new_count:
                log.debug("Updating {}: {} -> {}", count_key, old_count, new_count)
                state[count_key] = new_count
    
    # Now apply the steering with fresh state
    apply_steering()

@ctrl.trigger("apply_steering_triggered")
def apply_steering_triggered():
    """Called after flushState() completes to apply steering with fresh state."""
    # Force reassignment of all steering array state variables to ensure sync
    # This is needed because Vue's in-place array mutations may not trigger Trame's change detection
    for proxy_def in steerable_proxies:
        if proxy_def['type'] == 'array':
            proxy_safe_name = proxy_def['safe_name']
            flat_params = []
            def collect_params(items):
                for item in items:
                    if item['item_type'] == 'group':
                        collect_params(item['children'])
                    else:
                        flat_params.append(item)
            collect_params(proxy_def['children'])
            
            for param in flat_params:
                safe_name = param['safe_name']
                if param['num_elements'] > 1:
                    for i in range(param['num_elements']):
                        key = f"steer_{safe_name}_{i}"
                        try:
                            current = state[key]
                            if isinstance(current, list):
                                log.debug("Forcing sync for {}: {}", key, current)
                        except Exception:
                            pass
                else:
                    key = f"steer_{safe_name}"
                    try:
                        current = state[key]
                        if isinstance(current, list):
                            log.debug("Forcing sync for {}: {}", key, current)
                    except Exception:
                        pass
    
    apply_steering()

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
# poll_catalyst removed (duplicate of trame_runtime.poll_catalyst)



        
def pause_sim():
    log.ui("Pause clicked")
    link = getattr(_ctx, 'catalyst_link', None)
    try:
        if link:
            live.PauseCatalyst(link, pause=True)
            state.sim_paused = True
    except Exception as e:
        log.warn("Failed to pause simulation: {}", e)

def unpause_sim():
    log.ui("Unpause clicked")
    link = getattr(_ctx, 'catalyst_link', None)
    try:
        if link:
            live.PauseCatalyst(link, pause=False)
            state.sim_paused = False
    except Exception as e:
        log.warn("Failed to unpause simulation: {}", e)

def toggle_simulation():
    log.ui("Toggle Simulation clicked")
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
        log.warn("Failed to toggle simulation: {}", e)

def toggle_axes_grid():
    log.ui("Toggle Axes Grid clicked")
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
            log.warn("view_update failed: {}. Disabling further updates and live mode.", e)
            view_update_enabled = False
            state.live_mode = False
            state.status_text = "Live disabled: transport error"

# Initialize steerable proxies
steerable_proxies = steering_config.parse_steerable_parameters()

# Pre-compute steering array keys for the Apply button's JS handler
# We need these before the UI is built so we can generate the explicit JS code
steering_array_keys = []
for proxy_def in steerable_proxies:
    if proxy_def['type'] == 'array':
        flat_params = []
        def _collect_params(items):
            for item in items:
                if item['item_type'] == 'group':
                    _collect_params(item['children'])
                else:
                    flat_params.append(item)
        _collect_params(proxy_def['children'])
        for param in flat_params:
            safe_name = param['safe_name']
            if param['num_elements'] > 1:
                for i in range(param['num_elements']):
                    steering_array_keys.append(f"steer_{safe_name}_{i}")
            else:
                steering_array_keys.append(f"steer_{safe_name}")
log.debug("Steering array keys (computed early): {}", steering_array_keys)

# Build the JS code for the Apply button to reassign all arrays before flushing
if steering_array_keys:
    # Build explicit reassignments like: steer_pos_0 = [...steer_pos_0]; steer_pos_1 = [...steer_pos_1]; ...
    reassign_js = "; ".join([f"{k} = [...{k}]" for k in steering_array_keys])
    steering_apply_click = f"{reassign_js}; flushState(); $nextTick(() => trigger('apply_steering_triggered'))"
else:
    steering_apply_click = "flushState(); $nextTick(() => trigger('apply_steering_triggered'))"

# -----------------------------------------------------------------------------
# Opacity Point Controller Functions (must be defined before UI references them)
# -----------------------------------------------------------------------------
def _add_opacity_point():
    log.debug("add_opacity_point TRIGGERED")
    points = list(state.opacity_points)
    new_id = state.opacity_next_id
    state.opacity_next_id += 1
    points.append({'x': 50.0, 'y': 0.5, 'id': new_id})
    state.opacity_points = points

ctrl.add_opacity_point = _add_opacity_point

def _remove_opacity_point(id_to_remove):
    log.debug("remove_opacity_point TRIGGERED: id={}", id_to_remove)
    try:
        id_to_remove = int(id_to_remove)
    except (ValueError, TypeError):
        pass
    points = [p for p in state.opacity_points if p['id'] != id_to_remove]
    state.opacity_points = points

ctrl.remove_opacity_point = _remove_opacity_point

def _update_opacity_point(index, id, field, value, *extras):
    log.debug("update_opacity_point TRIGGERED: index={}, id={}, field={}, value={}", index, id, field, value)
    
    try:
        idx = int(index)
    except (ValueError, TypeError):
        idx = None

    try:
        search_id = int(id)
    except (ValueError, TypeError):
        search_id = id

    points = list(state.opacity_points)
    log.debug("Snapshot before update: {}", points)

    target_index = None

    if idx is not None and 0 <= idx < len(points):
        candidate_id = points[idx].get('id')
        try:
            candidate_id_int = int(candidate_id)
        except (ValueError, TypeError):
            candidate_id_int = candidate_id
        if candidate_id_int == search_id:
            target_index = idx

    if target_index is None:
        for i, p in enumerate(points):
            pid = p.get('id')
            try:
                pid_int = int(pid)
            except (ValueError, TypeError):
                pid_int = pid
            if pid_int == search_id:
                target_index = i
                break

    if target_index is None:
        log.debug("No point found with id {}", search_id)
        return

    point = points[target_index]
    before_val = point.get(field)

    try:
        new_val = float(value)
    except (TypeError, ValueError) as err:
        log.warn("Invalid incoming value '{}' ({}): {}", value, type(value), err)
        return

    try:
        before_float = float(before_val)
    except (TypeError, ValueError):
        before_float = None

    if before_float is not None and abs(before_float - new_val) <= 1e-6:
        return

    new_point = dict(point)
    new_point[field] = new_val
    points[target_index] = new_point

    log.debug("Updating point id={} at index={}: {} -> {}", point.get('id'), target_index, before_float, new_val)
    state.opacity_points = points

ctrl.update_opacity_point = _update_opacity_point

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



                        vuetify3.VBtn("Apply", click=steering_apply_click, disabled=("!connected",), color="primary", size="small", variant="flat", classes="text-white")

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
        
        # order of GUI elements (left-to-right): live switch, play/pause, grid toggle, separator, connect toggle, connected status
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

        with vuetify3.VBtn(icon=True, click=take_screenshot, variant="text", color="teal-accent-3", classes="mr-1"):
            vuetify3.VIcon("mdi-camera")
            vuetify3.VTooltip(activator="parent", location="bottom", text="Save screenshot (PNG)")

        with vuetify3.VMenu(v_model=("screenshot_menu_open", False), open_on_hover=False, close_on_content_click=False):
            with vuetify3.Template(v_slot_activator="{ props }"):
                with vuetify3.VBtn(v_bind="props", icon=True, variant="text", color="teal-accent-1", classes="mr-2"):
                    vuetify3.VIcon("mdi-menu-down")
                    vuetify3.VTooltip(activator="parent", location="bottom", text="Screenshot settings")
            with vuetify3.VCard(style="min-width: 320px;"):
                vuetify3.VCardTitle("Screenshot Settings", classes="text-subtitle-2 pb-1")
                vuetify3.VDivider()
                with vuetify3.VCardText(classes="pt-3"):
                    vuetify3.VTextField(
                        v_model=("screenshot_save_path",),
                        label="Save directory",
                        variant="outlined",
                        density="compact",
                        hide_details=False,
                        hint="Directory for PNG captures",
                        persistent_hint=True,
                    )
                with vuetify3.VCardActions():
                    vuetify3.VBtn("Use default", variant="text", click=reset_screenshot_path)
                    vuetify3.VSpacer()
                    vuetify3.VBtn("Close", variant="text", click="screenshot_menu_open = false")

        vuetify3.VDivider(vertical=True, classes="mx-4")

        # Connection Settings Menu
        with vuetify3.VMenu(v_model=("connection_menu_open", False), open_on_hover=False, close_on_content_click=False):
            with vuetify3.Template(v_slot_activator="{ props }"):
                with vuetify3.VBtn(v_bind="props", icon=True, variant="text", color="grey", classes="mr-2", disabled=("connected",)):
                    vuetify3.VIcon("mdi-cog")
                    vuetify3.VTooltip(activator="parent", location="bottom", text="Connection Settings")
            with vuetify3.VCard(style="min-width: 300px;"):
                vuetify3.VCardTitle("Catalyst Connection", classes="text-subtitle-2 pb-1")
                vuetify3.VDivider()
                with vuetify3.VCardText(classes="pt-3"):
                    vuetify3.VTextField(
                        v_model=("catalyst_host", "localhost"),
                        label="Host",
                        variant="outlined",
                        density="compact",
                        hide_details=False,
                        classes="mb-3"
                    )
                    vuetify3.VTextField(
                        v_model=("catalyst_port", 22222),
                        label="Port",
                        variant="outlined",
                        density="compact",
                        hide_details=False,
                        type="number"
                    )
                with vuetify3.VCardActions():
                    vuetify3.VSpacer()
                    vuetify3.VBtn("Close", variant="text", click="connection_menu_open = false")

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
                    # Extract Slice Button for sField or Resampled vField
                    with vuetify3.VContainer(v_if="(editing_source.startsWith('ippl_sField') || editing_source.includes('.Resample')) && !editing_source.includes('.Slice')", classes="pa-0 mb-2"):
                        with vuetify3.VRow(dense=True):
                            with vuetify3.VCol(cols=6):
                                vuetify3.VBtn("Extract Slice", click=extract_slice, block=True, color="secondary", variant="tonal")
                            with vuetify3.VCol(cols=6):
                                vuetify3.VBtn("Extract Ghosts", click=extract_ghosts, block=True, color="deep-orange", variant="tonal", disabled=("editing_source.includes('.Ghosts')",))
                        vuetify3.VDivider(classes="my-2")

                    # Extract Scalar Field Button for vField (only for base vField, not extracted parts)
                    with vuetify3.VContainer(v_if="editing_source.startsWith('ippl_vField') && !editing_source.includes('.Resample') && !editing_source.includes('.Slice') && !editing_source.includes('.Ghosts')", classes="pa-0 mb-2"):
                        vuetify3.VBtn("Extract Scalar Field", click=extract_scalar_field, block=True, color="primary", variant="tonal")
                        vuetify3.VDivider(classes="my-2")

                    # Slice Controls
                    with vuetify3.VContainer(v_if="editing_source.includes('.Slice')", classes="pa-0 mb-2"):
                        vuetify3.VLabel(text="Slice Origin", classes="text-caption font-weight-bold")
                        with vuetify3.VRow(dense=True):
                            with vuetify3.VCol(cols=12, classes="py-0"):
                                vuetify3.VSlider(v_model=("slice_origin_x",), min=("slice_bounds[0]",), max=("slice_bounds[1]",), step="any", density="compact", hide_details=True, label="X", thumb_label=True)
                            with vuetify3.VCol(cols=12, classes="py-0"):
                                vuetify3.VSlider(v_model=("slice_origin_y",), min=("slice_bounds[2]",), max=("slice_bounds[3]",), step="any", density="compact", hide_details=True, label="Y", thumb_label=True)
                            with vuetify3.VCol(cols=12, classes="py-0"):
                                vuetify3.VSlider(v_model=("slice_origin_z",), min=("slice_bounds[4]",), max=("slice_bounds[5]",), step="any", density="compact", hide_details=True, label="Z", thumb_label=True)
                        
                        vuetify3.VLabel(text="Slice Normal", classes="text-caption font-weight-bold mt-2")
                        with vuetify3.VRow(dense=True):
                            with vuetify3.VCol(cols=4):
                                vuetify3.VTextField(v_model=("slice_normal_x",), label="X", type="number", step="0.1", density="compact", variant="outlined", hide_details=True)
                            with vuetify3.VCol(cols=4):
                                vuetify3.VTextField(v_model=("slice_normal_y",), label="Y", type="number", step="0.1", density="compact", variant="outlined", hide_details=True)
                            with vuetify3.VCol(cols=4):
                                vuetify3.VTextField(v_model=("slice_normal_z",), label="Z", type="number", step="0.1", density="compact", variant="outlined", hide_details=True)
                        
                        # Quick Normal Buttons
                        with vuetify3.VRow(dense=True, classes="mt-1"):
                            with vuetify3.VCol(cols=12, classes="d-flex justify-center"):
                                with vuetify3.VBtnToggle(density="compact", variant="outlined", color="primary"):
                                    vuetify3.VBtn("X", click="slice_normal_x=1; slice_normal_y=0; slice_normal_z=0")
                                    vuetify3.VBtn("Y", click="slice_normal_x=0; slice_normal_y=1; slice_normal_z=0")
                                    vuetify3.VBtn("Z", click="slice_normal_x=0; slice_normal_y=0; slice_normal_z=1")

                        vuetify3.VDivider(classes="my-2")

                    vuetify3.VSelect(
                        v_model=("current_representation",),
                        items=("available_representations", []),
                        label="Representation",
                        density="compact",
                        variant="outlined",
                    )
                    vuetify3.VDivider(classes="my-2")
                    vuetify3.VSelect(
                        v_model=("current_color_array",),
                        items=("color_arrays", []),
                        label="Color By",
                        item_title="title",
                        item_value="value",
                        density="compact",
                        variant="outlined",
                    )

                    # Component Selection for Vector Arrays
                    vuetify3.VSelect(
                        v_if="color_array_is_vector",
                        v_model=("current_color_component",),
                        items=("['Magnitude', 'X', 'Y', 'Z']",),
                        label="Component",
                        density="compact",
                        variant="outlined",
                        classes="mt-2"
                    )
                    
                    with vuetify3.VContainer(v_if="current_color_array === 'SOLID'", classes="pa-0"):
                        vuetify3.VLabel(text="Solid Color", classes="text-caption")
                        vuetify3.VColorPicker(
                            v_model=("solid_color",),
                            mode="hex",
                            hide_inputs=True,
                            hide_mode_switch=True,
                            show_swatches=True,
                            elevation=0,
                            canvas_height=100,
                        )

                    vuetify3.VDivider(classes="my-2")
                    vuetify3.VSwitch(
                        v_model=("scalar_bar_visible", False),
                        label="Show Colour Bar",
                        inset=True,
                        density="comfortable",
                        color=("scalar_bar_visible ? 'green' : 'grey'",),
                        disabled=("current_color_array === 'SOLID'",),
                        # change handled via state.change hook
                    )
                    vuetify3.VDivider(classes="my-2")
                    
                    # Rescale Controls
                    vuetify3.VSwitch(
                        v_model=("auto_rescale_color", True),
                        label="Auto Rescale Range",
                        inset=True,
                        density="comfortable",
                        color="primary",
                        disabled=("current_color_array === 'SOLID'",),
                    )
                    
                    vuetify3.VSwitch(
                        v_model=("symmetric_rescale", True),
                        label="Symmetric Rescale (around 0)",
                        inset=True,
                        density="comfortable",
                        color="primary",
                        disabled=("current_color_array === 'SOLID' || !auto_rescale_color",),
                    )
                    
                    with vuetify3.VContainer(v_if="!auto_rescale_color && current_color_array !== 'SOLID'", classes="pa-0 mb-2"):
                        vuetify3.VLabel(text="Custom Range", classes="text-caption")
                        with vuetify3.VRow(dense=True):
                            with vuetify3.VCol(cols=6):
                                vuetify3.VTextField(v_model=("custom_rescale_min",), label="Min", type="number", density="compact", variant="outlined", hide_details=True)
                            with vuetify3.VCol(cols=6):
                                vuetify3.VTextField(v_model=("custom_rescale_max",), label="Max", type="number", density="compact", variant="outlined", hide_details=True)
                        vuetify3.VBtn("Apply Range", click=apply_custom_range, block=True, size="small", variant="tonal", classes="mt-2")

                    vuetify3.VDivider(classes="my-2")
                    vuetify3.VSelect(
                        v_model=("current_color_map",),
                        items=("available_color_maps", []),
                        label="Color Map",
                        item_title="title",
                        item_value="value",
                        density="compact",
                        variant="outlined",
                        disabled=("current_color_array === 'SOLID'",),
                        # Rely on state change hook to apply preset
                    )
                    
                    vuetify3.VDivider(classes="my-2")
                    with vuetify3.VContainer(v_if="current_color_array !== 'SOLID'", classes="pa-0"):
                        vuetify3.VLabel(text="Opacity Map (Normalized)", classes="text-caption")
                        
                        # Header
                        with vuetify3.VRow(dense=True, classes="mb-1"):
                            with vuetify3.VCol(cols=5): vuetify3.VLabel(text="Position", classes="text-caption")
                            with vuetify3.VCol(cols=5): vuetify3.VLabel(text="Opacity", classes="text-caption")
                        
                        # Dynamic List - bind directly to state array indices
                        with vuetify3.VContainer(classes="pa-0", style="max-height: 200px; overflow-y: auto;"):
                            with vuetify3.VRow(v_for="(item, i) in opacity_points", key="item.id", dense=True, classes="align-center mb-1"):
                                with vuetify3.VCol(cols=5):
                                    vuetify3.VTextField(
                                        v_model=("opacity_points[i].x",),
                                        type="number", min=0, max=100, step="any", density="compact", hide_details=True, variant="outlined"
                                    )
                                with vuetify3.VCol(cols=5):
                                    vuetify3.VSlider(
                                        v_model=("opacity_points[i].y",),
                                        min=0, max=1, step=0.01, density="compact", hide_details=True,
                                        thumb_label="always"
                                    )
                                with vuetify3.VCol(cols=2):
                                    vuetify3.VBtn(icon="mdi-delete", size="x-small", variant="text", color="error", click=(ctrl.remove_opacity_point, "[item.id]"))

                        vuetify3.VBtn("Add Point", click=ctrl.add_opacity_point, block=True, size="small", variant="tonal", classes="mt-2 mb-2")

                        with vuetify3.VRow(dense=True):
                            with vuetify3.VCol(cols=6):
                                # Force state sync by reassigning the array, then call update
                                vuetify3.VBtn(
                                    "Apply Opacity",
                                    click="opacity_points = [...opacity_points]; flushState('opacity_points')",
                                    block=True, size="small", variant="tonal", color="primary"
                                )
                            with vuetify3.VCol(cols=6):
                                vuetify3.VBtn("Reset", click=reset_opacity, block=True, size="small", variant="text")

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
state.color_array_is_vector = False
state.current_color_component = 'Magnitude'
state.solid_color = "#ffffff"
state.available_representations = []
state.current_representation = None
state.scalar_bar_visible = False
state.slice_normal_x = 1.0
state.slice_normal_y = 0.0
state.slice_normal_z = 0.0
state.slice_origin_x = 0.0
state.slice_origin_y = 0.0
state.slice_origin_z = 0.0
state.slice_bounds = [-1, 1, -1, 1, -1, 1]
state.available_color_maps = []
state.current_color_map = None
state.color_map_per_source = {}
state.symmetric_rescale = True
state.opacity_points = []
state.opacity_next_id = 0
state.loading_opacity = False
state.default_screenshot_path = _get_workspace_parent_dir()
state.screenshot_save_path = state.default_screenshot_path
state.screenshot_menu_open = False

# Initialize state for steerable parameters
state.selected_steering_proxy = steerable_proxies[0]['name'] if steerable_proxies else None

# Track all steering array keys for state flushing
steering_array_keys = []

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
                    steering_array_keys.append(key)  # Track for flushing
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
                steering_array_keys.append(key)  # Track for flushing
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

# Store the list of steering array keys for client-side flushing
state.steering_array_keys = steering_array_keys
log.debug("Steering array keys to flush: {}", steering_array_keys)

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
    log.debug("Connected changed: {}", connected)
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
    log.debug("Status color update on connected={}", connected)
    state.status_color = "success" if connected else "grey"

# Log Live mode toggles
@state.change("live_mode")
def _on_live_mode_change(live_mode=None, **kwargs):
    try:
        log.ui("Live mode toggled: {}", bool(live_mode))
    except Exception:
        pass

# Initialize shared context and wire runtime resume
_ctx = Ctx(server=server, state=state, ctrl=ctrl)
resume_polling(_ctx)

@state.change("auto_rescale_color", "symmetric_rescale")
def _on_rescale_mode_change(**kwargs):
    try:
        ColorAPI(_ctx).apply_auto_rescale()
    except Exception:
        pass

@state.change("auto_rescale_color", "custom_rescale_min", "custom_rescale_max", "symmetric_rescale")
def _on_rescale_settings_change(**kwargs):
    try:
        ColorAPI(_ctx).save_rescale_settings()
    except Exception:
        pass

def update_color_component(value):
    ColorAPI(_ctx).set_color_component(value)

@state.change("current_color_component")
def _on_color_component_change(current_color_component=None, **kwargs):
    if current_color_component:
        update_color_component(current_color_component)

@state.change("current_color_array")
def _on_color_array_change(current_color_array=None, **kwargs):
    if current_color_array:
        update_color_by(current_color_array)

@state.change("opacity_points")
def _on_opacity_points_change(**kwargs):
    log.debug("_on_opacity_points_change triggered.")
    if state.loading_opacity:
        log.debug("loading_opacity is True, skipping update.")
        return
    # Sort points by x before sending
    points = sorted(state.opacity_points, key=lambda p: float(p['x']))

    log.debug("Sending points to ColorAPI: {}", points)
    ColorAPI(_ctx).update_opacity_points(points)

# Update the button callback to use the sorted logic too
def update_opacity():
    log.debug("Manual Apply Opacity triggered via button")
    _on_opacity_points_change()

def reset_opacity():
    state.opacity_points = [
        {'x': 0.0, 'y': 1.0, 'id': 0},
        {'x': 100.0, 'y': 1.0, 'id': 1}
    ]
    state.opacity_next_id = 2
    update_opacity()

if __name__ == "__main__":
    import argparse
    from trame_app.trame_logging import set_log_level
    
    parser = argparse.ArgumentParser(description='Trame Catalyst Visualization App')
    parser.add_argument('--debug', type=int, default=0, choices=[-1, 0, 1, 2],
                        help='Debug level: -1=silent, 0=UI only (default), 1=moderate, 2=detailed')
    # Use parse_known_args() to allow Trame server arguments like --server, --port, etc.
    args, remaining = parser.parse_known_args()
    
    set_log_level(args.debug)
    
    simple.GetRenderView()
    server.start()