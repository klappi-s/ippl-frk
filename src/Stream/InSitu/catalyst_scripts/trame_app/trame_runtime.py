# Runtime (polling/backpressure/debounce) extracted into a module
# Minimal integration: provide resume_polling(ctx) to start the poll loop

import asyncio
import time
from typing import Any
from paraview import simple, live

from catalystSubroutines import find_source_by_name
# Prefer local trame_app render_config; fallback to script-mode import if needed
try:
    from . import trame_render_config as render_config
except Exception:
    try:
        from trame_app import trame_render_config as render_config
    except Exception:
        import trame_render_config as render_config

async def poll_catalyst(ctx: Any):
    state = ctx.state
    ctrl = ctx.ctrl
    while True:
        if ctx.catalyst_link:
            try:
                live.ProcessServerNotifications()
                if state.live_mode and state.has_data:
                    # Camera signature for debounce
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

                    if camera_sig is not None and camera_sig != ctx.camera_last_sig:
                        ctx.camera_last_sig = camera_sig
                        ctx.camera_last_change_ts = now
                        # Optional: verbose camera logging; leave to app layer if needed
                        try:
                            pos, foc, up, ang, pscale = camera_sig
                            print(f"[UI] Camera changed: pos={pos}, focal={foc}, up={up}, angle={ang}, parallelScale={pscale}")
                        except Exception:
                            print("[UI] Camera changed")

                    # Update pipelines
                    for sel, proxy in ctx.active_proxies.items():
                        if sel.startswith("ippl_sField"):
                            if proxy:
                                proxy.UpdatePipeline()
                            merged_proxy = find_source_by_name(f"{sel}.MergedBlocks")
                            if merged_proxy:
                                merged_proxy.UpdatePipeline()
                            resample_proxy = find_source_by_name(f"{sel}_Resample")
                            if resample_proxy:
                                now_gate = time.time()
                                if (now_gate - getattr(state, 'last_resample_update_ts', 0.0)) > 0.5:
                                    resample_proxy.UpdatePipeline()
                                    render_config.update_scalar_field_view(resample_proxy, simple.GetActiveView())
                                    state.last_resample_update_ts = now_gate
                        elif sel.startswith("ippl_vField"):
                            if proxy:
                                proxy.UpdatePipeline()
                            glyph_proxy = find_source_by_name(f"{sel}.Glyph")
                            if glyph_proxy:
                                glyph_proxy.UpdatePipeline()
                                render_config.update_vector_field_view(glyph_proxy, simple.GetActiveView())
                            else:
                                glyph_proxy = find_source_by_name(f"{sel}_Glyph")
                                if glyph_proxy:
                                    glyph_proxy.UpdatePipeline()
                                    render_config.update_vector_field_view(glyph_proxy, simple.GetActiveView())
                        else:
                            if proxy:
                                proxy.UpdatePipeline()
                                if sel.startswith("ippl_particles"):
                                    box_proxy = find_source_by_name(f"{sel}.box")
                                    if box_proxy:
                                        box_proxy.UpdatePipeline()
                                    render_config.update_particle_view(proxy, simple.GetActiveView())

                    simple.Render()
                    # Throttle view updates with backpressure and camera debounce
                    now = time.time()
                    has_scalar_field = any(k.startswith("ippl_sField") for k in ctx.active_proxies.keys())
                    interval = 2.0 if has_scalar_field else 0.5
                    camera_stable_for = (now - ctx.camera_last_change_ts) if ctx.camera_last_change_ts else 999
                    camera_stable = (camera_stable_for > 1.0)
                    if now < ctx.view_update_pending_until:
                        pass
                    elif has_scalar_field and not camera_stable:
                        pass
                    elif hasattr(ctrl, 'view_update') and ctx.view_update_enabled and state.live_mode and (now - ctx.last_view_update_ts) > interval:
                        try:
                            ctrl.view_update()
                            ctx.last_view_update_ts = now
                            ctx.view_update_pending_until = now + (1.2 if has_scalar_field else 0.3)
                        except Exception as e:
                            print(f"[WARN] view_update failed during polling: {e}. Disabling further updates and live mode.")
                            ctx.view_update_enabled = False
                            state.live_mode = False
                            state.status_text = "Live disabled: transport error"
            except Exception:
                pass
        # NOTE: caller can choose overall poll cadence; default to 0.5s like current app
        await asyncio.sleep(0.5)


def resume_polling(ctx: Any):
    """Assign a resume_polling to ctrl that starts the runtime loop with the shared ctx."""
    ctx.ctrl.resume_polling = lambda: asyncio.create_task(poll_catalyst(ctx))
