import os
import xml.etree.ElementTree as ET
from paraview import simple, servermanager

# Ensure we always read catalyst_proxy.xml from the catalyst_scripts folder, not trame_app
_TRAME_APP_DIR = os.path.dirname(os.path.abspath(__file__))
_CATALYST_SCRIPTS_DIR = os.path.dirname(_TRAME_APP_DIR)

def get_xml_path():
    return os.path.join(_CATALYST_SCRIPTS_DIR, "catalyst_proxy.xml")

def preload_steerable_proxies():
    """Load definitions into the global servermanager before connection."""
    xml_path = get_xml_path()
    if not os.path.exists(xml_path):
        print(f"[ERROR] catalyst_proxy.xml not found at {xml_path}")
        return

    try:
        with open(xml_path, 'r') as f:
            xml_content = f.read()
        try:
            pm = servermanager.vtkSMProxyManager.GetProxyManager()
            if pm and hasattr(pm, 'GetProxyDefinitionManager'):
                pdm = pm.GetProxyDefinitionManager()
                if pdm:
                    pdm.LoadConfigurationXMLFromString(xml_content)
                    print(f"[DEBUG] Pre-loaded steerable proxies into global definition manager")
        except Exception as e:
            print(f"[WARN] Failed to pre-load into global manager: {e}")
        if servermanager.ActiveConnection:
             try:
                 pm = servermanager.ActiveConnection.Session.GetSessionProxyManager()
                 if hasattr(pm, 'LoadConfigurationXMLFromString'):
                     pm.LoadConfigurationXMLFromString(xml_content)
                 else:
                     pm.LoadConfigurationXML(xml_content)
                 print("[DEBUG] Pre-loaded steerable proxies into active session")
             except Exception as e:
                 print(f"[WARN] Failed to pre-load into active session: {e}")
        else:
             print("[WARN] No ActiveConnection found during pre-load")
    except Exception as e:
        print(f"[ERROR] Failed to read/load steerable proxies: {e}")

def load_steerable_proxies(proxy_manager=None):
    """Load the catalyst_proxy.xml to register steerable proxy definitions."""
    xml_path = get_xml_path()
    if os.path.exists(xml_path):
        try:
            with open(xml_path, 'r') as f:
                xml_content = f.read()
            if proxy_manager:
                print(f"[DEBUG] Loading steerable proxies into provided proxy manager")
                if hasattr(proxy_manager, 'LoadConfigurationXMLFromString'):
                    proxy_manager.LoadConfigurationXMLFromString(xml_content)
                elif hasattr(proxy_manager, 'LoadConfigurationXML'):
                    proxy_manager.LoadConfigurationXML(xml_content)
                elif hasattr(proxy_manager, 'GetProxyDefinitionManager'):
                    proxy_manager.GetProxyDefinitionManager().LoadConfigurationXMLFromString(xml_content)
                return
            if servermanager.ActiveConnection:
                 pm = servermanager.ActiveConnection.Session.GetSessionProxyManager()
                 if hasattr(pm, 'LoadConfigurationXMLFromString'):
                     pm.LoadConfigurationXMLFromString(xml_content)
                 else:
                     pm.LoadConfigurationXML(xml_content)
                 print("[DEBUG] Loaded steerable proxies into active session")
        except Exception as e:
            print(f"[ERROR] Failed to load steerable proxies: {e}")
    else:
        print(f"[ERROR] catalyst_proxy.xml not found at {xml_path}")

