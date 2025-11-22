#pragma once

#include "Stream/InSitu/CatalystAdaptor.h"
#include "Stream/InSitu/CatalystAdaptor.hpp"
#include <array>




namespace ippl{



// =====================================================================================
// INITIALISATION:
// =====================================================================================

template<typename T>
requires (!std::is_enum_v<std::decay_t<T>>)
void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const T& steerable_scalar_forwardpass,  const std::string& label ){
    ca_m << "::Initialize()::InitSteerableChannel(" << label << "):  | Type: " << typeid(T).name() << endl;
    proxyWriter.include(steerable_scalar_forwardpass, label);
    conduit_cpp::Node script_args = node["catalyst/scripts/script/args"];
    script_args.append().set_string(label);
}

// Enum overload (explicit) to ensure proper dropdown setup even if template above is shadowed
template<typename E>
requires (std::is_enum_v<std::decay_t<E>>)
void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const E& e, const std::string& label ){
    ca_m << "::Initialize()::InitSteerableChannel(" << label << "):  | Type: Enum" << endl;
    auto it = enumChoices_.find(label);
    if (it != enumChoices_.end()) {
        proxyWriter.includeEnum(label, it->second, static_cast<int>(e));
    } else {
        // Fallback to an int-like property without dropdown (simplest path is to use Int checkbox placeholder replaced above)
        proxyWriter.includeBool(label, false);
    }
    conduit_cpp::Node script_args = node["catalyst/scripts/script/args"];
    script_args.append().set_string(label);
}

// Bool-like Switch: init (checkbox in GUI)
void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const bool& sw, const std::string& label ){
    ca_m << "::Initialize()::InitSteerableChannel(" << label << "):  | Type: Switch" << endl;
    proxyWriter.includeBool(label, static_cast<bool>(sw));

    conduit_cpp::Node script_args = node["catalyst/scripts/script/args"];
    script_args.append().set_string(label);
}

// Button-like: init (push button in GUI)
void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const ippl::Button& btn, const std::string& label ){
    ca_m << "::Initialize()::InitSteerableChannel(" << label << "):  | Type: Button" << endl;
    proxyWriter.includeButton(label);
    conduit_cpp::Node script_args = node["catalyst/scripts/script/args"];
    script_args.append().set_string(label);
}

// Vector steerable overloads
template<typename T, unsigned Dim_v>
void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const ippl::Vector<T, Dim_v>& steerable_vec_forwardpass, const std::string& label )
{
    ca_m << "::Initialize()::InitSteerableChannel(" << label << "):  | Vector<" << typeid(T).name() << "," << Dim_v << ">" << endl;
    // Register this label as a vector channel in the proxy writer (limit to 3 comps in GUI)
    proxyWriter.includeVector<T, Dim_v>(label);
    (void)steerable_vec_forwardpass;

    // Ensure the Python pipeline receives this label after `--steer_channel_names`
    // so it creates the corresponding forward reader and unified sender wiring.
    conduit_cpp::Node script_args = node["catalyst/scripts/script/args"];
    script_args.append().set_string(label);
}


// LinMap steerable: grouped under one logical label, registers 3 vectors and a time scalar
inline void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const ippl::LinMap& lm, const std::string& label )
{
    ca_m << "::Initialize()::InitSteerableChannel(" << label << "):  | Type: LinMap" << endl;
    // Register as a LinMap group with initial defaults from the current values; time rendered as textbox
    std::array<double,3> xd{ lm.x_row[0], lm.x_row[1], lm.x_row[2] };
    std::array<double,3> yd{ lm.y_row[0], lm.y_row[1], lm.y_row[2] };
    std::array<double,3> zd{ lm.z_row[0], lm.z_row[1], lm.z_row[2] };
    proxyWriter.includeLinMapWithDefaults(label, xd, yd, zd, lm.time);

    conduit_cpp::Node script_args = node["catalyst/scripts/script/args"];
    script_args.append().set_string(label + "_x_row");
    script_args.append().set_string(label + "_y_row");
    script_args.append().set_string(label + "_z_row");
    script_args.append().set_string(label + "_time");
}

