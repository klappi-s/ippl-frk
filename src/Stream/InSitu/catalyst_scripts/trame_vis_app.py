import asyncio
import time
from trame.app import get_server
from trame.ui.vuetify3 import SinglePageLayout
from trame.widgets import vuetify3, vtk

from paraview import simple
from paraview import live

import trame_render_config as render_config

# -----------------------------------------------------------------------------
# Trame Setup
# -----------------------------------------------------------------------------
server = get_server()
state = server.state
ctrl = server.controller

# Global variables
catalyst_link = None
source_proxy = None 

# -----------------------------------------------------------------------------
# Helper Functions
# -----------------------------------------------------------------------------

def find_source_by_name(base_name):
    """
    Robustly finds a source in the pipeline.
    """
    s = simple.FindSource(base_name)
    if s: return s
    
    sources = simple.GetSources()
    for (group, name), proxy in sources.items():
        if name.startswith(base_name):
            return proxy
    return None
    
def get_available_extract_names():
    """
    Queries the Catalyst In-Situ Proxy Manager for available channels.
    """
    if not catalyst_link:
        return []

    try:
        # Access the raw C++ object to get the InSitu Manager
        if hasattr(catalyst_link, "GetInsituProxyManager"):
            pm = catalyst_link.GetInsituProxyManager()
        else:
            pm = catalyst_link.GetClientSideObject().GetInsituProxyManager()

        if pm is None:
            return []

        # The 'sources' group in the Insitu Manager contains the available extracts
        num_proxies = pm.GetNumberOfProxies("sources")
        
        all_names = []
        for idx in range(num_proxies):
            name = pm.GetProxyName("sources", idx)
            if name:
                all_names.append(name)
        
        print(f"DEBUG: All found sources: {sorted(list(set(all_names)))}")

        names = []
        for name in all_names:
            # Filter out the generated filters (ending in .bunch, .box, .MergedBlocks, .Cell2Point, .Glyph)
            if any(name.endswith(suffix) for suffix in [".bunch", ".box", ".MergedBlocks", ".Cell2Point", ".Glyph", ".ResampleToImage"]):
                continue
            names.append(name)

        return sorted(list(set(names)))

    except Exception as e:
        print(f"[Error] Failed to query InsituProxyManager: {e}")
        return []

# -----------------------------------------------------------------------------
# Core Functions
# -----------------------------------------------------------------------------

def connect_to_catalyst():
    global catalyst_link
    cat_host = "localhost"
    cat_port = 22222
    
    try:
        print(f"Connecting to {cat_host}:{cat_port}...")
        catalyst_link = live.ConnectToCatalyst(ds_host=cat_host, ds_port=cat_port)
        
        if catalyst_link:
            state.connected = True
            state.status_text = f"Connected"
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
    
    names = get_available_extract_names()
    
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
        print(f"Exact match found: {current_input}")
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
    global source_proxy
    if not catalyst_link: return
    
    channel_name = state.selected_source 
    if not channel_name: return

    if state.has_data and source_proxy:
         print("Data pipeline already active.")
         return

    # Determine proxies to extract
    proxies_to_extract = [channel_name]
    if channel_name.startswith("ippl_sField"):
        proxies_to_extract.append(f"{channel_name}.MergedBlocks")
        # proxies_to_extract.append(f"{channel_name}.Cell2Point")
        # proxies_to_extract.append(f"{channel_name}.ResampleToImage")
    elif channel_name.startswith("ippl_vField"):
        proxies_to_extract.append(f"{channel_name}.Glyph")

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
        # primary_proxy_name = f"{channel_name}.Cell2Point"
        # primary_proxy_name = f"{channel_name}.ResampleToImage"

    # Wait Loop
    found_data = False
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
                    break
            except:
                pass
        time.sleep(0.1)

    if found_data:
        # Update global source_proxy to point to the primary one
        source_proxy = find_source_by_name(primary_proxy_name)
        
        # Get the active view
        view = simple.GetActiveView()
        
        # --- Dispatch based on name ---
        if channel_name.startswith("ippl_particles"):
            print(f"Applying Custom Particle Render for {channel_name}...")
            render_config.setup_particle_view(source_proxy, view)
        elif channel_name.startswith("ippl_sField"):
            print(f"Applying Scalar Field Render for {channel_name}...")
            # Use MergedBlocks as source
            merged_proxy = find_source_by_name(f"{channel_name}.MergedBlocks")
            if merged_proxy:
                render_config.setup_scalar_field_view(merged_proxy, view, channel_name)
            # else:
            #     render_config.setup_scalar_field_view(source_proxy, view, channel_name)
        elif channel_name.startswith("ippl_vField"):
            print(f"Applying Vector Field Render for {channel_name}...")
            glyph_proxy = find_source_by_name(f"{channel_name}.Glyph")
            if glyph_proxy:
                print(f"Found extracted Glyph filter: {channel_name}.Glyph")
                render_config.setup_vector_field_view(glyph_proxy, view, channel_name, is_extracted=True)
            else:
                print(f"Extracted Glyph filter not found. Creating local Glyph filter.")
                render_config.setup_vector_field_view(source_proxy, view, channel_name, is_extracted=False)
        else:
            print(f"Applying Default Render for {channel_name}...")
            render_config.setup_default_view(source_proxy, view)

        # Trigger update
        simple.Render()
        if hasattr(ctrl, 'view_update'):
            ctrl.view_update()
            
        state.has_data = True
        print(f"Extraction successful: {channel_name}")