def get_steerable_proxy(catalyst_link, proxy_name=None):
    """Find the steerable proxy from the insitu proxy manager across common groups.

    Tries groups in order: sources, filters, insitu, misc. Matches by registered name
    and by XMLName. Also supports safe_name variants (':' replaced with '_').
    """
    if not catalyst_link:
        return None
    try:
        pm = catalyst_link.GetInsituProxyManager()
    except Exception:
        return None
    if not pm:
        return None

    groups = ["sources", "filters", "insitu", "misc"]

    # Known alias mappings to bridge UI names to registered names/XML names
    alias_map = {
        # Common prefix mismatch: UI uses SteerableParameters_ but runtime lists Steering*
        "SteerableParameters_SCALARS": [
            "SteerableParameters_scalars",
            "SteerableParameters_SCALARS",
            "SteeringParameters_SCALARS",
            "Steering_SCALARS",
        ],
        # Array variants may register with '_' instead of ':' and may drop 'Parameters'
        "SteerableParameters_array:LinMap_array": [
            "SteerableParameters_array_LinMap_array",
            "Steering_array:LinMap_array",
            "Steering_array_LinMap_array",
        ],
        "SteerableParameters_array:SimpParams_arrays": [
            "SteerableParameters_array_SimpParams_arrays",
            "Steering_array:SimpParams_arrays",
            "Steering_array_SimpParams_arrays",
        ],
        "SteerableParameters_bool_array": ["Steering_bool_array"],
        "SteerableParameters_button_array": ["Steering_button_array"],
        "SteerableParameters_double_array": ["Steering_double_array"],
        "SteerableParameters_double_array_2": ["Steering_double_array_2"],
        "SteerableParameters_double_vector_array1": ["Steering_double_vector_array1"],
        "SteerableParameters_double_vector_array2": ["Steering_double_vector_array2"],
        "SteerableParameters_double_vector_array3": ["Steering_double_vector_array3"],
        "SteerableParameters_enum_array": ["Steering_enum_array"],
        "SteerableParameters_int_array": ["Steering_int_array"],
        "SteerableParameters_int_vector_array1": ["Steering_int_vector_array1"],
        "SteerableParameters_int_vector_array2": ["Steering_int_vector_array2"],
        "SteerableParameters_int_vector_array3": ["Steering_int_vector_array3"],
    }

    def search_by_xml_name(target_xml_name: str):
        for g in groups:
            try:
                n = pm.GetNumberOfProxies(g)
            except Exception:
                continue
            for i in range(n):
                try:
                    name = pm.GetProxyName(g, i)
                    p = pm.GetProxy(g, name)
                    if p and p.GetXMLName() == target_xml_name:
                        return p
                except Exception:
                    continue
        return None

    if proxy_name:
        candidates = [proxy_name]
        # Add alias candidates
        candidates.extend(alias_map.get(proxy_name, []))
        # Add safe_name variant
        candidates.append(proxy_name.replace(':', '_'))
        # Also try lowercased variants
        candidates.extend(list({c for c in [proxy_name.lower(), proxy_name.replace(':', '_').lower()]}))

        # Try direct lookup in all groups
        for candidate in candidates:
            for g in groups:
                try:
                    p = pm.GetProxy(g, candidate)
                    if p:
                        return p
                except Exception:
                    pass
            # Fallback: try ParaView source registry
            try:
                p = simple.FindSource(candidate)
                if p:
                    return p
            except Exception:
                pass
        # Try by XMLName match
        # Try all candidate XMLName matches (case-insensitive)
        for candidate in candidates:
            p = search_by_xml_name(candidate)
            if p:
                return p
        # Finally try case-insensitive XMLName compare
        for g in groups:
            try:
                n = pm.GetNumberOfProxies(g)
            except Exception:
                continue
            for i in range(n):
                try:
                    name = pm.GetProxyName(g, i)
                    p = pm.GetProxy(g, name)
                    if p and p.GetXMLName().lower() == proxy_name.lower():
                        return p
                except Exception:
                    continue
        # Final fallback: try FindSource on the original name
        try:
            p = simple.FindSource(proxy_name)
            if p:
                return p
        except Exception:
            pass
        if p:
            return p
        return None

    # No specific name requested: return first steerable proxy we find
    for g in groups:
        try:
            n = pm.GetNumberOfProxies(g)
        except Exception:
            continue
        for i in range(n):
            try:
                name = pm.GetProxyName(g, i)
                p = pm.GetProxy(g, name)
                if p and p.GetXMLName().startswith("SteerableParameters"):
                    return p
            except Exception:
                continue
    return None

def debug_list_insitu_proxies(catalyst_link):
    """Utility to print available proxies and their XML names across groups."""
    if not catalyst_link:
        print("[DEBUG] No catalyst_link for debug list")
        return
    try:
        pm = catalyst_link.GetInsituProxyManager()
    except Exception:
        print("[DEBUG] No insitu proxy manager")
        return
    groups = ["sources", "filters", "insitu", "misc"]
    for g in groups:
        try:
            n = pm.GetNumberOfProxies(g)
        except Exception:
            continue
        print(f"[DEBUG] Group '{g}' has {n} proxies")
        for i in range(n):
            try:
                name = pm.GetProxyName(g, i)
                p = pm.GetProxy(g, name)
                xmln = p.GetXMLName() if p else "?"
                print(f"[DEBUG]  - {g}:{name} (XMLName={xmln})")
            except Exception:
                continue