// LinMaps steerable: dynamic lists (SoA) across all maps; no ProxyWriter involvement (XML-driven)
inline void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const ippl::LinMaps& lms, const std::string& label )
{
    ca_m << "::Initialize()::InitSteerableChannel(" << label << "):  | Type: LinMaps (dynamic lists)" << endl;
    // For XML-driven approach, just inform the script about property names
    conduit_cpp::Node script_args = node["catalyst/scripts/script/args"];
    script_args.append().set_string("Map_x_row");
    script_args.append().set_string("Map_y_row");
    script_args.append().set_string("Map_z_row");
    script_args.append().set_string("Map_time");

    // Also publish initial forward arrays so that CatalystInitializePropertiesWithMesh
    // can find them immediately on proxy creation (before first execute).
    // This mirrors AddSteerableChannel(LinMaps) but runs during initialize.
    const size_t N = lms.time.size();
    auto steerable_channel = node["catalyst/channels/steerable_channel_0D_mesh"];
    steerable_channel["type"].set("mesh");
    auto steerable_data = steerable_channel["data"];
    steerable_data["coordsets/coords/type"].set_string("explicit");

    {
        std::vector<double> xs; xs.reserve(N);
        for (size_t i = 0; i < N; ++i) xs.push_back(static_cast<double>(i));
        steerable_data["coordsets/coords/values/x"].set(xs);
    }

    steerable_data["topologies/sMesh_topo/type"].set("unstructured");
    steerable_data["topologies/sMesh_topo/coordset"].set("coords");
    steerable_data["topologies/sMesh_topo/elements/shape"].set("point");
    {
        std::vector<int32_t> conn; conn.reserve(N);
        for (int32_t i = 0; i < static_cast<int32_t>(N); ++i) conn.push_back(i);
        steerable_data["topologies/sMesh_topo/elements/connectivity"].set(conn);
    }

    // time field
    {
        auto f = steerable_data["fields/steerable_field_f_Map_time"];
        f["association"].set("vertex");
        f["topology"].set("sMesh_topo");
        f["volume_dependent"].set("false");
        std::vector<double> timeVals; timeVals.reserve(N);
        for (size_t i = 0; i < N; ++i) timeVals.push_back(lms.time[i]);
        f["values"].set(timeVals);
    }

    auto set_vec3_field = [&](const std::string& base, const std::vector<ippl::Vector<double,3>>& rows){
        auto f = steerable_data[std::string("fields/steerable_field_f_") + base];
        f["association"].set("vertex");
        f["topology"].set("sMesh_topo");
        f["volume_dependent"].set("false");
        std::vector<double> xs; xs.reserve(N);
        std::vector<double> ys; ys.reserve(N);
        std::vector<double> zs; zs.reserve(N);
        for (size_t i = 0; i < N; ++i) {
            const auto& v = rows[i];
            xs.push_back(v[0]); ys.push_back(v[1]); zs.push_back(v[2]);
        }
        f["values/x"].set(xs);
        f["values/y"].set(ys);
        f["values/z"].set(zs);
    };

    set_vec3_field("Map_x_row", lms.x_row);
    set_vec3_field("Map_y_row", lms.y_row);
    set_vec3_field("Map_z_row", lms.z_row);
}

// Init: std::vector<LinMap> (AoS) -> reuse LinMaps (SoA) initialization for identical GUI wiring
inline void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const std::vector<ippl::LinMap>& lm_vec, const std::string& label )
{
    ca_m << "::Initialize()::InitSteerableChannel(" << label << "):  | Type: vector<LinMap> (AoS alias LinMaps)" << endl;
    ippl::LinMaps tmp;
    const size_t N = lm_vec.size();
    tmp.time.reserve(N);
    tmp.x_row.reserve(N);
    tmp.y_row.reserve(N);
    tmp.z_row.reserve(N);
    for(const auto& m : lm_vec){
        tmp.time.push_back(m.time);
        tmp.x_row.push_back(m.x_row);
        tmp.y_row.push_back(m.y_row);
        tmp.z_row.push_back(m.z_row);
    }
    InitSteerableChannel(tmp, label);
}



// =====================================================================================
// SETTING UP CONDUIT NODE:
// =====================================================================================


template<typename T>
requires (!std::is_enum_v<std::decay_t<T>>)
void CatalystAdaptor::AddSteerableChannel( const T& steerable_scalar_forwardpass,  const std::string& steerable_suffix ) 
{
        ca_m << "::Execute()::AddSteerableChannel(" << steerable_suffix << ");  | Type: " << typeid(T).name() << endl;
        
    auto steerable_channel = node["catalyst/channels/steerable_channel_0D_mesh"];
        // auto steerable_channel = node["catalyst/channels/steerable_channels_forward_all"];

        steerable_channel["type"].set("mesh");
        auto steerable_data = steerable_channel["data"];
        steerable_data["coordsets/coords/type"].set_string("explicit");
        steerable_data["coordsets/coords/values/x"].set( 0 );

        steerable_data["topologies/sMesh_topo/type"].set("unstructured");
        steerable_data["topologies/sMesh_topo/coordset"].set("coords");
        steerable_data["topologies/sMesh_topo/elements/shape"].set("point");
        steerable_data["topologies/sMesh_topo/elements/connectivity"].set( 0 );
        
        /* 3D double is not mandatory ?? and irrelevant we want to pass minimal vtk object with 1 data point */
        // steerable_data["coordsets/coords/values/x"].set_float64_vector({ 0 });
        // steerable_data["coordsets/coords/values/y"].set_float64_vector({ 0 });
        // steerable_data["coordsets/coords/values/z"].set_float64_vector({ 0 });

        // steerable_data["topologies/sMesh_topo/elements/connectivity"].set_int32_vector({ 0 });


        conduit_cpp::Node steerable_field = steerable_data["fields/steerable_field_f_" + steerable_suffix];
        steerable_field["association"].set("vertex");
        steerable_field["topology"].set("sMesh_topo");
        steerable_field["volume_dependent"].set("false");

        conduit_cpp::Node values = steerable_field["values"];

        // std::is_scalar_v<std::decay_t<T>>
        if constexpr(std::is_enum_v<std::decay_t<T>>){
            values.set(static_cast<int>(steerable_scalar_forwardpass));
        }
        else if constexpr(std::is_scalar_v<T>){
            values.set(steerable_scalar_forwardpass);
            // lastForwardSteerVal[steerable_suffix] = static_cast<double>(steerable_scalar_forwardpass);
        }        
        else {
            throw IpplException("Stream::InSitu::CatalystAdaptor::AddSteerableChannel", "Unsupported steerable type for channel: " + steerable_suffix);
        }
}

