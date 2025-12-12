from paraview import simple

# Pipeline helper import needs to work whether we run as a package or standalone script
try:
    from . import trame_pipeline as _pipeline
except Exception:
    try:
        from trame_app import trame_pipeline as _pipeline  # type: ignore
    except Exception:
        try:
            import trame_pipeline as _pipeline  # type: ignore
        except Exception:
            _pipeline = None

from catalystSubroutines import find_source_by_name

class ColorAPI:
    def __init__(self, ctx):
        self.ctx = ctx
        self.state = ctx.state
        self.ctrl = ctx.ctrl

    def get_display_proxy(self, name):
        """Return the display proxy for the named pipeline item."""
        pipe = _pipeline
        if pipe:
            try:
                proxy = pipe.get_display_proxy(self.ctx, name)
                if proxy:
                    return proxy
            except Exception as e:
                print(f"[WARN] Failed to get display proxy via pipeline helper: {e}")

        # Fallbacks: check active_proxies cache, then legacy name patterns
        proxy = getattr(self.ctx, "active_proxies", {}).get(name)
        if proxy:
            return proxy

        return self._find_display_proxy_by_name(name)

    def _find_display_proxy_by_name(self, name):
        """Legacy search copied from the pre-refactor implementation."""
        try:
            if name.endswith(".bunch") or name.endswith(".box"):
                return find_source_by_name(name)
            elif name.startswith("ippl_sField"):
                for candidate in (
                    f"{name}_Resample",
                    f"{name}.MergedBlocks",
                    name,
                ):
                    proxy = find_source_by_name(candidate)
                    if proxy:
                        return proxy
            elif name.startswith("ippl_vField"):
                for candidate in (
                    f"{name}_Glyph",
                    f"{name}.Glyph",
                    name,
                ):
                    proxy = find_source_by_name(candidate)
                    if proxy:
                        return proxy
            elif name.startswith("ippl_particles"):
                for candidate in (
                    f"{name}.bunch",
                    f"{name}.box",
                    name,
                ):
                    proxy = find_source_by_name(candidate)
                    if proxy:
                        return proxy
            else:
                return find_source_by_name(name)
        except Exception as e:
            print(f"[WARN] Legacy display proxy lookup failed: {e}")
        return None

    def _update_available_arrays(self, info):
        arrays = []
        pd = info.GetPointDataInformation()
        for i in range(pd.GetNumberOfArrays()):
            ai = pd.GetArrayInformation(i)
            name = ai.GetName()
            if name:
                arrays.append({'title': f"{name} (Points)", 'value': f"POINTS:{name}"})
        
        cd = info.GetCellDataInformation()
        for i in range(cd.GetNumberOfArrays()):
            ai = cd.GetArrayInformation(i)
            name = ai.GetName()
            if name:
                arrays.append({'title': f"{name} (Cells)", 'value': f"CELLS:{name}"})
        
        arrays.insert(0, {'title': 'Solid Color', 'value': 'SOLID'})
        
        # Debug: Print found arrays
        print(f"[UI] Available Arrays for {self.state.editing_source}: {[a['value'] for a in arrays]}")
        
        self.state.color_arrays = arrays

    def open_edit_dialog(self, name):
        print(f"[UI] Open Edit Dialog clicked: {name}")
        s = self.state
        s.editing_source = name
        s.show_edit_dialog = True

        proxy = self.get_display_proxy(name)
        if not proxy:
            s.color_arrays = []
            s.current_color_array = None
            return

        rep = simple.GetRepresentation(proxy, simple.GetActiveView())
        if not rep:
            s.color_arrays = []
            s.current_color_array = None
            s.scalar_bar_visible = False
            return

        info = proxy.GetDataInformation()
        self._update_available_arrays(info)

        ca = rep.ColorArrayName
        if ca and len(ca) > 1 and ca[0] and ca[1]:
            val = f"{ca[0]}:{ca[1]}"
            # Check if valid
            valid_values = [a['value'] for a in s.color_arrays]
            if val in valid_values:
                s.current_color_array = val
            else:
                print(f"[Warn] Current color {val} not found in available arrays: {valid_values}")
                # Fallback: try to find partial match or default
                found = False
                for v in valid_values:
                    if ca[1] in v:
                        print(f"[Auto] Matching {val} to {v}")
                        s.current_color_array = v
                        found = True
                        break
                if not found:
                    s.current_color_array = 'SOLID'
        else:
            s.current_color_array = 'SOLID'

        try:
            view = simple.GetActiveView()
            if ca and len(ca) > 1 and ca[1]:
                lut = simple.GetColorTransferFunction(ca[1])
                sb = simple.GetScalarBar(lut, view)
                if sb is not None:
                    s.scalar_bar_visible = bool(getattr(sb, 'Visibility', 0))
                else:
                    s.scalar_bar_visible = bool(rep.GetScalarBarVisibility(view))

            else:
                s.scalar_bar_visible = False
        except Exception:
            s.scalar_bar_visible = False

        s.available_color_maps = [
            { 'title': 'Cool to Warm', 'value': 'cool_to_warm' },
            { 'title': 'Cool to Warm (Extended)', 'value': 'cool_to_warm_ext' },
            { 'title': 'Cold and Hot', 'value': 'cold_and_hot' },
            { 'title': 'Viridis', 'value': 'viridis' },
            { 'title': 'Inferno', 'value': 'inferno' },
            { 'title': 'Black-Body Radiation', 'value': 'black_body' },
        ]
        if s.current_color_array != 'SOLID':
            try:
                saved = s.color_map_per_source.get(name)
                s.current_color_map = saved if saved else 'cool_to_warm'
            except Exception:
                s.current_color_map = 'cool_to_warm'
        else:
            s.current_color_map = None

        # Initialize Rescale State
        # Load from per-source settings if available
        try:
            settings = s.rescale_settings_per_source.get(name, {})
        except Exception:
            settings = {}
            s.rescale_settings_per_source = {}

        s.auto_rescale_color = settings.get('auto', True)
        s.symmetric_rescale = settings.get('symmetric', True)
        s.custom_rescale_min = settings.get('min', 0.0)
        s.custom_rescale_max = settings.get('max', 1.0)

        # Initialize Slice state if applicable
        if ".Slice" in name:
            try:
                # Assuming SliceType is Plane
                normal = list(proxy.SliceType.Normal)
                origin = list(proxy.SliceType.Origin)
                
                # Use scalars for stability
                s.slice_normal_x, s.slice_normal_y, s.slice_normal_z = normal
                s.slice_origin_x, s.slice_origin_y, s.slice_origin_z = origin
                
                # Set bounds for sliders based on input bounds
                input_proxy = proxy.Input
                if input_proxy:
                    input_proxy.UpdatePipeline() # Ensure bounds are fresh
                    input_info = input_proxy.GetDataInformation()
                    bounds = input_info.GetBounds()
                    # Check if bounds are valid (initialized)
                    if bounds[0] > bounds[1]:
                         # Fallback if bounds invalid
                         s.slice_bounds = [-1, 1, -1, 1, -1, 1]
                    else:
                        s.slice_bounds = list(bounds)
                else:
                    s.slice_bounds = [-1, 1, -1, 1, -1, 1]

            except Exception as e:
                print(f"[WARN] Failed to init slice state: {e}")

        # Initialize solid color state
        try:
            if hasattr(rep, 'DiffuseColor'):
                rgb = rep.DiffuseColor
                # Convert [0-1] RGB to hex string for UI
                r = int(rgb[0] * 255)
                g = int(rgb[1] * 255)
                b = int(rgb[2] * 255)
                s.solid_color = f"#{r:02x}{g:02x}{b:02x}"
            else:
                s.solid_color = "#ffffff"
        except Exception:
            s.solid_color = "#ffffff"

        # Initialize Opacity State
        try:
            if ca and len(ca) > 1 and ca[1]:
                sof = simple.GetOpacityTransferFunction(ca[1])
                lut = simple.GetColorTransferFunction(ca[1])
                
                # lut.GetRange() fails in some environments, use RGBPoints to infer range
                rgb_points = lut.RGBPoints
                if rgb_points and len(rgb_points) >= 4:
                    min_val = rgb_points[0]
                    max_val = rgb_points[-4]
                else:
                    min_val = 0.0
                    max_val = 1.0
                
                width = max_val - min_val
                if width == 0: width = 1.0
                
                num_samples = 5
                
                # Parse existing points from proxy
                # Format: [x, y, mid, sharp, ...]
                points_flat = sof.Points
                current_points = []
                if points_flat:
                    for k in range(0, len(points_flat), 4):
                        val = points_flat[k]
                        opacity = points_flat[k+1]
                        # Normalize x
                        norm_x = (val - min_val) / width if width > 0 else 0
                        norm_x = max(0.0, min(1.0, norm_x))
                        # Store as 0-100
                        current_points.append({'x': norm_x * 100.0, 'y': opacity})
                
                # Ensure at least 2 points
                if not current_points:
                    current_points = [{'x': 0.0, 'y': 0.0}, {'x': 100.0, 'y': 1.0}]
                
                # Assign IDs for UI tracking
                for i, p in enumerate(current_points):
                    p['id'] = i
                
                s.opacity_points = current_points
                s.opacity_next_id = len(current_points)
            else:
                # Default linear ramp
                s.opacity_points = [
                    {'x': 0.0, 'y': 0.0, 'id': 0},
                    {'x': 25.0, 'y': 0.25, 'id': 1},
                    {'x': 50.0, 'y': 0.5, 'id': 2},
                    {'x': 75.0, 'y': 0.75, 'id': 3},
                    {'x': 100.0, 'y': 1.0, 'id': 4}
                ]
                s.opacity_next_id = 5
        except Exception as e:
            print(f"[WARN] Failed to init opacity state: {e}")
            s.opacity_points = [
                {'x': 0.0, 'y': 0.0, 'id': 0},
                {'x': 100.0, 'y': 1.0, 'id': 1}
            ]
            s.opacity_next_id = 2

        # Representation logic
        try:
            current_rep = rep.Representation
            # Ensure we store a clean string, not a ParaView property object
            if hasattr(current_rep, 'GetData'):
                current_rep = current_rep.GetData()
            s.current_representation = str(current_rep).strip("'\"")
            
            if name.endswith(".bunch"):
                s.available_representations = ['Points', 'Point Gaussian', 'Outline']
            elif name.endswith(".box"):
                s.available_representations = ['Outline', 'Points']
            elif ".Ghosts" in name:
                s.available_representations = ['Surface', 'Wireframe', 'Points', 'Outline', 'Volume', 'Feature Edges']
            elif ".Slice" in name:
                s.available_representations = ['Surface', 'Wireframe', 'Points', 'Outline']
            elif name.startswith("ippl_sField") or ".Resample" in name:
                # Prefer Volume for sField and Resample filters
                s.available_representations = ['Volume', 'Surface', 'Wireframe', 'Points', 'Outline']
            else:
                # Default fallback (including vector fields)
                s.available_representations = ['Surface', 'Wireframe', 'Points', 'Outline']
        except Exception as e:
            print(f"[WARN] Failed to setup representation options: {e}")
            s.available_representations = []
            s.current_representation = None

    def set_slice_normal(self, normal):
        print(f"[UI] Set Slice Normal: {normal}")
        name = self.state.editing_source
        proxy = self.get_display_proxy(name)
        if not proxy or ".Slice" not in name: return
        
        try:
            proxy.SliceType.Normal = [float(x) for x in normal]
            simple.Render()
            self._safe_view_update()
        except Exception as e:
            print(f"[WARN] Failed to set slice normal: {e}")

    def set_slice_origin(self, origin):
        # print(f"[UI] Set Slice Origin: {origin}") # Verbose
        name = self.state.editing_source
        proxy = self.get_display_proxy(name)
        if not proxy or ".Slice" not in name: return
        
        try:
            proxy.SliceType.Origin = [float(x) for x in origin]
            simple.Render()
            self._safe_view_update()
        except Exception as e:
            print(f"[WARN] Failed to set slice origin: {e}")

    def set_solid_color(self, hex_color):
        print(f"[UI] Set Solid Color: {hex_color}")
        name = self.state.editing_source
        if not name: return
        
        proxy = self.get_display_proxy(name)
        if not proxy: return
        
        rep = simple.GetRepresentation(proxy, simple.GetActiveView())
        if not rep: return
        
        try:
            # Convert hex to [0-1] RGB
            if hex_color.startswith('#'):
                hex_color = hex_color[1:]
            if len(hex_color) == 6:
                r = int(hex_color[0:2], 16) / 255.0
                g = int(hex_color[2:4], 16) / 255.0
                b = int(hex_color[4:6], 16) / 255.0
                rep.DiffuseColor = [r, g, b]
                # Also set AmbientColor to match for some representations
                if hasattr(rep, 'AmbientColor'):
                    rep.AmbientColor = [r, g, b]
                
                simple.Render()
                self._safe_view_update()
        except Exception as e:
            print(f"[WARN] Failed to set solid color: {e}")

    def set_representation(self, mode):
        # Handle potential list input from UI
        if isinstance(mode, list):
            if len(mode) > 0:
                mode = mode[0]
            else:
                return

        # Strip quotes if present (defensive fix for some UI bindings)
        if isinstance(mode, str):
            mode = mode.strip("'\"")
            
        print(f"[UI] Set Representation: {mode} (type: {type(mode)})")
        
        # Ensure mode is a native string and strip quotes (ParaView properties often stringify with quotes)
        mode = str(mode).strip("'\"")

        name = self.state.editing_source
        if not name: return
        
        # Special handling for sField base proxies (switching between Volume/Surface proxies)
        # Exclude derived proxies like Ghosts or Slices which behave like standard proxies
        if name.startswith("ippl_sField") and ".Ghosts" not in name and ".Slice" not in name:
            self._set_sfield_representation(name, mode)
            return
        
        proxy = self.get_display_proxy(name)
        if not proxy: return
        
        rep = simple.GetRepresentation(proxy, simple.GetActiveView())
        if not rep: return
        
        # Auto-switch to Point Data for Volume rendering if currently using Cell Data
        if mode == 'Volume':
            ca = rep.ColorArrayName
            if ca and len(ca) > 0 and ca[0] == 'CELLS':
                print(f"[Auto] Switching to Point Data for Volume rendering on {name}")
                info = proxy.GetDataInformation()
                pd = info.GetPointDataInformation()
                found_name = None
                if pd.GetArrayInformation("density"):
                    found_name = "density"
                else:
                    for i in range(pd.GetNumberOfArrays()):
                        arr = pd.GetArrayInformation(i)
                        if arr.GetName():
                            found_name = arr.GetName()
                            break
                
                if found_name:
                    print(f"[Auto] Selected Point Array: {found_name}")
                    self.update_color_by(found_name)
                else:
                    print("[Warn] No Point Data found for Volume rendering. It may be invisible.")

        try:
            rep.SetRepresentationType(mode)
            
            # For Volume rendering, we often need to rescale the transfer function 
            # and opacity map to the current data range to make it visible.
            if mode == 'Volume' and self.state.auto_rescale_color:
                rep.RescaleTransferFunctionToDataRange(True, True)
                
            simple.Render()
            
            # Refresh available arrays and validate current selection
            info = proxy.GetDataInformation()
            self._update_available_arrays(info)
            self._validate_color_selection(rep)
            
            self._safe_view_update()
        except Exception as e:
            print(f"[WARN] Failed to set representation: {e}")

    def _validate_color_selection(self, rep):
        """Ensure current color selection is valid for the new representation/proxy."""
        s = self.state
        current = s.current_color_array
        
        # Check if current selection is in available arrays
        valid_values = [a['value'] for a in s.color_arrays]
        if current not in valid_values:
            print(f"[UI] Current color {current} invalid for new representation. Resetting.")
            # Try to find a valid default
            if len(valid_values) > 1: # 0 is SOLID, 1 is first array
                # Prefer first array if available
                new_val = valid_values[1]
                self.update_color_by(new_val)
                s.current_color_array = new_val
            else:
                # Fallback to SOLID
                self.update_color_by('SOLID')
                s.current_color_array = 'SOLID'

    def _set_sfield_representation(self, name, mode):
        view = simple.GetActiveView()
        resample = find_source_by_name(f"{name}_Resample")
        merged = find_source_by_name(f"{name}.MergedBlocks")
        
        if not resample or not merged:
            print(f"[WARN] Missing proxies for sField representation switch: resample={resample}, merged={merged}")
            return

        # Determine target proxy based on mode
        # User request: Always use ResampleToImage (resample) for sField visualization
        # because MergedBlocks (unstructured) is causing issues with non-Volume reps.
        target_proxy = resample
        other_proxy = merged
        
        # Hide other
        other_rep = simple.GetRepresentation(other_proxy, view)
        if other_rep:
            other_rep.Visibility = 0
            
        # Show target
        target_proxy.UpdatePipeline()
        target_rep = simple.Show(target_proxy, view)
        target_rep.Visibility = 1
        target_rep.SetRepresentationType(mode)
        
        if mode == 'Point Gaussian':
            # Configure default Gaussian radius based on bounds
            bounds = target_proxy.GetDataInformation().GetBounds()
            dx = bounds[1] - bounds[0]
            dy = bounds[3] - bounds[2]
            dz = bounds[5] - bounds[4]
            import math
            diagonal = math.sqrt(dx*dx + dy*dy + dz*dz)
            if diagonal > 0:
                target_rep.GaussianRadius = diagonal / 100.0
            else:
                target_rep.GaussianRadius = 0.05
        
        # Debug: Check if target has data
        t_info = target_proxy.GetDataInformation()
        print(f"[DEBUG] Switched to {target_proxy.GetLogName()}. Cells: {t_info.GetNumberOfCells()}, Bounds: {t_info.GetBounds()}")
        
        # Transfer color state if needed
        s = self.state
        if s.current_color_array == 'SOLID':
            target_rep.ColorArrayName = [None, '']
            if hasattr(target_rep, 'DiffuseColor') and s.solid_color:
                # Re-apply solid color
                self.set_solid_color(s.solid_color)
        elif s.current_color_array:
            # Re-apply color by
            self.update_color_by(s.current_color_array)
            
        simple.Render()
        
        # Refresh available arrays and validate current selection for the new proxy
        info = target_proxy.GetDataInformation()
        self._update_available_arrays(info)
        
        # Special handling for Volume: it requires Point Data.
        # If we switched to Volume and the current array is Cell Data (which is common for MergedBlocks),
        # we must switch to a Point Data array to avoid invisibility.
        if mode == 'Volume':
            current_val = s.current_color_array
            if current_val and 'CELLS:' in current_val:
                print(f"[Auto] Volume mode requires Point Data. Switching from {current_val}...")
                # Try to find the same array name in POINTS
                array_name = current_val.split(':')[1]
                new_val = f"POINTS:{array_name}"
                
                # Check if this point array actually exists
                valid_values = [a['value'] for a in s.color_arrays]
                if new_val in valid_values:
                    print(f"[Auto] Found matching Point Data: {new_val}")
                    self.update_color_by(new_val)
                    s.current_color_array = new_val
                else:
                    # Fallback to first available point array
                    point_arrays = [v for v in valid_values if 'POINTS:' in v]
                    if point_arrays:
                        print(f"[Auto] Fallback to first Point Data: {point_arrays[0]}")
                        self.update_color_by(point_arrays[0])
                        s.current_color_array = point_arrays[0]
        
        self._validate_color_selection(target_rep)
        self._validate_color_selection(target_rep)
        
        self._safe_view_update()

    def update_color_by(self, value):
        print(f"[UI] Color By changed: {value}")
        name = self.state.editing_source
        if not name:
            return
        proxy = self.get_display_proxy(name)
        if not proxy:
            return
        view = simple.GetActiveView()
        rep = simple.GetRepresentation(proxy, view)
        if not rep:
            return

        # Hide previous scalar bar, if any, before switching
        try:
            prev_ca = rep.ColorArrayName
            if prev_ca and len(prev_ca) > 1 and prev_ca[1]:
                lut_prev = simple.GetColorTransferFunction(prev_ca[1])
                sb_prev = simple.GetScalarBar(lut_prev, view)
                if sb_prev:
                    sb_prev.Visibility = 0
        except Exception:
            pass

        if value == 'SOLID':
            rep.ColorArrayName = [None, '']
            try:
                rep.SetScalarBarVisibility(view, False)
            except Exception:
                pass
            self.state.scalar_bar_visible = False
        else:
            parts = value.split(':')
            if len(parts) < 2:
                return
            assoc = parts[0]
            array_name = parts[1]
            
            # Check if we are switching to a new array or just refreshing
            is_new_array = True
            current_ca = rep.ColorArrayName
            if current_ca and len(current_ca) > 1 and current_ca[1] == array_name and current_ca[0] == assoc:
                 is_new_array = False

            if is_new_array:
                rep.ColorArrayName = [assoc, array_name]
            
            lut = simple.GetColorTransferFunction(array_name)
            info = proxy.GetDataInformation()
            dinfo = info.GetPointDataInformation() if assoc == 'POINTS' else info.GetCellDataInformation()
            ai = dinfo.GetArrayInformation(array_name)
            
            # Check if vector
            is_vector = False
            if ai and ai.GetNumberOfComponents() > 1:
                is_vector = True
            self.state.color_array_is_vector = is_vector
            
            if is_new_array:
                self.state.current_color_component = 'Magnitude' # Reset to default
            else:
                # Sync UI with current LUT state
                if lut.VectorMode == 'Magnitude':
                    self.state.current_color_component = 'Magnitude'
                elif lut.VectorMode == 'Component':
                    comps = {0: 'X', 1: 'Y', 2: 'Z'}
                    self.state.current_color_component = comps.get(lut.VectorComponent, 'Magnitude')

            if ai:
                # Determine component for range
                comp_idx = -1
                if not is_new_array and is_vector and lut.VectorMode == 'Component':
                    comp_idx = lut.VectorComponent
                
                r = ai.GetComponentRange(comp_idx)
                print(f"[DEBUG] Rescaling LUT for {array_name} to {r}")
                
                if self.state.auto_rescale_color:
                    min_v, max_v = r[0], r[1]
                    if self.state.symmetric_rescale:
                        max_abs = max(abs(min_v), abs(max_v))
                        min_v = -max_abs
                        max_v = max_abs
                        
                    if max_v > min_v:
                        lut.RescaleTransferFunction(min_v, max_v)
                    # Update custom range UI to match data
                    self.state.custom_rescale_min = min_v
                    self.state.custom_rescale_max = max_v
                else:
                    # Apply custom range if valid
                    try:
                        vmin = float(self.state.custom_rescale_min)
                        vmax = float(self.state.custom_rescale_max)
                        if vmax > vmin:
                             lut.RescaleTransferFunction(vmin, vmax)
                    except Exception:
                        # Fallback to data range if custom is invalid
                        if r[1] > r[0]:
                            lut.RescaleTransferFunction(r[0], r[1])

            rep.LookupTable = lut
            
            # Ensure VectorMode is reset to Magnitude initially ONLY if new array
            if is_vector and is_new_array:
                lut.VectorMode = 'Magnitude'
            
            sb = simple.GetScalarBar(lut, view)
            sb.Title = array_name
            
            # Update component title
            if is_vector:
                if lut.VectorMode == 'Magnitude':
                    sb.ComponentTitle = 'Magnitude'
                elif lut.VectorMode == 'Component':
                    comps = {0: 'X', 1: 'Y', 2: 'Z'}
                    sb.ComponentTitle = comps.get(lut.VectorComponent, '')
            else:
                sb.ComponentTitle = ''

            desired_vis = bool(getattr(self.state, 'scalar_bar_visible', True))
            sb.Visibility = 1 if desired_vis else 0
            try:
                rep.SetScalarBarVisibility(view, desired_vis)
            except Exception:
                pass
            self.state.scalar_bar_visible = desired_vis

        simple.Render()
        self._safe_view_update()

    def set_color_component(self, mode):
        print(f"[UI] Set Color Component: {mode}")
        name = self.state.editing_source
        if not name: return
        proxy = self.get_display_proxy(name)
        if not proxy: return
        view = simple.GetActiveView()
        rep = simple.GetRepresentation(proxy, view)
        if not rep: return
        
        ca = rep.ColorArrayName
        if not ca or len(ca) < 2 or not ca[1]:
            return
            
        array_name = ca[1]
        lut = simple.GetColorTransferFunction(array_name)
        
        # Map mode to ParaView settings
        # VectorMode: 0=Magnitude, 1=Component, 2=RGB
        component_idx = -1
        if mode == 'Magnitude':
            lut.VectorMode = 'Magnitude'
            component_title = 'Magnitude'
        elif mode == 'X':
            lut.VectorMode = 'Component'
            lut.VectorComponent = 0
            component_idx = 0
            component_title = 'X'
        elif mode == 'Y':
            lut.VectorMode = 'Component'
            lut.VectorComponent = 1
            component_idx = 1
            component_title = 'Y'
        elif mode == 'Z':
            lut.VectorMode = 'Component'
            lut.VectorComponent = 2
            component_idx = 2
            component_title = 'Z'
        else:
            return

        # Rescale to component range
        info = proxy.GetDataInformation()
        dinfo = info.GetPointDataInformation() if ca[0] == 'POINTS' else info.GetCellDataInformation()
        ai = dinfo.GetArrayInformation(array_name)
        if ai:
            r = ai.GetComponentRange(component_idx)
            print(f"[DEBUG] Rescaling LUT for {array_name} component {mode} to {r}")
            if self.state.auto_rescale_color:
                min_v, max_v = r[0], r[1]
                if self.state.symmetric_rescale:
                    max_abs = max(abs(min_v), abs(max_v))
                    min_v = -max_abs
                    max_v = max_abs
                    
                print(f"[DEBUG] Rescaling LUT for {array_name} to {min_v} - {max_v} (Symmetric: {self.state.symmetric_rescale})")
                    
                if max_v > min_v:
                    lut.RescaleTransferFunction(min_v, max_v)
                self.state.custom_rescale_min = min_v
                self.state.custom_rescale_max = max_v
            else:
                 # Apply custom range if valid
                try:
                    vmin = float(self.state.custom_rescale_min)
                    vmax = float(self.state.custom_rescale_max)
                    if vmax > vmin:
                         lut.RescaleTransferFunction(vmin, vmax)
                except Exception:
                    if r[1] > r[0]:
                        lut.RescaleTransferFunction(r[0], r[1])

        sb = simple.GetScalarBar(lut, view)
        if sb:
            sb.ComponentTitle = component_title
            
        simple.Render()
        self._safe_view_update()

    def _safe_view_update(self):
        if hasattr(self.ctrl, 'view_update') and self.ctx.view_update_enabled:
            try:
                self.ctrl.view_update()
            except Exception:
                pass

    def apply_color_map(self, preset_key):
        print(f"[UI] Apply Color Map clicked: {preset_key} (source: {self.state.editing_source})")
        name = self.state.editing_source
        if not name:
            return
        proxy = self.get_display_proxy(name)
        view = simple.GetActiveView()
        if not proxy or not view:
            return
        rep = simple.GetRepresentation(proxy, view)
        if not rep:
            return
        if self.state.current_color_array == 'SOLID':
            return
        ca = rep.ColorArrayName
        if not ca or len(ca) < 2 or not ca[1]:
            return
        array_name = ca[1]
        lut = simple.GetColorTransferFunction(array_name)
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
            lut.ApplyPreset(preset_name, True)
            
            # Re-apply custom range if auto rescale is disabled
            if not self.state.auto_rescale_color:
                 try:
                    vmin = float(self.state.custom_rescale_min)
                    vmax = float(self.state.custom_rescale_max)
                    if vmax > vmin:
                         lut.RescaleTransferFunction(vmin, vmax)
                 except Exception:
                    pass
            elif self.state.symmetric_rescale:
                # Re-apply symmetric rescale if auto is enabled
                try:
                    r = lut.GetRange()
                    min_v, max_v = r[0], r[1]
                    max_abs = max(abs(min_v), abs(max_v))
                    lut.RescaleTransferFunction(-max_abs, max_abs)
                    self.state.custom_rescale_min = -max_abs
                    self.state.custom_rescale_max = max_abs
                except Exception:
                    pass

        except Exception as e:
            print(f"[WARN] Failed to apply preset '{preset_name}': {e}")
            return
        rep.LookupTable = lut
        sb = simple.GetScalarBar(lut, view)
        if sb:
            sb.Visibility = 1 if self.state.scalar_bar_visible else 0
            sb.Title = array_name
            sb.ComponentTitle = 'Magnitude'
        try:
            rep.SetScalarBarVisibility(view, bool(self.state.scalar_bar_visible))
        except Exception:
            pass
        simple.Render()
        self._safe_view_update()
        try:
            d = dict(self.state.color_map_per_source)
            d[name] = preset_key
            self.state.color_map_per_source = d
            self.state.current_color_map = preset_key
        except Exception:
            self.state.color_map_per_source = { name: preset_key }

    def set_scalar_bar_visibility(self, desired_vis):
        print(f"[UI] Scalar Bar visibility toggled: {desired_vis} (source: {self.state.editing_source})")
        name = self.state.editing_source
        proxy = self.get_display_proxy(name) if name else None
        view = simple.GetActiveView()
        if not proxy or not view:
            return
        rep = simple.GetRepresentation(proxy, view)
        if not rep:
            return
        cur_sel = self.state.current_color_array
        if cur_sel == 'SOLID':
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
        self.state.scalar_bar_visible = desired_vis
        simple.Render()
        self._safe_view_update()

    def _safe_view_update(self):
        # Respect throttling flags set in runtime
        try:
            if hasattr(self.ctrl, 'view_update') and getattr(self.ctx, 'view_update_enabled', True):
                self.ctrl.view_update()
        except Exception as e:
            print(f"[WARN] view_update failed: {e}. Disabling further updates and live mode.")
            try:
                self.ctx.view_update_enabled = False
                self.state.live_mode = False
                self.state.status_text = "Live disabled: transport error"
            except Exception:
                pass

    def apply_custom_range(self):
        print(f"[UI] Apply Custom Range clicked")
        s = self.state
        
        try:
            vmin = float(s.custom_rescale_min)
            vmax = float(s.custom_rescale_max)
        except (ValueError, TypeError):
            print("[WARN] Invalid custom range values")
            return
            
        if vmin >= vmax:
            print("[WARN] Min must be less than Max")
            return

        name = s.editing_source
        if not name: return
        proxy = self.get_display_proxy(name)
        if not proxy: return
        view = simple.GetActiveView()
        rep = simple.GetRepresentation(proxy, view)
        if not rep: return
        
        ca = rep.ColorArrayName
        if not ca or len(ca) < 2 or not ca[1]:
            return
            
        array_name = ca[1]
        lut = simple.GetColorTransferFunction(array_name)
        lut.RescaleTransferFunction(vmin, vmax)
        
        simple.Render()
        self._safe_view_update()

    def apply_auto_rescale(self):
        print("[UI] Apply Auto Rescale triggered")
        if not self.state.auto_rescale_color:
            return

        name = self.state.editing_source
        if not name: return
        proxy = self.get_display_proxy(name)
        if not proxy: return
        view = simple.GetActiveView()
        rep = simple.GetRepresentation(proxy, view)
        if not rep: return
        
        ca = rep.ColorArrayName
        if not ca or len(ca) < 2 or not ca[1]:
            return
            
        array_name = ca[1]
        assoc = ca[0]
        lut = simple.GetColorTransferFunction(array_name)
        
        info = proxy.GetDataInformation()
        dinfo = info.GetPointDataInformation() if assoc == 'POINTS' else info.GetCellDataInformation()
        ai = dinfo.GetArrayInformation(array_name)
        
        if ai:
            # Determine component for range
            comp_idx = -1
            if ai.GetNumberOfComponents() > 1:
                if lut.VectorMode == 'Component':
                    comp_idx = lut.VectorComponent
            
            r = ai.GetComponentRange(comp_idx)
            print(f"[DEBUG] Auto-rescaling {array_name} (comp {comp_idx}) to {r}")
            
            min_v, max_v = r[0], r[1]
            if self.state.symmetric_rescale:
                max_abs = max(abs(min_v), abs(max_v))
                min_v = -max_abs
                max_v = max_abs
                
            print(f"[DEBUG] Applying range: {min_v} to {max_v} (Symmetric: {self.state.symmetric_rescale})")
            
            if max_v > min_v:
                lut.RescaleTransferFunction(min_v, max_v)
            
            self.state.custom_rescale_min = min_v
            self.state.custom_rescale_max = max_v
            
            simple.Render()
            self._safe_view_update()

    def save_rescale_settings(self):
        name = self.state.editing_source
        if not name: return
        
        try:
            d = dict(self.state.rescale_settings_per_source)
        except Exception:
            d = {}
            
        d[name] = {
            'auto': self.state.auto_rescale_color,
            'symmetric': self.state.symmetric_rescale,
            'min': self.state.custom_rescale_min,
            'max': self.state.custom_rescale_max
        }
        self.state.rescale_settings_per_source = d

    def update_opacity_points(self, points):
        print(f"[DEBUG-BACKEND] update_opacity_points called with: {points}")
        name = self.state.editing_source
        if not name: 
            print("[DEBUG-BACKEND] No editing source.")
            return
        proxy = self.get_display_proxy(name)
        if not proxy: 
            print(f"[DEBUG-BACKEND] No proxy found for {name}")
            return
        view = simple.GetActiveView()
        rep = simple.GetRepresentation(proxy, view)
        if not rep: 
            print("[DEBUG-BACKEND] No representation found.")
            return
        
        ca = rep.ColorArrayName
        if not ca or len(ca) < 2 or not ca[1]:
            print(f"[DEBUG-BACKEND] Invalid ColorArrayName: {ca}")
            return
            
        array_name = ca[1]
        print(f"[DEBUG-BACKEND] Updating opacity for array: {array_name}")
        lut = simple.GetColorTransferFunction(array_name)
        sof = simple.GetOpacityTransferFunction(array_name)
        
        # Get current range
        # Prefer state values if available as they are the source of truth for the UI
        # and avoid issues with stale proxy properties
        try:
            min_val = float(self.state.custom_rescale_min)
            max_val = float(self.state.custom_rescale_max)
            # Validate range
            if min_val >= max_val:
                raise ValueError("Invalid state range")
        except Exception:
            # Fallback to inferring from LUT
            rgb_points = lut.RGBPoints
            if rgb_points and len(rgb_points) >= 4:
                min_val = rgb_points[0]
                max_val = rgb_points[-4]
            else:
                min_val = 0.0
                max_val = 1.0
            
        width = max_val - min_val
        
        if width == 0: width = 1.0
        
        print(f"[DEBUG-BACKEND] Calculated range: {min_val} to {max_val}, width: {width}")
        
        # Build new points list [x, y, mid, sharp, ...]
        new_points = []
        for p in points:
            try:
                # x is 0-100, normalize to 0-1
                norm_x = float(p.get('x', 0.0)) / 100.0
                opacity = float(p.get('y', 0.0))
                val = min_val + norm_x * width
                # Append x, y, midpoint, sharpness
                new_points.extend([val, opacity, 0.5, 0.0])
            except Exception as e:
                print(f"[WARN] Failed to process opacity point {p}: {e}")

        print(f"[DEBUG-BACKEND] Flattened points for ParaView: {new_points}")
        
        sof.Points = new_points
        print(f"[DEBUG-BACKEND] Assigned to sof.Points")
        
        # # Ensure the representation is using this SOF (sometimes it gets detached)
        # Also ensure OpacityTransferFunction is set to 'Piecewise Function'

        if hasattr(rep, 'OpacityTransferFunction'):

            print("rep has an OpacityTransferFunction")
            rep.OpacityTransferFunction = 'Piecewise Function'
        else:
            print("rep has no OpacityTransferFunction")

        if hasattr(rep, 'ScalarOpacityFunction'):
            print("rep has an attribute ScalarOpacityFunction")
            rep.ScalarOpacityFunction = sof
        else:
            print("rep has no attribute scalar opacity function")
        
        
        simple.Render()
        self._safe_view_update()