def update_steering_parameter(catalyst_link, proxy_name, param_name, value):
    """Update a property on the steerable proxy."""
    proxy = get_steerable_proxy(catalyst_link, proxy_name)
    if proxy:
        print(f"[DEBUG] Updating {proxy_name}.{param_name} to {value} (type={type(value)}, len={len(value) if isinstance(value, list) else 'N/A'})")
        prop = proxy.GetProperty(param_name)
        if prop:
            def to_int(x):
                if x is None: return 0
                if isinstance(x, bool): return int(x)
                if isinstance(x, (int, float)): return int(x)
                try:
                    return int(str(x))
                except Exception:
                    sx = str(x).strip()
                    if sx.lower() in ("true", "on", "yes"): return 1
                    if sx.lower() in ("false", "off", "no"): return 0
                    try:
                        return int(float(sx))
                    except Exception:
                        return 0
            def to_float(x):
                if x is None: return 0.0
                if isinstance(x, bool): return 1.0 if x else 0.0
                if isinstance(x, (int, float)): return float(x)
                try:
                    return float(str(x))
                except Exception:
                    sx = str(x).strip()
                    try:
                        return float(sx)
                    except Exception:
                        return 0.0
            is_int_prop = prop.IsA("vtkSMIntVectorProperty")
            
            if isinstance(value, list):
                flat_val = []
                for v in value:
                    if isinstance(v, list):
                        flat_val.extend(v)
                    else:
                        flat_val.append(v)
                casted = [to_int(x) if is_int_prop else to_float(x) for x in flat_val]
                
                # CRITICAL: For steering proxies with use_index='1' and repeat_command='1',
                # we MUST clear the array first, then set elements to trigger proper command invocations
                # Get clean command if available
                clean_cmd = None
                if hasattr(prop, 'GetCleanCommand') and prop.GetCleanCommand():
                    clean_cmd = prop.GetCleanCommand()
                
                if clean_cmd:
                    # Call the clean command (usually "Clear") with the array name
                    # The initial_string in XML is the array name
                    try:
                        initial_string = None
                        if hasattr(prop, 'GetInitialString') and prop.GetInitialString():
                            initial_string = prop.GetInitialString()
                        
                        if initial_string:
                            # Invoke clean command with array name
                            proxy_obj = proxy
                            if hasattr(proxy, 'GetClientSideObject'):
                                proxy_obj = proxy.GetClientSideObject()
                            if hasattr(proxy_obj, clean_cmd):
                                getattr(proxy_obj, clean_cmd)(initial_string)
                                print(f"[DEBUG] Called {clean_cmd}('{initial_string}') on proxy")
                    except Exception as e:
                        print(f"[DEBUG] Could not call clean command: {e}")
                
                # Now set all elements - this will trigger repeat_command for each tuple
                prop.SetNumberOfElements(len(casted))
                for idx, val in enumerate(casted):
                    prop.SetElement(idx, val)
                print(f"[DEBUG] Set {len(casted)} elements for {param_name}")
            else:
                casted = to_int(value) if is_int_prop else to_float(value)
                prop.SetElement(0, casted)
                print(f"[DEBUG] Set 1 element for {param_name}: {casted}")
            
            # Verify what was set
            try:
                num_elems = prop.GetNumberOfElements()
                print(f"[DEBUG] After set, property {param_name} has {num_elems} elements")
            except Exception:
                pass
                
            proxy.UpdateVTKObjects()
            print(f"[DEBUG] UpdateVTKObjects() called for {proxy_name}")
            
            # Force update of the pipeline to ensure changes propagate
            try:
                if hasattr(proxy, 'UpdatePipeline'):
                    proxy.UpdatePipeline()
                    print(f"[DEBUG] UpdatePipeline() called for {proxy_name}")
            except Exception as e:
                print(f"[DEBUG] UpdatePipeline failed (may not be needed): {e}")
        else:
             print(f"[WARN] Property {param_name} not found on {proxy_name}")
    else:
        print(f"[DEBUG] Steerable proxy {proxy_name} not found")