// Enum overload explicit (clarity)
template<typename E>
requires (std::is_enum_v<std::decay_t<E>>)
void CatalystAdaptor::AddSteerableChannel( const E& e, const std::string& steerable_suffix )
{
    ca_m << "::Execute()::AddSteerableChannel(" << steerable_suffix << ");  | Type: Enum" << endl;
    auto steerable_channel = node["catalyst/channels/steerable_channel_0D_mesh"];
    steerable_channel["type"].set("mesh");
    auto steerable_data = steerable_channel["data"];    
    steerable_data["coordsets/coords/type"].set_string("explicit");
    steerable_data["coordsets/coords/values/x"].set( 0 );
    steerable_data["topologies/sMesh_topo/type"].set("unstructured");
    steerable_data["topologies/sMesh_topo/coordset"].set("coords");
    steerable_data["topologies/sMesh_topo/elements/shape"].set("point");
    steerable_data["topologies/sMesh_topo/elements/connectivity"].set( 0 );
    auto steerable_field = steerable_data["fields/steerable_field_f_" + steerable_suffix];
    steerable_field["association"].set("vertex");
    steerable_field["topology"].set("sMesh_topo");
    steerable_field["volume_dependent"].set("false");
    steerable_field["values"].set(static_cast<int>(e));
}

// Bool-like Switch: forward as single-sample scalar (0/1)
void CatalystAdaptor::AddSteerableChannel( const bool& sw, const std::string& steerable_suffix )
{
    ca_m << "::Execute()::AddSteerableChannel(" << steerable_suffix << ");  | Type: bool/Switch" << endl;
    
    auto steerable_channel = node["catalyst/channels/steerable_channel_0D_mesh"];
    steerable_channel["type"].set("mesh");
    auto steerable_data = steerable_channel["data"];
    steerable_data["coordsets/coords/type"].set_string("explicit");
    steerable_data["coordsets/coords/values/x"].set( 0 );

    steerable_data["topologies/sMesh_topo/type"].set("unstructured");
    steerable_data["topologies/sMesh_topo/coordset"].set("coords");
    steerable_data["topologies/sMesh_topo/elements/shape"].set("point");
    steerable_data["topologies/sMesh_topo/elements/connectivity"].set( 0 );

    conduit_cpp::Node steerable_field = steerable_data["fields/steerable_field_f_" + steerable_suffix];
    steerable_field["association"].set("vertex");
    steerable_field["topology"].set("sMesh_topo");
    steerable_field["volume_dependent"].set("false");
    
    int bool_as_int = sw ? 1 : 0;
    steerable_field["values"].set(bool_as_int);
}

// Button-like: forward as single-sample scalar (0/1)
void CatalystAdaptor::AddSteerableChannel( const ippl::Button& btn, const std::string& steerable_suffix )
{
    ca_m << "::Execute()::AddSteerableChannel(" << steerable_suffix << ");  | Type: Button" << endl;
    auto steerable_channel = node["catalyst/channels/steerable_channel_0D_mesh"];
    steerable_channel["type"].set("mesh");
    auto steerable_data = steerable_channel["data"];
    steerable_data["coordsets/coords/type"].set_string("explicit");
    steerable_data["coordsets/coords/values/x"].set( 0 );

    steerable_data["topologies/sMesh_topo/type"].set("unstructured");
    steerable_data["topologies/sMesh_topo/coordset"].set("coords");
    steerable_data["topologies/sMesh_topo/elements/shape"].set("point");
    steerable_data["topologies/sMesh_topo/elements/connectivity"].set( 0 );

    conduit_cpp::Node steerable_field = steerable_data["fields/steerable_field_f_" + steerable_suffix];
    steerable_field["association"].set("vertex");
    steerable_field["topology"].set("sMesh_topo");
    steerable_field["volume_dependent"].set("false");
    steerable_field["values"].set(static_cast<int>(btn ? 1 : 0));
}

// ippl::Vector overload
template<typename T, unsigned Dim_v>
void CatalystAdaptor::AddSteerableChannel( const ippl::Vector<T, Dim_v>& steerable_vec_forwardpass, const std::string& steerable_suffix )
{
    ca_m << "::Execute()::AddSteerableChannel(" << steerable_suffix << ");  | Vector<" << typeid(T).name() << "," << Dim_v << ">" << endl;

    auto steerable_channel = node["catalyst/channels/steerable_channel_0D_mesh"];
    steerable_channel["type"].set("mesh");
    auto steerable_data = steerable_channel["data"];
    steerable_data["coordsets/coords/type"].set_string("explicit");
    steerable_data["coordsets/coords/values/x"].set(0);

    steerable_data["topologies/sMesh_topo/type"].set("unstructured");
    steerable_data["topologies/sMesh_topo/coordset"].set("coords");
    steerable_data["topologies/sMesh_topo/elements/shape"].set("point");
    steerable_data["topologies/sMesh_topo/elements/connectivity"].set(0);

    // Single vector field with 3 components (x,y,z) under one name
    const std::string base = std::string("fields/steerable_field_f_") + steerable_suffix;
    // Ensure gotX/Y/Z are defined before use, fix the summary logging, and print time per-map.
    auto fnode = steerable_data[base];
    fnode["association"].set_string("vertex");
    fnode["topology"].set_string("sMesh_topo");
    fnode["volume_dependent"].set_string("false");

    const double vx = (Dim_v >= 1) ? static_cast<double>(steerable_vec_forwardpass[0]) : 0.0;
    const double vy = (Dim_v >= 2) ? static_cast<double>(steerable_vec_forwardpass[1]) : 0.0;
    const double vz = (Dim_v >= 3) ? static_cast<double>(steerable_vec_forwardpass[2]) : 0.0;

    fnode["values/x"].set(vx);
    fnode["values/y"].set(vy);
    fnode["values/z"].set(vz);
}


// LinMap forward: delegate to scalar/vector overloads for each sub-element
inline void CatalystAdaptor::AddSteerableChannel( const ippl::LinMap& lm, const std::string& steerable_suffix )
{
    ca_m << "::Execute()::AddSteerableChannel(" << steerable_suffix << ");  | Type: LinMap" << endl;
    AddSteerableChannel(lm.x_row, steerable_suffix + "_x_row");
    AddSteerableChannel(lm.y_row, steerable_suffix + "_y_row");
    AddSteerableChannel(lm.z_row, steerable_suffix + "_z_row");
    AddSteerableChannel(lm.time , steerable_suffix + "_time");
}

