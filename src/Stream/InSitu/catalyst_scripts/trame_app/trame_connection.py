# Connection and extraction logic moved from trame_vis_app.py
import time
from typing import Any
from paraview import simple, live

try:
    from . import trame_render_config as render_config  # sibling module in trame_app
    from . import trame_steering_config as steering_config
    from . import trame_logging as log
except Exception:
    import trame_app.trame_render_config as render_config
    import trame_app.trame_steering_config as steering_config
    import trame_app.trame_logging as log
from catalystSubroutines import find_source_by_name, get_available_extract_names

# Public API: install(ctx) optional, and functions accepting ctx

def install(ctx: Any):
    # No-op for now; placeholder if we need to register hooks
    return ctx


def connect_to_catalyst(ctx: Any):
    log.ui("Connect clicked")
    state = ctx.state
    cat_host = state.catalyst_host
    cat_port = int(state.catalyst_port)
    try:
        # Preload definitions to avoid errors during handshake (best effort)
        steering_config.preload_steerable_proxies()

        log.info("Connecting to {}:{}", cat_host, cat_port)
        ctx.catalyst_link = live.ConnectToCatalyst(ds_host=cat_host, ds_port=cat_port)
        if ctx.catalyst_link:
            state.connected = True
            state.status_text = f"Connected"
            try:
                ipm = ctx.catalyst_link.GetInsituProxyManager()
                if ipm:
                    log.debug("connect_to_catalyst: Loading proxies into insitu PM")
                    steering_config.load_steerable_proxies(proxy_manager=ipm)
                    # Extract steering channels so proxies exist before Apply
                    try:
                        names = get_available_extract_names(ctx.catalyst_link)
                        steering_names = [n for n in names if n.startswith("Steering") or n.startswith("SteeringParameters")]
                        if steering_names:
                            log.debug("Extracting steering channels: {}", steering_names)
                            for sname in steering_names:
                                try:
                                    live.ExtractCatalystData(ctx.catalyst_link, sname)
                                except Exception as e_ex:
                                    log.warn("Extract steering '{}' failed: {}", sname, e_ex)
                            # Process notifications to materialize proxies
                            for _ in range(20):
                                if not live.ProcessServerNotifications():
                                    break
                        else:
                            log.debug("No steering channels listed to extract")
                    except Exception as e_list:
                        log.warn("Failed to list/extract steering channels: {}", e_list)
                    # Refresh parsed steering definitions for UI
                    try:
                        ctx.state.steerable_proxies = steering_config.parse_steerable_parameters()
                        if not ctx.state.selected_steering_proxy and ctx.state.steerable_proxies:
                            ctx.state.selected_steering_proxy = ctx.state.steerable_proxies[0]['name']
                    except Exception as e2:
                        log.warn("Failed to parse steerable parameters: {}", e2)
                    state.insitu_proxies_loaded = True
            except Exception as e:
                log.debug("connect_to_catalyst: Failed to load insitu proxies: {}", e)
            # Start polling via runtime module (already wired in app init)
            ctx.ctrl.resume_polling()
            # Auto-scan list on connect, but don't auto-select
            scan_sources_only(ctx)
        else:
            state.status_text = "Connection returned None"
    except Exception as e:
        log.error("Connect: {}", e)
        state.status_text = f"Error: {e}"


def toggle_connection(ctx: Any):
    log.ui("Toggle Connection clicked")
    state = ctx.state
    if state.connected:
        log.info("Disconnecting from Catalyst...")
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
    log.info("Scanning for available channels...")
    msgs_processed = 0
    while live.ProcessServerNotifications():
        msgs_processed += 1
        if msgs_processed > 50: break
    names = get_available_extract_names(ctx.catalyst_link)
    names = [n for n in names if not (n.startswith("Steering"))]
    if names:
        log.info("Scan complete. Found: {}", names)
        ctx.state.available_sources = names
    else:
        log.info("Scan returned empty.")


