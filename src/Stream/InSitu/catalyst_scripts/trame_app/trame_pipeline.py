# Pipeline interactions moved from trame_vis_app.py
from typing import Any
from paraview import simple

from catalystSubroutines import find_source_by_name
import trame_render_config as render_config


def get_display_proxy(ctx: Any, name: str):
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


def remove_proxy(ctx: Any, name: str):
    print(f"[UI] Remove Source clicked: {name}")
    if name not in ctx.active_proxies: return
    print(f"Removing proxy: {name}")
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
        p_box = find_source_by_name(f"{sel}.box")
        if p_box: simple.Delete(p_box)
        p_bunch = find_source_by_name(f"{sel}.bunch")
        if p_bunch: simple.Delete(p_bunch)
        p_container = find_source_by_name(sel)
        if p_container: simple.Delete(p_container)
    else:
        p = find_source_by_name(sel)
        if p: simple.Delete(p)
    del ctx.active_proxies[name]
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


def toggle_visibility(ctx: Any, name: str):
    print(f"[UI] Toggle Visibility clicked: {name}")
    if name not in ctx.active_proxies: return
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
    display_proxy = None
    if name.startswith("ippl_sField"):
        display_proxy = find_source_by_name(f"{name}_Resample")
        if not display_proxy: display_proxy = find_source_by_name(f"{name}.MergedBlocks")
    elif name.startswith("ippl_vField"):
        display_proxy = find_source_by_name(f"{name}_Glyph")
        if not display_proxy: display_proxy = find_source_by_name(f"{name}.Glyph")
        if not display_proxy: display_proxy = find_source_by_name(name)
    elif name.startswith("ippl_particles"):
        p_bunch = find_source_by_name(f"{name}.bunch")
        if p_bunch:
            rep = simple.GetRepresentation(p_bunch, simple.GetActiveView())
            if rep: rep.Visibility = 1 if is_visible else 0
        p_box = find_source_by_name(f"{name}.box")
        if p_box:
            rep = simple.GetRepresentation(p_box, simple.GetActiveView())
            if rep: rep.Visibility = 1 if is_visible else 0
        simple.Render()
        if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
            try:
                ctx.ctrl.view_update()
            except Exception as e:
                print(f"[WARN] view_update failed: {e}. Disabling further updates and live mode.")
                ctx.view_update_enabled = False
                ctx.state.live_mode = False
                ctx.state.status_text = "Live disabled: transport error"
        return
    if display_proxy:
        rep = simple.GetRepresentation(display_proxy, simple.GetActiveView())
        if rep:
            rep.Visibility = 1 if is_visible else 0
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