// LinMaps forward: flatten into dynamic lists and set arrays under steerable_field_f_Map_*
inline void CatalystAdaptor::AddSteerableChannel( const ippl::LinMaps& lms, const std::string& steerable_suffix )
{
    (void)steerable_suffix; // ignored for LinMaps; XML expects fixed Map_* names
    ca_m << "::Execute()::AddSteerableChannel(LinMaps)" << endl;

    auto steerable_channel = node["catalyst/channels/steerable_channel_0D_mesh"];
    steerable_channel["type"].set("mesh");
    auto steerable_data = steerable_channel["data"];
    steerable_data["coordsets/coords/type"].set_string("explicit");

    const size_t N = lms.time.size();
    // Provide N dummy coordinates and connectivity for an unstructured point mesh with N points
    {
        std::vector<double> xs; xs.reserve(N);
        for (size_t i = 0; i < N; ++i) xs.push_back(static_cast<double>(i));
        steerable_data["coordsets/coords/values/x"].set(xs);
    }

    steerable_data["topologies/sMesh_topo/type"].set("unstructured");
    steerable_data["topologies/sMesh_topo/coordset"].set("coords");
    steerable_data["topologies/sMesh_topo/elements/shape"].set("point");
    {
        std::vector<int32_t> conn; conn.reserve(N);
        for (int32_t i = 0; i < static_cast<int32_t>(N); ++i) conn.push_back(i);
        steerable_data["topologies/sMesh_topo/elements/connectivity"].set(conn);
    }

    // time: N entries as scalar point-data
    {
        auto f = steerable_data["fields/steerable_field_f_Map_time"];
        f["association"].set("vertex");
        f["topology"].set("sMesh_topo");
        f["volume_dependent"].set("false");
        std::vector<double> timeVals; timeVals.reserve(N);
        for (size_t i = 0; i < N; ++i) timeVals.push_back(lms.time[i]);
        f["values"].set(timeVals);
    }

    // x_row, y_row, z_row: 3-component vector arrays; provide values/x,y,z of length N
    auto set_vec3_field = [&](const std::string& base, const std::vector<ippl::Vector<double,3>>& rows){
        auto f = steerable_data[std::string("fields/steerable_field_f_") + base];
        f["association"].set("vertex");
        f["topology"].set("sMesh_topo");
        f["volume_dependent"].set("false");
        std::vector<double> xs; xs.reserve(N);
        std::vector<double> ys; ys.reserve(N);
        std::vector<double> zs; zs.reserve(N);
        for (size_t i = 0; i < N; ++i) {
            const auto& v = rows[i];
            xs.push_back(v[0]); ys.push_back(v[1]); zs.push_back(v[2]);
        }
        f["values/x"].set(xs);
        f["values/y"].set(ys);
        f["values/z"].set(zs);
    };

    set_vec3_field("Map_x_row", lms.x_row);
    set_vec3_field("Map_y_row", lms.y_row);
    set_vec3_field("Map_z_row", lms.z_row);
}

// Add: std::vector<LinMap> forward pass -> convert AoS to SoA and delegate
inline void CatalystAdaptor::AddSteerableChannel( const std::vector<ippl::LinMap>& lm_vec, const std::string& steerable_suffix )
{
    (void)steerable_suffix; // same semantics as LinMaps
    ca_m << "::Execute()::AddSteerableChannel(vector<LinMap>)" << endl;
    ippl::LinMaps tmp;
    const size_t N = lm_vec.size();
    tmp.time.reserve(N);
    tmp.x_row.reserve(N);
    tmp.y_row.reserve(N);
    tmp.z_row.reserve(N);
    for(const auto& m : lm_vec){
        tmp.time.push_back(m.time);
        tmp.x_row.push_back(m.x_row);
        tmp.y_row.push_back(m.y_row);
        tmp.z_row.push_back(m.z_row);
    }
    AddSteerableChannel(tmp, steerable_suffix);
}



// =====================================================================================
// FETCHING FROM CONDUIT NODE:
// =====================================================================================


/* overload for: Scalar, Bool(switch), Button */
template<typename T>
requires (!std::is_enum_v<std::decay_t<T>>)
void CatalystAdaptor::FetchSteerableChannelValue( T& steerable_scalar_backwardpass, const std::string& label) {
    ca_m << "::Execute()::FetchSteerableChannel(" << label  << ") | Type: " << typeid(T).name() << endl;

    std::string unified_path = std::string("catalyst/steerable_channel_backward_all/fields/") +
                               "steerable_field_b_" + label + "/values";

    const std::string* chosen = nullptr;
    if (results.has_path(unified_path)) {
        chosen = &unified_path;
    } else {
        ca_m << "::Execute()::FetchSteerableChannel(" << label << ") | no backward result present; skipping." << endl;
        return;
    }

    conduit_cpp::Node values_node = results[*chosen];
    if (!values_node.dtype().is_number()) {
        ca_m << "::Execute()::FetchSteerableChannel(" << label << ") | backward value not numeric; skipping." << endl;
        return;
    }

    if constexpr (std::is_enum_v<std::decay_t<T>>)      steerable_scalar_backwardpass = static_cast<T>(values_node.to_int32());
    else if constexpr (std::is_same_v<T,double>)        steerable_scalar_backwardpass = values_node.to_double();
    else if constexpr (std::is_same_v<T,float>)         steerable_scalar_backwardpass = static_cast<float>(values_node.to_float());
    else if constexpr (std::is_same_v<T,int>)           steerable_scalar_backwardpass = static_cast<int>(values_node.to_int32());
    else if constexpr (std::is_same_v<T,unsigned>)      steerable_scalar_backwardpass = static_cast<unsigned>(values_node.to_uint32());
    else if constexpr (std::is_same_v<T,bool>)          steerable_scalar_backwardpass = static_cast<bool>(values_node.to_int32());
    else if constexpr (std::is_same_v<T,ippl::Button>)  steerable_scalar_backwardpass = static_cast<bool>(values_node.to_int32());
    else {
        throw IpplException("Stream::InSitu::CatalystAdaptor::FetchSteerableChannelValue(" + label +  ")", "Unsupported type for channel: " + label);
    }

    ca_m << "::Execute()::FetchSteerableChannel(" << label << ") | received:" << steerable_scalar_backwardpass << endl;
}

