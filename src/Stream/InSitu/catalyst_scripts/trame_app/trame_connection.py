# Connection and extraction logic moved from trame_vis_app.py
import time
from typing import Any
from paraview import simple, live

try:
    from . import trame_render_config as render_config  # sibling module in trame_app
    from . import trame_steering_config as steering_config
except Exception:
    import trame_app.trame_render_config as render_config
    import trame_app.trame_steering_config as steering_config
from catalystSubroutines import find_source_by_name, get_available_extract_names

# Public API: install(ctx) optional, and functions accepting ctx

def install(ctx: Any):
    # No-op for now; placeholder if we need to register hooks
    return ctx


def connect_to_catalyst(ctx: Any):
    print("[UI] Connect clicked")
    state = ctx.state
    cat_host = state.catalyst_host
    cat_port = int(state.catalyst_port)
    try:
        # Preload definitions to avoid errors during handshake (best effort)
        steering_config.preload_steerable_proxies()

        print(f"Connecting to {cat_host}:{cat_port}...")
        ctx.catalyst_link = live.ConnectToCatalyst(ds_host=cat_host, ds_port=cat_port)
        if ctx.catalyst_link:
            state.connected = True
            state.status_text = f"Connected"
            try:
                ipm = ctx.catalyst_link.GetInsituProxyManager()
                if ipm:
                    print("[DEBUG] connect_to_catalyst: Loading proxies into insitu PM")
                    steering_config.load_steerable_proxies(proxy_manager=ipm)
                    # Extract steering channels so proxies exist before Apply
                    try:
                        names = get_available_extract_names(ctx.catalyst_link)
                        steering_names = [n for n in names if n.startswith("Steering") or n.startswith("SteeringParameters")]
                        if steering_names:
                            print(f"[DEBUG] Extracting steering channels: {steering_names}")
                            for sname in steering_names:
                                try:
                                    live.ExtractCatalystData(ctx.catalyst_link, sname)
                                except Exception as e_ex:
                                    print(f"[WARN] Extract steering '{sname}' failed: {e_ex}")
                            # Process notifications to materialize proxies
                            for _ in range(20):
                                if not live.ProcessServerNotifications():
                                    break
                        else:
                            print("[DEBUG] No steering channels listed to extract")
                    except Exception as e_list:
                        print(f"[WARN] Failed to list/extract steering channels: {e_list}")
                    # Refresh parsed steering definitions for UI
                    try:
                        ctx.state.steerable_proxies = steering_config.parse_steerable_parameters()
                        if not ctx.state.selected_steering_proxy and ctx.state.steerable_proxies:
                            ctx.state.selected_steering_proxy = ctx.state.steerable_proxies[0]['name']
                    except Exception as e2:
                        print(f"[WARN] Failed to parse steerable parameters: {e2}")
                    state.insitu_proxies_loaded = True
            except Exception as e:
                print(f"[DEBUG] connect_to_catalyst: Failed to load insitu proxies: {e}")
            # Start polling via runtime module (already wired in app init)
            ctx.ctrl.resume_polling()
            # Auto-scan list on connect, but don't auto-select
            scan_sources_only(ctx)
        else:
            state.status_text = "Connection returned None"
    except Exception as e:
        print(f"[Error] Connect: {e}")
        state.status_text = f"Error: {e}"


def toggle_connection(ctx: Any):
    print("[UI] Toggle Connection clicked")
    state = ctx.state
    if state.connected:
        print("Disconnecting from Catalyst...")
        if state.has_data:
            reset_visualization(ctx)
        ctx.catalyst_link = None
        state.connected = False
        state.status_text = "Disconnected"
        state.available_sources = []
        state.selected_source = None
    else:
        connect_to_catalyst(ctx)


def scan_sources_only(ctx: Any):
    if not ctx.catalyst_link: return
    print("Scanning for available channels...")
    msgs_processed = 0
    while live.ProcessServerNotifications():
        msgs_processed += 1
        if msgs_processed > 50: break
    names = get_available_extract_names(ctx.catalyst_link)
    names = [n for n in names if not (n.startswith("Steering"))]
    if names:
        print(f"Scan complete. Found: {names}")
        ctx.state.available_sources = names
    else:
        print("Scan returned empty.")


def search_and_select_best(ctx: Any):
    print(f"[UI] Search invoked with input: {ctx.state.selected_source}")
    current_input = ctx.state.selected_source
    available = ctx.state.available_sources
    if not available:
        scan_sources_only(ctx)
        available = ctx.state.available_sources
    if not available or not current_input:
        print("Search skipped: No input provided or no sources found.")
        return
    if current_input in available:
        print(f"Exact match found: {current_input}. Initializing...")
        extract_data(ctx)
        return
    best_match = None
    for name in available:
        if current_input.lower() in name.lower():
            best_match = name
            break
    if best_match:
        print(f"Auto-switching selection to: {best_match}")
        ctx.state.selected_source = best_match
    else:
        print(f"No match found for '{current_input}'")