def reset_visualization():
    global source_proxy
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

    sel = state.selected_source
    
    if sel:
        if sel.startswith("ippl_sField"):
             # p1 = find_source_by_name(f"{sel}.Cell2Point")
             # p1 = find_source_by_name(f"{sel}.ResampleToImage")
             p1 = find_source_by_name(f"{sel}_Resample") # Local filter
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
             
             if source_proxy: simple.Delete(source_proxy)
             p = find_source_by_name(sel)
             if p and p != source_proxy: simple.Delete(p)
        elif sel.startswith("ippl_particles"):
             # Clean up filters created in render_config
             p1 = find_source_by_name("Particles_Only")
             if p1: simple.Delete(p1)
             p2 = find_source_by_name("Merged_Particles")
             if p2: simple.Delete(p2)
             
             # Clean up main source
             if source_proxy: simple.Delete(source_proxy)
             p = find_source_by_name(sel)
             if p and p != source_proxy: simple.Delete(p)
        else:
             if source_proxy: simple.Delete(source_proxy)
             # Ensure parent is gone if source_proxy was different
             p = find_source_by_name(sel)
             if p and p != source_proxy: simple.Delete(p)
    
    source_proxy = None
        
    simple.Render()
    if hasattr(ctrl, 'view_update'):
        ctrl.view_update()

# ... (Polling and Steering) ...
async def poll_catalyst():
    while True:
        if catalyst_link:
            try:
                live.ProcessServerNotifications()
                if state.live_mode and state.has_data:
                    sel = state.selected_source
                    
                    # --- CALL UPDATE LOGIC ---
                    if sel.startswith("ippl_sField"):
                        # c2p_proxy = find_source_by_name(f"{sel}.Cell2Point")
                        # if c2p_proxy: 
                        #     c2p_proxy.UpdatePipeline()
                        #     render_config.update_scalar_field_view(c2p_proxy, simple.GetActiveView())
                        # resample_proxy = find_source_by_name(f"{sel}.ResampleToImage")
                        # if resample_proxy: 
                        #     resample_proxy.UpdatePipeline()
                        #     render_config.update_scalar_field_view(resample_proxy, simple.GetActiveView())
                        
                        # Update base proxy
                        if source_proxy: source_proxy.UpdatePipeline()
                        
                        # Update MergedBlocks
                        merged_proxy = find_source_by_name(f"{sel}.MergedBlocks")
                        if merged_proxy: merged_proxy.UpdatePipeline()

                        # Update local filter if exists
                        resample_proxy = find_source_by_name(f"{sel}_Resample")
                        if resample_proxy:
                            resample_proxy.UpdatePipeline()
                            render_config.update_scalar_field_view(resample_proxy, simple.GetActiveView())
                            
                    elif sel.startswith("ippl_vField"):
                        if source_proxy: source_proxy.UpdatePipeline()
                        
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
                        if source_proxy: 
                            source_proxy.UpdatePipeline()
                            if sel.startswith("ippl_particles"):
                                render_config.update_particle_view(source_proxy, simple.GetActiveView())
                    
                    simple.Render()
                    if hasattr(ctrl, 'view_update'): ctrl.view_update()
            except Exception: pass
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

# -----------------------------------------------------------------------------
# GUI Layout
# -----------------------------------------------------------------------------
with SinglePageLayout(server) as layout:
    layout.title.set_text("Catalyst Control Center")

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
            disabled=("!connected || has_data",),
            density="compact",
            hide_details=True,
            style="max-width: 250px;",
            classes="mx-1"
        )
        
        # --- SEARCH BUTTON (Pick Best Match) ---
        with vuetify3.VBtn(
            icon=True, 
            click=search_and_select_best, 
            disabled=("!connected || has_data",), 
            variant="text", 
            color="primary"
        ):
            # vuetify3.VIcon("mdi-crosshairs-gps")
            vuetify3.VIcon("mdi-magnify")
            vuetify3.VTooltip(activator="parent", location="bottom", text="[Enter] Find Best Match")

        # Initialize & Reset
        vuetify3.VBtn("Initialize", click=extract_data, disabled=("!connected || has_data || !selected_source",), variant="text")
        with vuetify3.VBtn(icon=True, click=reset_visualization, disabled=("!has_data",), color="error", variant="text"):
            vuetify3.VIcon("mdi-refresh")
        
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

state.connected = False
state.has_data = False
state.live_mode = False 
state.status_text = "Disconnected"
state.sim_paused = False
state.available_sources = [] 
state.selected_source = None

@state.change("connected")
def update_color(connected, **kwargs):
    state.status_color = "success" if connected else "grey" # Changed status chip to success green too for consistency

ctrl.resume_polling = lambda: asyncio.create_task(poll_catalyst())

if __name__ == "__main__":
    simple.GetRenderView()
    server.start()