// Enum overload explicit (clarity)
template<typename E>
requires (std::is_enum_v<std::decay_t<E>>)
void CatalystAdaptor::FetchSteerableChannelValue( E& e, const std::string& label)
{
    ca_m << "::Execute()::FetchSteerableChannel(" << label  << ") | Type: Enum| Value sent:" << /* enumChoices_[label].second  */to_string(e) << endl;
    std::string unified_path = std::string("catalyst/steerable_channel_backward_all/fields/") +
                               "steerable_field_b_" + label + "/values";
    if (!results.has_path(unified_path)) {
        ca_m << "  no backward enum found for label '" << label << "'" << endl;
        return;
    }
    conduit_cpp::Node values_node = results[unified_path];
    if (!values_node.dtype().is_number()) return;
    e = static_cast<E>(values_node.to_int32());

    ca_m << "::Execute()::FetchSteerableChannel(" << label  << ") | Type: Enum|  received:" << to_string(e) << endl;
}


/* overload for: ippl::Vector */
template<typename T, unsigned Dim_v>
void CatalystAdaptor::FetchSteerableChannelValue( ippl::Vector<T, Dim_v>& steerable_vec_backwardpass, const std::string& label)
{
    ca_m << "::Execute()::FetchSteerableChannel(" << label  << ") | Vector<" << typeid(T).name() << "," << Dim_v << ">" << endl;

    // static const char* comp_names[3] = {"x","y","z"};
    const unsigned comps = Dim_v > 3 ? 3u : Dim_v;
    // bool any_set = false;
    // // Preferred: unified vector array with values/x,y,z
    // for (unsigned c = 0; c < comps; ++c) {
    //     std::string unified_vec_comp = std::string("catalyst/steerable_channel_backward_all/fields/") +
    //                                    "steerable_field_b_" + label + "/values/" + comp_names[c];
    //     if (results.has_path(unified_vec_comp)) {
    //         conduit_cpp::Node vnode = results[unified_vec_comp];
    //         if (vnode.dtype().is_number()) {
    //             steerable_vec_backwardpass[c] = static_cast<T>(vnode.to_double());
    //             any_set = true;
    //             ca_m << "  read component '" << comp_names[c] << "' from " << unified_vec_comp
    //                  << ": " << vnode.to_double() << endl;
    //         }
    //     }
    // }

    // Fallback: a flat numeric array at .../values holding at least `comps` elements
    // if (!any_set) {
    if (true) {
        std::string unified_vec_values = std::string("catalyst/steerable_channel_backward_all/fields/") +
                                         "steerable_field_b_" + label + "/values";
        const std::string* chosen = nullptr;
        if (results.has_path(unified_vec_values)) chosen = &unified_vec_values;
        if (chosen) {
            conduit_cpp::Node vnode = results[*chosen];
                bool idx_read = true;
                for (unsigned c = 0; c < comps; ++c) {
                    std::string idx_path = *chosen + "/" + std::to_string(c);
                    if (results.has_path(idx_path)) {
                        conduit_cpp::Node ic = results[idx_path];
                        if (ic.dtype().is_number()) {
                            steerable_vec_backwardpass[c] = static_cast<T>(ic.to_double());
                        } else {
                            idx_read = false;
                        }
                    } else {
                        idx_read = false;
                    }
                }
                if (idx_read) {
                    // ca_m << "  read list-like values from " << *chosen << ":" << steerable_vec_backwardpass << endl;
                    ca_m << "::Execute()::FetchSteerableChannel(" << label  << ") | Vector<" << typeid(T).name() << "," << Dim_v << "> | received: " << steerable_vec_backwardpass << endl;
                
                }
            // }
        }
        else {
            ca_warn << "  no backward vector found for label '" << label << "' under expected paths." << endl;

        }
    }
}


// LinMap fetch: fetch sub-elements back under suffixed labels
inline void CatalystAdaptor::FetchSteerableChannelValue( ippl::LinMap& lm, const std::string& label)
{
    FetchSteerableChannelValue(lm.x_row, label + "_x_row");
    FetchSteerableChannelValue(lm.y_row, label + "_y_row");
    FetchSteerableChannelValue(lm.z_row, label + "_z_row");
    FetchSteerableChannelValue(lm.time , label + "_time");
}

