def search_and_select_best(ctx: Any):
    log.ui("Search invoked with input: {}", ctx.state.selected_source)
    current_input = ctx.state.selected_source
    available = ctx.state.available_sources
    if not available:
        scan_sources_only(ctx)
        available = ctx.state.available_sources
    if not available or not current_input:
        log.info("Search skipped: No input provided or no sources found.")
        return
    if current_input in available:
        log.info("Exact match found: {}. Initializing...", current_input)
        extract_data(ctx)
        return
    best_match = None
    for name in available:
        if current_input.lower() in name.lower():
            best_match = name
            break
    if best_match:
        log.info("Auto-switching selection to: {}", best_match)
        ctx.state.selected_source = best_match
    else:
        log.info("No match found for '{}'", current_input)


def extract_data(ctx: Any):
    log.ui("Init clicked for selection: {}", ctx.state.selected_source)
    state = ctx.state
    if not ctx.catalyst_link: return
    channel_name = state.selected_source
    if not channel_name: return
    if channel_name in ctx.active_proxies:
        # Special check for particles: if one component is missing, allow re-extraction
        if channel_name.startswith("ippl_particles"):
            bunch_key = f"{channel_name}.bunch"
            box_key = f"{channel_name}.box"
            if bunch_key in ctx.active_proxies and box_key in ctx.active_proxies:
                 log.info("Source {} is already fully active.", channel_name)
                 return
            log.info("Partial particle source detected. Re-extracting missing components for {}...", channel_name)
        else:
            log.info("Source {} is already active.", channel_name)
            return
    proxies_to_extract = [channel_name]
    if channel_name.startswith("ippl_sField"):
        proxies_to_extract.append(f"{channel_name}.MergedBlocks")
    elif channel_name.startswith("ippl_vField"):
        proxies_to_extract.append(f"{channel_name}.Glyph")
        proxies_to_extract.append(f"{channel_name}.MergedBlocks")
    elif channel_name.startswith("ippl_particles"):
        proxies_to_extract.append(f"{channel_name}.bunch")
        proxies_to_extract.append(f"{channel_name}.box")
    log.info("Requesting {}...", proxies_to_extract)
    try:
        for p_name in proxies_to_extract:
            live.ExtractCatalystData(ctx.catalyst_link, p_name)
    except Exception as e:
        log.warn("Extract call failed: {}", e)
        return
    primary_proxy_name = channel_name
    if channel_name.startswith("ippl_sField"):
        primary_proxy_name = f"{channel_name}.MergedBlocks"
    elif channel_name.startswith("ippl_particles"):
        primary_proxy_name = f"{channel_name}.bunch"
    found_data = False
    new_proxy = None
    last_error = None
    for i in range(50):
        live.ProcessServerNotifications()
        p_proxy = find_source_by_name(primary_proxy_name)
        if p_proxy:
            try:
                p_proxy.UpdatePipeline()
                info = p_proxy.GetDataInformation()
                num_pts = info.GetNumberOfPoints()
                num_cls = info.GetNumberOfCells()
                if num_pts > 0 or num_cls > 0:
                    found_data = True
                    new_proxy = p_proxy
                    log.debug("Found data for {}: {} points, {} cells", primary_proxy_name, num_pts, num_cls)
                    break
                else:
                    last_error = f"Proxy found but empty (0 points, 0 cells)"
            except Exception as e:
                last_error = str(e)
        else:
            last_error = f"Proxy '{primary_proxy_name}' not found"
        time.sleep(0.1)
    
    if not found_data or not new_proxy:
        log.error("Failed to extract {}: {}", channel_name, last_error or "Unknown error")
        state.status_text = f"Failed to extract {channel_name}"
        return
        
    if found_data and new_proxy:
        ctx.active_proxies[channel_name] = new_proxy
        current_items = list(state.pipeline_items)
        
        if channel_name.startswith("ippl_particles"):
            # Split particle source into Bunch and Box
            bunch_id = f"{channel_name}.bunch"
            if not any(item['id'] == bunch_id for item in current_items):
                current_items.append({"id": bunch_id, "name": f"{channel_name} (Bunch)", "visible": True})
            ctx.active_proxies[bunch_id] = new_proxy # new_proxy is the bunch
            
            box_proxy = find_source_by_name(f"{channel_name}.box")
            if box_proxy:
                box_id = f"{channel_name}.box"
                if not any(item['id'] == box_id for item in current_items):
                    current_items.append({"id": box_id, "name": f"{channel_name} (Box)", "visible": True})
                ctx.active_proxies[box_id] = box_proxy
        else:
            if not any(item['id'] == channel_name for item in current_items):
                current_items.append({"id": channel_name, "name": channel_name, "visible": True})
            
        state.pipeline_items = current_items
        view = simple.GetActiveView()
        if channel_name.startswith("ippl_particles"):
            log.info("Applying Custom Particle Render for {}...", channel_name)
            render_config.setup_particle_view(new_proxy, view, channel_name)
        elif channel_name.startswith("ippl_sField"):
            log.info("Applying Scalar Field Render for {}...", channel_name)
            merged_proxy = find_source_by_name(f"{channel_name}.MergedBlocks")
            if merged_proxy:
                render_config.setup_scalar_field_view(merged_proxy, view, channel_name)
        elif channel_name.startswith("ippl_vField"):
            log.info("Applying Vector Field Render for {}...", channel_name)
            # Always create local glyph from MergedBlocks - extracted glyphs can become
            # corrupted after other field initializations and may only contain rank-1 data
            merged_proxy = find_source_by_name(f"{channel_name}.MergedBlocks")
            if merged_proxy:
                render_config.setup_vector_field_view(merged_proxy, view, channel_name)
            else:
                log.warn("No MergedBlocks found for {}. Using base proxy.", channel_name)
                render_config.setup_vector_field_view(new_proxy, view, channel_name)
        else:
            log.info("Applying Default Render for {}...", channel_name)
            render_config.setup_default_view(new_proxy, view)
        render_config.reset_camera(simple.GetActiveView(), channel_name, new_proxy)

        # Defensive: ensure the newly initialized representation stays visible.
        # We've seen cases where init order (sField then vField) leaves the vField
        # rep logically present but not drawn; re-asserting visibility here helps.
        try:
            target = None
            if channel_name.startswith("ippl_particles"):
                target = find_source_by_name(f"{channel_name}.bunch")
            elif channel_name.startswith("ippl_sField"):
                target = find_source_by_name(f"{channel_name}_Resample") or find_source_by_name(f"{channel_name}.MergedBlocks")
            elif channel_name.startswith("ippl_vField"):
                target = find_source_by_name(f"{channel_name}_Glyph") or find_source_by_name(f"{channel_name}.Glyph")
            else:
                target = new_proxy
            if target:
                rep = simple.GetRepresentation(target, simple.GetActiveView())
                if rep:
                    rep.Visibility = 1
        except Exception as e:
            log.debug("Post-reset camera visibility reassert failed: {}", e)

        simple.Render()
        if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
            try:
                ctx.ctrl.view_update()
            except Exception as e:
                log.warn("view_update failed: {}. Disabling further updates and live mode.", e)
                ctx.view_update_enabled = False
                state.live_mode = False
                state.status_text = "Live disabled: transport error"
        state.has_data = True
        state.selected_source = None
        log.info("Extraction successful: {}", channel_name)