def extract_data(ctx: Any):
    print(f"[UI] Init clicked for selection: {ctx.state.selected_source}")
    state = ctx.state
    if not ctx.catalyst_link: return
    channel_name = state.selected_source
    if not channel_name: return
    if channel_name in ctx.active_proxies:
        print(f"Source {channel_name} is already active.")
        return
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
            live.ExtractCatalystData(ctx.catalyst_link, p_name)
    except Exception as e:
        print(f"Extract call failed: {e}")
        return
    primary_proxy_name = channel_name
    if channel_name.startswith("ippl_sField"):
        primary_proxy_name = f"{channel_name}.MergedBlocks"
    elif channel_name.startswith("ippl_particles"):
        primary_proxy_name = f"{channel_name}.bunch"
    found_data = False
    new_proxy = None
    for i in range(50):
        live.ProcessServerNotifications()
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
        ctx.active_proxies[channel_name] = new_proxy
        current_items = list(state.pipeline_items)
        current_items.append({"id": channel_name, "name": channel_name, "visible": True})
        state.pipeline_items = current_items
        view = simple.GetActiveView()
        if channel_name.startswith("ippl_particles"):
            print(f"Applying Custom Particle Render for {channel_name}...")
            render_config.setup_particle_view(new_proxy, view, channel_name)
        elif channel_name.startswith("ippl_sField"):
            print(f"Applying Scalar Field Render for {channel_name}...")
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
        render_config.reset_camera(simple.GetActiveView(), channel_name, new_proxy)
        simple.Render()
        if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
            try:
                ctx.ctrl.view_update()
            except Exception as e:
                print(f"[WARN] view_update failed: {e}. Disabling further updates and live mode.")
                ctx.view_update_enabled = False
                state.live_mode = False
                state.status_text = "Live disabled: transport error"
        state.has_data = True
        state.selected_source = None
        print(f"Extraction successful: {channel_name}")


def reset_visualization(ctx: Any):
    print("[UI] Reset Visualization clicked")
    state = ctx.state
    state.live_mode = False
    state.has_data = False
    view = simple.GetActiveView()
    if view:
        for rep in view.Representations:
            try:
                rep.SetScalarBarVisibility(view, False)
            except:
                pass
    for name in list(ctx.active_proxies.keys()):
        # inline remove_proxy behavior: ensure deletion of various child proxies
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
    ctx.active_proxies = {}
    state.pipeline_items = []
    simple.Render()
    if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
        try:
            ctx.ctrl.view_update()
        except Exception as e:
            print(f"[WARN] view_update failed: {e}. Disabling further updates and live mode.")
            ctx.view_update_enabled = False
            state.live_mode = False
            state.status_text = "Live disabled: transport error"


def reload_steering_proxies(ctx: Any):
    """Extract/reload steering proxies and report availability vs XML."""
    print("[UI] Reload Steering Proxies requested")
    link = ctx.catalyst_link
    if not link:
        print("[WARN] No Catalyst connection; cannot reload steering proxies.")
        return

    # Parse expected proxies from XML
    try:
        expected_defs = steering_config.parse_steerable_parameters()
        expected_names = [p['name'] for p in expected_defs]
        print(f"[DEBUG] XML-defined steerable proxies: {expected_names}")
    except Exception as e:
        print(f"[ERROR] Failed to parse steerable parameters: {e}")
        expected_defs = []
        expected_names = []

    # List available extracts
    try:
        names = get_available_extract_names(link)
    except Exception:
        names = []
    print(f"DEBUG: All found sources: {names}")
    steering_names = [n for n in names if n.startswith("Steering") or n.startswith("SteeringParameters")]
    print(f"[DEBUG] Steering channels discovered: {steering_names}")

    # Request extraction for each steering channel
    for sname in steering_names:
        try:
            live.ExtractCatalystData(link, sname)
        except Exception as e_ex:
            print(f"[WARN] Extract steering '{sname}' failed: {e_ex}")

    # Process notifications to ensure proxies materialize client-side
    try:
        for _ in range(50):
            if not live.ProcessServerNotifications():
                break
    except Exception:
        pass

    # Report which expected proxies are available
    found = []
    missing = []
    try:
        pm = link.GetInsituProxyManager()
    except Exception:
        pm = None
    groups = ["sources", "filters", "insitu", "misc"]
    for pname in expected_names:
        candidates = [pname, pname.replace(":", "_"), pname.lower(), pname.replace(":", "_").lower()]
        if pname.startswith("SteerableParameters_"):
            base = pname.replace("SteerableParameters_", "")
            candidates.extend([
                f"Steering_{base}",
                f"SteeringParameters_{base}",
                f"Steering_array:{base}",
                f"Steering_array_{base}",
            ])
        found_flag = False
        if pm:
            for g in groups:
                try:
                    for cand in candidates:
                        p = pm.GetProxy(g, cand)
                        if p:
                            found_flag = True
                            break
                    if found_flag:
                        break
                except Exception:
                    pass
        if not found_flag:
            for cand in candidates:
                try:
                    p = simple.FindSource(cand)
                    if p:
                        found_flag = True
                        break
                except Exception:
                    pass
        (found if found_flag else missing).append(pname)

    print(f"[INFO] Steering proxies available (from XML): {found}")
    if missing:
        print(f"[WARN] Steering proxies missing/not extracted: {missing}")
    else:
        print("[INFO] All XML-defined steering proxies are available.")

    # Refresh state copies for UI consumers
    try:
        ctx.state.steerable_proxies = expected_defs
        if not ctx.state.selected_steering_proxy and expected_defs:
            ctx.state.selected_steering_proxy = expected_defs[0]['name']
    except Exception:
        pass