// LinMaps fetch: read back arrays from results and populate lms; resize to match incoming lists
inline void CatalystAdaptor::FetchSteerableChannelValue( ippl::LinMaps& lms, const std::string& label)
{
    (void)label; // ignored for LinMaps
    ca_m << "::Execute()::FetchSteerableChannel(LinMaps)| count=" << lms.time.size()  << endl;

    auto time_ =  results["catalyst/steerable_channel_backward_all/fields/steerable_field_b_Map_time/values"];
    size_t N_old = lms.time.size();
    size_t N_new = time_.dtype().number_of_elements();
    size_t N = std::max(N_old,N_new);


    auto fetch_scalar_array = [&](const std::string& name, std::vector<double>& out) -> bool {
        std::string path = std::string("catalyst/steerable_channel_backward_all/fields/") +
                           "steerable_field_b_" + name + "/values";
        if (!results.has_path(path)) return false;
        // values is a list of doubles: [t0, t1, ...] or an object with numeric keys


        conduit_cpp::Node vals = results[path];
        size_t n = 0;
        n = vals.dtype().number_of_elements();
        if (n == 0) return false;
        out.resize(n);
        for (size_t i = 0; i < n; ++i) out[i] =  vals.as_double_ptr()[i];
        return true;
    };



    auto fetch_vec3_array = [&](const std::string& where,
                                std::vector<double>& _x0,
                                std::vector<double>& _y1,
                                std::vector<double>& _z2) -> bool {
        
        // Case A: named components x/y/z
        bool has_indexed = results.has_path(where + "/0") && results.has_path(where + "/1") && results.has_path(where + "/2");
        // bool has_xyz = results.has_path(root + "/x") && results.has_path(root + "/y") && results.has_path(root + "/z");
        std::cout << where << "has indexed:"  << has_indexed << std::endl;
        if (has_indexed) {                                
                conduit_cpp::Node vals = results[where];
                conduit_cpp::Node vals_0 = results[where + "/0"];
                // conduit_cpp::Node vals_1 = results[where + "/1"];
                // conduit_cpp::Node vals_2 = results[where + "/2"];
                for (size_t i = 0; i < N; ++i) {
                    _x0[i] = vals_0.as_double_ptr()[3*i];
                    _y1[i] = vals_0.as_double_ptr()[1+i*3];
                    _z2[i] = vals_0.as_double_ptr()[2+i*3];
                }
            return true;
        }
        return false;
    };

    //     auto fetch_vec3_array = [&](const std::string& where,
    //                             ippl::vector<double,3>& _x0,
    //                             ippl::vector<double,3>& _y1,
    //                             ippl::vector<double,3>& _z2) -> bool {
        
    //     // Case A: named components x/y/z
    //     bool has_indexed = results.has_path(where + "/0") && results.has_path(where + "/1") && results.has_path(where + "/2");
    //     // bool has_xyz = results.has_path(root + "/x") && results.has_path(root + "/y") && results.has_path(root + "/z");
    //     std::cout << where << "has indexed:"  << has_indexed << std::endl;
    //     if (has_indexed) {                                
    //             conduit_cpp::Node vals = results[where];
    //             conduit_cpp::Node vals_0 = results[where + "/0"];
    //             // conduit_cpp::Node vals_1 = results[where + "/1"];
    //             // conduit_cpp::Node vals_2 = results[where + "/2"];
    //             for (size_t i = 0; i < N; ++i) {
    //                 _x0[i] = vals_0.as_double_ptr()[  i];
    //                 _y1[i] = vals_0.as_double_ptr()[3+i];
    //                 _z2[i] = vals_0.as_double_ptr()[6+i];
    //             }
    //         return true;
    //     }
    //     return false;
    // };



    std::vector<double> timeVals;
    std::vector<double> x_x(N), x_y(N), x_z(N);
    std::vector<double> y_x(N), y_y(N), y_z(N);
    std::vector<double> z_x(N), z_y(N), z_z(N);
    
    const std::string where = std::string("catalyst/steerable_channel_backward_all/fields/");
    bool gotTime = fetch_scalar_array("Map_time", timeVals);

    bool gotX = fetch_vec3_array(where + "steerable_field_b_Map_x_row/values", x_x, x_y, x_z);
    bool gotY = fetch_vec3_array(where + "steerable_field_b_Map_y_row/values", y_x, y_y, y_z);
    bool gotZ = fetch_vec3_array(where + "steerable_field_b_Map_z_row/values", z_x, z_y, z_z);


            // If nothing present, keep prior values (pre-GUI case)
            if (!(gotTime || gotX || gotY || gotZ)) {
                return;
            }

            // Determine new list length from available arrays
            // size_t N_new = 0;
            // if (gotTime) N_new = std::max(N_new, timeVals.size());
            // if (gotX)    N_new = std::max(N_new, x_x.size());
            // if (gotY)    N_new = std::max(N_new, y_x.size());
            // if (gotZ)    N_new = std::max(N_new, z_x.size());

            // Replace current content with exactly what the client sent (prevents mixing)
            lms.time.assign(N_new, 0.0);
            lms.x_row.assign(N_new, ippl::Vector<double,3>({0.0,0.0,0.0}));
            lms.y_row.assign(N_new, ippl::Vector<double,3>({0.0,0.0,0.0}));
            lms.z_row.assign(N_new, ippl::Vector<double,3>({0.0,0.0,0.0}));

            for (size_t i = 0; i < N_new; ++i) {
                if (gotTime && i < timeVals.size()) lms.time[i] = timeVals[i];
                if (gotX && i < x_x.size()) lms.x_row[i] = ippl::Vector<double,3>({x_x[i], x_y[i], x_z[i]});
                if (gotY && i < y_x.size()) lms.y_row[i] = ippl::Vector<double,3>({y_x[i], y_y[i], y_z[i]});
                if (gotZ && i < z_x.size()) lms.z_row[i] = ippl::Vector<double,3>({z_x[i], z_y[i], z_z[i]});

                /* alternative: */
                // if (gotX && i < x_x.size()) fetch_vec3_array(where + "steerable_field_b_Map_x_row/values", lms.x_row[0] = ippl::Vector<double,3>({x_x[i], x_y[i], x_z[i]});
                // if (gotY && i < y_x.size()) fetch_vec3_array(lms.y_row[i] = ippl::Vector<double,3>({y_x[i], y_y[i], y_z[i]});
                // if (gotZ && i < z_x.size()) fetch_vec3_array(lms.z_row[i] = ippl::Vector<double,3>({z_x[i], z_y[i], z_z[i]});
            }

    for (size_t i = 0; i < lms.time.size(); ++i) {
        std::cout << i << ":  time" " " << lms.time[i] << std::endl;
        std::cout << "Map:" << std::endl;
        std::cout << lms.x_row[i][0] << " " << lms.x_row[i][1] << " " << lms.x_row[i][2] << std::endl;
        std::cout << lms.y_row[i][0] << " " << lms.y_row[i][1] << " " << lms.y_row[i][2] << std::endl;
        std::cout << lms.z_row[i][0] << " " << lms.z_row[i][1] << " " << lms.z_row[i][2] << std::endl;
    }
    ca_m << "::Execute()::FetchSteerableChannel(LinMaps)| after fetch | count=" << lms.time.size() << endl;

}