def reset_visualization(ctx: Any):
    log.ui("Reset Visualization clicked")
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
    
    proxies_to_delete = set()

    # 1. Add everything from active_proxies values (Direct reference)
    # This ensures we catch proxies like Slices/Ghosts that might have default names
    for p in ctx.active_proxies.values():
        if p:
            proxies_to_delete.add(p)

    # 2. Look for related proxies based on naming conventions of active keys
    # This catches implicit proxies (MergedBlocks, Resample) that might not be in active_proxies
    for name in list(ctx.active_proxies.keys()):
        sel = name
        if sel.startswith("ippl_sField"):
            p1 = find_source_by_name(f"{sel}_Resample")
            p3 = find_source_by_name(f"{sel}.MergedBlocks")
            if p1: proxies_to_delete.add(p1)
            if p3: proxies_to_delete.add(p3)
        elif sel.startswith("ippl_vField"):
            p1 = find_source_by_name(f"{sel}_Glyph")
            p2 = find_source_by_name(f"{sel}.Glyph")
            if p1: proxies_to_delete.add(p1)
            if p2: proxies_to_delete.add(p2)
        elif sel.startswith("ippl_particles"):
            p_box = find_source_by_name(f"{sel}.box")
            p_bunch = find_source_by_name(f"{sel}.bunch")
            if p_box: proxies_to_delete.add(p_box)
            if p_bunch: proxies_to_delete.add(p_bunch)

    # 3. Scan all registered sources for our naming patterns (Catch-all)
    # This catches anything else that matches our patterns
    sources = simple.GetSources()
    for (group, name), proxy in sources.items():
        if (name.startswith("ippl_") or 
            ".Slice" in name or 
            ".Ghosts" in name or 
            ".MergedBlocks" in name or 
            "_Resample" in name or 
            ".Glyph" in name or 
            ".bunch" in name or 
            ".box" in name):
            proxies_to_delete.add(proxy)
            
    # 4. Delete them
    for p in proxies_to_delete:
        try:
            simple.Delete(p)
        except Exception as e:
            log.warn("Failed to delete proxy during reset: {}", e)
            
    ctx.active_proxies = {}
    state.pipeline_items = []
    
    # Reset per-source settings
    state.scalar_bar_per_source = {}
    state.color_map_per_source = {}
    state.rescale_settings_per_source = {}
    
    simple.Render()
    if hasattr(ctx.ctrl, 'view_update') and ctx.view_update_enabled:
        try:
            ctx.ctrl.view_update()
        except Exception as e:
            log.warn("view_update failed: {}. Disabling further updates and live mode.", e)
            ctx.view_update_enabled = False
            state.live_mode = False
            state.status_text = "Live disabled: transport error"