def parse_steerable_parameters(base_path=None):
    """Parse catalyst_proxy.xml to extract steerable parameters."""
    if base_path is None:
        # Default to catalyst_scripts directory
        base_path = _CATALYST_SCRIPTS_DIR
    xml_path = os.path.join(base_path, "catalyst_proxy.xml")
    if not os.path.exists(xml_path):
        return []
    try:
        tree = ET.parse(xml_path)
        root = tree.getroot()
        proxies_list = []
        sources_group = root.find(".//ProxyGroup[@name='sources']")
        if sources_group is None:
            return []
        def extract_enum(prop_element):
            enum_domain = prop_element.find("EnumerationDomain")
            if enum_domain is not None:
                items = []
                for entry in enum_domain.findall("Entry"):
                    items.append({"title": entry.get("text"), "value": int(entry.get("value"))})
                return items
            return None
        for proxy in sources_group.findall("SourceProxy"):
            proxy_name = proxy.get('name')
            if not proxy_name.startswith("SteerableParameters_"):
                continue
            is_array = "_array" in proxy_name
            raw_params = []
            param_map = {}
            for prop in proxy:
                if prop.tag not in ['DoubleVectorProperty', 'IntVectorProperty', 'StringVectorProperty']:
                    continue
                name = prop.get('name')
                label = prop.get('label', name)
                panel_visibility = prop.get('panel_visibility', 'default')
                if panel_visibility == 'never':
                    continue
                param_type = prop.tag
                default_str = prop.get('default_values', '')
                default_values = default_str.split()
                num_elements_str = prop.get('number_of_elements')
                if num_elements_str:
                    num_elements = int(num_elements_str)
                else:
                    command = prop.get('command', '')
                    if 'SetTuple2' in command: num_elements = 2
                    elif 'SetTuple3' in command: num_elements = 3
                    elif 'SetTuple4' in command: num_elements = 4
                    elif 'SetTuple6' in command: num_elements = 6
                    elif 'SetTuple9' in command: num_elements = 9
                    else: num_elements = 1
                widget_type = 'text'
                domain = {}
                if prop.get('panel_widget') == 'CheckBox':
                    widget_type = 'checkbox'
                elif prop.find("BooleanDomain") is not None:
                    widget_type = 'checkbox'
                elif prop.find("EnumerationDomain") is not None:
                    items = extract_enum(prop)
                    if items:
                        widget_type = 'select'
                        domain['items'] = items
                else:
                    range_domain = prop.find("DoubleRangeDomain") or prop.find("IntRangeDomain")
                    if range_domain is not None:
                        min_val = range_domain.get("min")
                        max_val = range_domain.get("max")
                        if min_val is not None and max_val is not None:
                            widget_type = 'slider'
                            domain['min'] = float(min_val) if 'Double' in param_type else int(min_val)
                            domain['max'] = float(max_val) if 'Double' in param_type else int(max_val)
                p_def = {
                    "name": name,
                    "safe_name": name.replace('.', '_').replace(':', '_'),
                    "label": label,
                    "type": param_type,
                    "default": default_values,
                    "num_elements": num_elements,
                    "widget": widget_type,
                    "domain": domain,
                    "item_type": "parameter"
                }
                raw_params.append(p_def)
                param_map[name] = p_def
            groups = []
            used_props = set()
            for group in proxy.findall("PropertyGroup"):
                group_label = group.get('label')
                children = []
                for prop_ref in group.findall("Property"):
                    p_name = prop_ref.get('name')
                    if p_name in param_map:
                        p_def = param_map[p_name]
                        p_def['function'] = prop_ref.get('function', '')
                        if '.' in p_name:
                            parts = p_name.split('.')
                            if parts[0] == group_label:
                                p_def['label'] = ".".join(parts[1:])
                            else:
                                p_def['label'] = parts[-1]
                        if p_def['widget'] == 'text': 
                            mapped_name = prop_ref.get('function')
                            hints = group.find("Hints")
                            if hints is not None:
                                proto_ref = hints.find("PropertyCollectionWidgetPrototype")
                                if proto_ref is not None:
                                    proto_name = proto_ref.get('name')
                                    proto_group = proto_ref.get('group', 'misc')
                                    pg = root.find(f"ProxyGroup[@name='{proto_group}']")
                                    if pg is not None:
                                        proto_proxy = pg.find(f"Proxy[@name='{proto_name}']")
                                        if proto_proxy is not None:
                                            proto_prop = proto_proxy.find(f"*[@name='{mapped_name}']")
                                            if proto_prop is not None:
                                                panel_widget = proto_prop.get('panel_widget')
                                                if panel_widget == 'CheckBox' or proto_prop.find('BooleanDomain') is not None:
                                                    p_def['widget'] = 'checkbox'
                                                items = extract_enum(proto_prop)
                                                if items:
                                                    p_def['widget'] = 'select'
                                                    p_def['domain']['items'] = items
                                                range_domain = proto_prop.find("DoubleRangeDomain") or proto_prop.find("IntRangeDomain")
                                                if range_domain is not None:
                                                    min_val = range_domain.get("min")
                                                    max_val = range_domain.get("max")
                                                    if min_val is not None and max_val is not None:
                                                        p_def['widget'] = 'slider'
                                                        p_def['domain']['min'] = float(min_val) if 'Double' in p_def['type'] else int(min_val)
                                                        p_def['domain']['max'] = float(max_val) if 'Double' in p_def['type'] else int(max_val)
                        children.append(p_def)
                        used_props.add(p_name)
                if children:
                    groups.append({
                        "item_type": "group",
                        "label": group_label,
                        "children": children
                    })
            final_structure = []
            for p in raw_params:
                if p['name'] not in used_props:
                    final_structure.append(p)
            final_structure.extend(groups)
            proxies_list.append({
                "name": proxy_name,
                "safe_name": proxy_name.replace(':', '_'),
                "label": proxy_name.replace("SteerableParameters_", ""),
                "type": "array" if is_array else "scalar",
                "children": final_structure
            })
        return proxies_list
    except Exception as e:
        print(f"[ERROR] Failed to parse catalyst_proxy.xml: {e}")
        return []