// Fetch: std::vector<LinMap> <- LinMaps
inline void CatalystAdaptor::FetchSteerableChannelValue( std::vector<ippl::LinMap>& lm_vec, const std::string& label)
{
    (void)label; // same fixed-channel semantics
    ca_m << "::Execute()::FetchSteerableChannel(vector<LinMap>) | prior_count=" << lm_vec.size() << endl;
    // ippl::LinMaps tmp;
    // // Seed tmp with current size (not strictly needed, fetch will overwrite based on results)
    // tmp.time.reserve(lm_vec.size());
    // tmp.x_row.reserve(lm_vec.size());
    // tmp.y_row.reserve(lm_vec.size());
    // tmp.z_row.reserve(lm_vec.size());
    // // Perform fetch on LinMaps representation
    // FetchSteerableChannelValue(tmp, label);
    // // Convert back to AoS
    // const size_t N = tmp.time.size();
    // lm_vec.clear();
    // lm_vec.reserve(N);
    // for(size_t i=0;i<N;++i){
    //     ippl::LinMap m;
    //     m.time  = tmp.time[i];
    //     m.x_row = tmp.x_row[i];
    //     m.y_row = tmp.y_row[i];
    //     m.z_row = tmp.z_row[i];
    //     lm_vec.push_back(std::move(m));
    // }

    // DOESNT WORK ....





    auto time_ =  results["catalyst/steerable_channel_backward_all/fields/steerable_field_b_Map_time/values"];
    size_t N_old = lm_vec.size();
    size_t N_new = time_.dtype().number_of_elements();
    size_t N = std::max(N_old,N_new);


    auto fetch_scalar_array = [&](const std::string& name, std::vector<double>& out) -> bool {
        std::string path = std::string("catalyst/steerable_channel_backward_all/fields/") +
                           "steerable_field_b_" + name + "/values";
        if (!results.has_path(path)) return false;
        // values is a list of doubles: [t0, t1, ...] or an object with numeric keys


        conduit_cpp::Node vals = results[path];
        size_t n = 0;
        n = vals.dtype().number_of_elements();
        if (n == 0) return false;
        out.resize(n);
        for (size_t i = 0; i < n; ++i) out[i] =  vals.as_double_ptr()[i];
        return true;
    };



    auto fetch_vec3_array = [&](const std::string& where,
                                std::vector<double>& _x0,
                                std::vector<double>& _y1,
                                std::vector<double>& _z2) -> bool {
        
        // Case A: named components x/y/z
        bool has_indexed = results.has_path(where + "/0") && results.has_path(where + "/1") && results.has_path(where + "/2");
        // bool has_xyz = results.has_path(root + "/x") && results.has_path(root + "/y") && results.has_path(root + "/z");
        std::cout << where << "has indexed:"  << has_indexed << std::endl;
        if (has_indexed) {                                
                conduit_cpp::Node vals = results[where];
                conduit_cpp::Node vals_0 = results[where + "/0"];
                // conduit_cpp::Node vals_1 = results[where + "/1"];
                // conduit_cpp::Node vals_2 = results[where + "/2"];
                for (size_t i = 0; i < N; ++i) {
                    _x0[i] = vals_0.as_double_ptr()[3*i];
                    _y1[i] = vals_0.as_double_ptr()[1+i*3];
                    _z2[i] = vals_0.as_double_ptr()[2+i*3];
                }
            return true;
        }
        return false;
    };

    std::vector<double> timeVals;
    std::vector<double> x_x(N), x_y(N), x_z(N);
    std::vector<double> y_x(N), y_y(N), y_z(N);
    std::vector<double> z_x(N), z_y(N), z_z(N);
    
    const std::string where = std::string("catalyst/steerable_channel_backward_all/fields/");
    bool gotTime = fetch_scalar_array("Map_time", timeVals);

    bool gotX = fetch_vec3_array(where + "steerable_field_b_Map_x_row/values", x_x, x_y, x_z);
    bool gotY = fetch_vec3_array(where + "steerable_field_b_Map_y_row/values", y_x, y_y, y_z);
    bool gotZ = fetch_vec3_array(where + "steerable_field_b_Map_z_row/values", z_x, z_y, z_z);


            // If nothing present, keep prior values (pre-GUI case)
            if (!(gotTime || gotX || gotY || gotZ)) {
                return;
            }

            // Replace current content with exactly what the client sent (prevents mixing)
            lm_vec.assign(N_new, ippl::LinMap() );

            for (size_t i = 0; i < N_new; ++i) {
                if (gotTime && i < timeVals.size()) lm_vec[i].time = timeVals[i];
                if (gotX && i < x_x.size()) lm_vec[i].x_row = ippl::Vector<double,3>({x_x[i], x_y[i], x_z[i]});
                if (gotY && i < y_x.size()) lm_vec[i].y_row = ippl::Vector<double,3>({y_x[i], y_y[i], y_z[i]});
                if (gotZ && i < z_x.size()) lm_vec[i].z_row = ippl::Vector<double,3>({z_x[i], z_y[i], z_z[i]});

                /* alternative: */
                // if (gotX && i < x_x.size()) fetch_vec3_array(where + "steerable_field_b_Map_x_row/values", lms.x_row[0] = ippl::Vector<double,3>({x_x[i], x_y[i], x_z[i]});
                // if (gotY && i < y_x.size()) fetch_vec3_array(lms.y_row[i] = ippl::Vector<double,3>({y_x[i], y_y[i], y_z[i]});
                // if (gotZ && i < z_x.size()) fetch_vec3_array(lms.z_row[i] = ippl::Vector<double,3>({z_x[i], z_y[i], z_z[i]});
            }

    // for (size_t i = 0; i < lms.time.size(); ++i) {
    //     std::cout << i << ":  time" " " << lms.time[i] << std::endl;
    //     std::cout << "Map:" << std::endl;
    //     std::cout << lm_vec.x_row[i][0] << " " << lm_vec.x_row[i][1] << " " << lm_vec.x_row[i][2] << std::endl;
    //     std::cout << lm_vec.y_row[i][0] << " " << lm_vec.y_row[i][1] << " " << lm_vec.y_row[i][2] << std::endl;
    //     std::cout << lm_vec.z_row[i][0] << " " << lm_vec.z_row[i][1] << " " << lm_vec.z_row[i][2] << std::endl;
    // }

    
    ca_m << "::Execute()::FetchSteerableChannel(vector<LinMap>) | new_count=" << lm_vec.size() << endl;
}


}//ippl






