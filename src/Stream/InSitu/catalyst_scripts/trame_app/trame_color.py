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
            if name.startswith("ippl_sField"):
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
        arrays = []
        pd = info.GetPointDataInformation()
        for i in range(pd.GetNumberOfArrays()):
            ai = pd.GetArrayInformation(i)
            arrays.append({'title': f"{ai.GetName()} (Points)", 'value': f"POINTS:{ai.GetName()}"})
        cd = info.GetCellDataInformation()
        for i in range(cd.GetNumberOfArrays()):
            ai = cd.GetArrayInformation(i)
            arrays.append({'title': f"{ai.GetName()} (Cells)", 'value': f"CELLS:{ai.GetName()}"})
        arrays.insert(0, {'title': 'Solid Color', 'value': 'SOLID'})
        s.color_arrays = arrays

        ca = rep.ColorArrayName
        if ca and len(ca) > 1 and ca[0] and ca[1]:
            s.current_color_array = f"{ca[0]}:{ca[1]}"
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
            rep.ColorArrayName = [assoc, array_name]
            lut = simple.GetColorTransferFunction(array_name)
            info = proxy.GetDataInformation()
            dinfo = info.GetPointDataInformation() if assoc == 'POINTS' else info.GetCellDataInformation()
            ai = dinfo.GetArrayInformation(array_name)
            if ai:
                r = ai.GetComponentRange(-1)
                if r[1] > r[0]:
                    lut.RescaleTransferFunction(r[0], r[1])
            rep.LookupTable = lut
            sb = simple.GetScalarBar(lut, view)
            sb.Title = array_name
            sb.ComponentTitle = 'Magnitude'
            desired_vis = bool(getattr(self.state, 'scalar_bar_visible', True))
            sb.Visibility = 1 if desired_vis else 0
            try:
                rep.SetScalarBarVisibility(view, desired_vis)
            except Exception:
                pass
            self.state.scalar_bar_visible = desired_vis

        simple.Render()
        self._safe_view_update()

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
        self.state.scalar_bar_visible = bool(desired_vis)
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