def reload_steering_proxies(ctx: Any):
    """Extract/reload steering proxies and report availability vs XML."""
    log.ui("Reload Steering Proxies requested")
    link = ctx.catalyst_link
    if not link:
        log.warn("No Catalyst connection; cannot reload steering proxies.")
        return

    # Parse expected proxies from XML
    try:
        expected_defs = steering_config.parse_steerable_parameters()
        expected_names = [p['name'] for p in expected_defs]
        log.debug("XML-defined steerable proxies: {}", expected_names)
    except Exception as e:
        log.error("Failed to parse steerable parameters: {}", e)
        expected_defs = []
        expected_names = []

    # List available extracts
    try:
        names = get_available_extract_names(link)
    except Exception:
        names = []
    log.debug("All found sources: {}", names)
    steering_names = [n for n in names if n.startswith("Steering") or n.startswith("SteeringParameters")]
    log.debug("Steering channels discovered: {}", steering_names)

    # Request extraction for each steering channel
    for sname in steering_names:
        try:
            live.ExtractCatalystData(link, sname)
        except Exception as e_ex:
            log.warn("Extract steering '{}' failed: {}", sname, e_ex)

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

    log.info("Steering proxies available (from XML): {}", found)
    if missing:
        log.warn("Steering proxies missing/not extracted: {}", missing)
    else:
        log.info("All XML-defined steering proxies are available.")

    # Refresh state copies for UI consumers
    try:
        ctx.state.steerable_proxies = expected_defs
        if not ctx.state.selected_steering_proxy and expected_defs:
            ctx.state.selected_steering_proxy = expected_defs[0]['name']
    except Exception:
        pass