// // LinMap forward: delegate to scalar/vector overloads for each sub-element
//         // Prefer child iteration (robust for lists/objects)
//         size_t n = vals.number_of_children();
//         if (n > 0) {
//             out.resize(n);
//             for (size_t i = 0; i < n; ++i) {
//                 out[i] = results[path + "/" + std::to_string(i)].to_double();
//             }
//             return true;
//         }
//         // Fallback: flat numeric array
//         n = vals.dtype().number_of_elements();
//         if (n == 0) return false;
//         out.resize(n);
//         for (size_t i = 0; i < n; ++i) {
//             out[i] = results[path + "/" + std::to_string(i)].to_double();
//         }
//         return true;





/* 
    auto fetch_vec3_array = [&](const std::string& where,
                                std::vector<double>& _x0,
                                std::vector<double>& _y1,
                                std::vector<double>& _z2) -> bool {
        
        // Case A: named components x/y/z
        bool has_indexed = results.has_path(where + "/0") && results.has_path(where + "/1") && results.has_path(where + "/2");
        // bool has_xyz = results.has_path(root + "/x") && results.has_path(root + "/y") && results.has_path(root + "/z");
        std::cout << where << "has indexed:"  << has_indexed << std::endl;
        if (has_indexed) {
            // auto read_comp_idx = [&](int compIdx, std::vector<double>& out){
            //     std::string compPath = where + "/" + std::to_string(compIdx);
            //     conduit_cpp::Node vals = results[compPath];
            //     size_t n = vals.dtype().number_of_elements();
            //     out.resize(n);
            //     for (size_t i = 0; i < n; ++i) out[i] = vals.as_double_ptr()[i];
            //     vals.print();
            //     std::cout <<out[0] << out[1] << out[2]
            //     // for (size_t i = 0; i < n; ++i) out[i] = results[compPath + "/" + std::to_string(i)].to_double();
            // };
            // read_comp_idx(0, xs); // component 0 across maps
            // read_comp_idx(1, ys); // component 1 across maps
            // read_comp_idx(2, zs); // component 2 across maps

                // std::string compPath_0 = where + "/0";
                // std::string compPath_0 = where + "/1";
                // std::string compPath_0 = where + "/2";

                                
                conduit_cpp::Node vals = results[where];
                conduit_cpp::Node vals_0 = results[where + "/0"];
                conduit_cpp::Node vals_1 = results[where + "/1"];
                conduit_cpp::Node vals_2 = results[where + "/2"];
          
                
                vals.print_detailed();
                std::cout << where << std::endl;
                vals_0.print();
                vals_1.print();
                vals_2.print(); 


                // conduit_cpp::Node tmp;
                // auto info_node = vals.info();
                // info_node.print();
                // info_node.print_detailed();

                
                // conduit_cpp::Node::compact_to(vals_0. tmp);W
                // auto compact = vals_0.compact_to();
                // auto compact = vals_0.to_compact();
                // compact.print();
                
                
                for (size_t i = 0; i < 6; ++i) {
                   //   std::cout <<  vals_0.as_double_ptr()[i] << std::endl; 
                    //    
                    // std::cout <<  vals_0.as_float64_ptr()[i] << std::endl;
                    // std::cout <<  vals_0.to_double()[i] << std::endl;
                    // std::cout <<  vals_0.as_double()[i] << std::endl;
                    // vals_0.to_double_array(tmp);
                }


                for (size_t i = 0; i < N; ++i) {
                    _x0[i] = vals_0.as_double_ptr()[3*i];
                    _y1[i] = vals_0.as_double_ptr()[1+i*3];
                    _z2[i] = vals_0.as_double_ptr()[2+i*3];
                    std::cout << "printing freshly assigned:"   << _x0[i]
                                                                << _y1[i]
                                                                << _z2[i] << std::endl;

                }




            return true;
        }
        return false;
    };
 */