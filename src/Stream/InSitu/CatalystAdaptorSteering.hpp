#pragma once

#include "Stream/InSitu/CatalystAdaptor.h"
#include "Stream/InSitu/CatalystVisitors.h" // ensure AllowedSteerType_v available
#include <array>
// Added standard headers required by RegisterStructMembers and helpers
#include <tuple>
#include <utility>
#include <functional>
#include <typeinfo>




namespace ippl{

namespace detail {

// Label sanitation: replace '/' to avoid unintended Conduit subtree splitting.
inline std::string sanitize_label(const std::string& in) {
    std::string tmp; tmp.reserve(in.size());
    std::string out; out.reserve(in.size());
    for (char c : in)  tmp += (c=='/' ? '_' : c);
    for (char c : tmp) out += (c=='/' ? '_' : c);
    
        
    return out;
}

// Recursive applicators with const and non-const overloads to avoid const_cast.
// Base cases (no members left).
template <typename Visitor, typename T>
inline void apply_struct_members(Visitor&, T&, const std::string&) {}
template <typename Visitor, typename T>
inline void apply_struct_members(Visitor&, const T&, const std::string&) {}

// Recursive steps: visit (name, pointer-to-member) then recurse.
template <typename Visitor, typename T, typename NameType, typename MemberPtr, typename... Rest>
inline void apply_struct_members(Visitor& vis, T& obj, const std::string& rootLabel,
                                 NameType name, MemberPtr ptr, Rest&&... rest) {
    // Build full label and dispatch to existing visitor overloads.
    std::string fullLabel = rootLabel + "." + sanitize_label(std::string(name));
    vis(fullLabel, obj.*ptr);  // rely on visitor operator() overload selection
    apply_struct_members(vis, obj, rootLabel, std::forward<Rest>(rest)...);
}
template <typename Visitor, typename T, typename NameType, typename MemberPtr, typename... Rest>
inline void apply_struct_members(Visitor& vis, const T& obj, const std::string& rootLabel,
                                 NameType name, MemberPtr ptr, Rest&&... rest) {
    std::string fullLabel = rootLabel + "." + sanitize_label(std::string(name));
    vis(fullLabel, obj.*ptr);
    apply_struct_members(vis, obj, rootLabel, std::forward<Rest>(rest)...);
}

// Meta storage per struct type T.
template <typename T>
struct StructMeta {
    static inline bool registered = false;
    // Dispatch lambda handles scalar/member steering. Takes a variant of visitors.
    static inline std::function<void(CatalystAdaptor::SteerVisitorVariant_t, T&, const std::string&)> dispatch;
    // Array-of-struct aggregation (vector<T>) – built at registration time.
    static inline std::function<void(CatalystAdaptor::SteerVisitorVariant_t, std::vector<T>&, const std::string&)> dispatch_vec;
};

} // namespace detail

// -------------------------------------------------------------------------------------
// Registration function (static method of CatalystAdaptor declared in header)
// -------------------------------------------------------------------------------------
// Args must be an even-sized pack of: name(string-like), pointer-to-member.
// Validates each member type at registration (throws IpplException if invalid).
// Stores three lambdas which expand the pack and delegate to visitor overloads.
template<typename T, typename... Args>
void CatalystAdaptor::RegisterStructMembers(Args&&... args) {
    using DecayT = std::decay_t<T>;
    static_assert(sizeof...(Args) % 2 == 0, "RegisterStructMembers requires (name, memberPtr) pairs");
    if (detail::StructMeta<DecayT>::registered) return;

    // Pack names and member pointers interleaved.
    auto pack = std::tuple<Args...>(std::forward<Args>(args)...);
    constexpr size_t N = sizeof...(Args);
    constexpr size_t PairCount = N / 2;

    // Validate each (name, memberPtr) pair.
    [&]<std::size_t... I>(std::index_sequence<I...>){
        (([] (auto name, auto memberPtr){
            using MemberType = std::decay_t<decltype(std::declval<DecayT&>().*memberPtr)>;
            if constexpr (!AllowedSteerType_v<MemberType>) {
                throw IpplException(
                    "CatalystAdaptor::RegisterStructMembers",
                    std::string("Unsupported member type for steering in struct '") + typeid(DecayT).name() +
                    "' member '" + std::string(name) + "'"
                );
            }
        })(std::get<2*I>(pack), std::get<2*I+1>(pack)), ...);
    }(std::make_index_sequence<PairCount>{});

    // Visitor dispatch reuses generic applicator.
    detail::StructMeta<DecayT>::dispatch = [pack](CatalystAdaptor::SteerVisitorVariant_t var, DecayT& obj, const std::string& root) mutable {
        std::visit([&](auto* active_ptr) {
            if (!active_ptr) return;
            auto& vis = *active_ptr;
            std::apply([&](auto&&... all){ ippl::detail::apply_struct_members(vis, obj, ippl::detail::sanitize_label(root), all...); }, pack);
        }, var);
    };

    // ================= Array-of-Struct (vector<T>) support =================
    detail::StructMeta<DecayT>::dispatch_vec = [pack](CatalystAdaptor::SteerVisitorVariant_t var, std::vector<DecayT>& arr, const std::string& root) mutable {
        if (arr.empty()) return;
        constexpr size_t PC = PairCount;
        std::visit([&](auto* active_ptr) {
            if (!active_ptr) return;
            auto& vis = *active_ptr;
            using VisitorType = std::decay_t<decltype(vis)>;
            
            [&]<std::size_t... I>(std::index_sequence<I...>){
                ( [&](){
                    using MemberPtrT = std::tuple_element_t<2*I+1, decltype(pack)>;
                    MemberPtrT mptr = std::get<2*I+1>(pack);
                    auto rawName = std::get<2*I>(pack);
                    std::string memberLabel = "array:" + ippl::detail::sanitize_label(root) + '.' + ippl::detail::sanitize_label(std::string(rawName));
                    using MType = std::remove_reference_t<decltype(std::declval<DecayT>().*mptr)>;
                    std::vector<MType> tmp; tmp.reserve(arr.size());
                    for (auto& el : arr) tmp.push_back(el.*mptr);
                    
                    vis(memberLabel, tmp); // fetch, init, or fwd
                    
                    if constexpr (std::is_same_v<VisitorType, CatalystAdaptor::SteerFetchVisitor>) {
                        if (tmp.size() != arr.size()) arr.resize(tmp.size()); // default construct new structs
                        // Write back member values
                        for (std::size_t i = 0; i < tmp.size(); ++i) {
                            arr[i].*mptr = tmp[i];
                        }
                    }
                }(), ... );
            }(std::make_index_sequence<PC>{});
        }, var);
    };

    detail::StructMeta<DecayT>::registered = true;
}

// ===================== Typed enum choices registration (definitions) =====================


template<typename E>
requires (std::is_enum_v<std::decay_t<E>>)
void CatalystAdaptor::RegisterEnumChoicesTyped(const std::string& label, const std::vector<std::pair<std::string, E>>& entries) {
    std::vector<std::pair<std::string,int>> conv;
    conv.reserve(entries.size());
    for (const auto& p : entries) {
        conv.emplace_back(p.first, static_cast<int>(p.second));
    }
    RegisterEnumChoices(label, conv);
}

template<typename E>
requires (std::is_enum_v<std::decay_t<E>>)
void CatalystAdaptor::RegisterEnumChoicesTyped(const std::vector<std::pair<std::string, E>>& entries) {
    std::vector<std::pair<std::string,int>> conv;
    conv.reserve(entries.size());
    for (const auto& p : entries) {
        conv.emplace_back(p.first, static_cast<int>(p.second));
    }
    enumChoicesByType_m[std::type_index(typeid(E))] = std::move(conv);
}




// =====================================================================================
// INITIALISATION:
// =====================================================================================

template<typename T>
requires (!std::is_enum_v<std::decay_t<T>>)
void CatalystAdaptor::InitSteerChannel( [[maybe_unused]] const T& steerableScalarForwardpass,  const std::string& label ){
    catalystInfo_m << "::Initialize()::InitSteerChannel(" << label << "):  | Type: " << typeid(T).name() << endl;
    // Only invoke ProxyWriter scalar include for arithmetic types; others are placeholders.
    if constexpr (std::is_arithmetic_v<std::decay_t<T>>) {
        proxyWriter_m.include(steerableScalarForwardpass, label);
    } else {
        catalystWarn_m << "ProxyWriter placeholder: include() for label '" << label
                << "' (type=" << typeid(T).name() << ") not implemented yet (TODO)." << endl;
    }
    conduit_cpp::Node scriptArgs = node_m["catalyst/scripts/script/args"];
    scriptArgs.append().set_string(label);
}

// Enum overload (explicit) to ensure proper dropdown setup even if template above is shadowed
template<typename E>
requires (std::is_enum_v<std::decay_t<E>>)
void CatalystAdaptor::InitSteerChannel( [[maybe_unused]] const E& e, const std::string& label ){
    catalystInfo_m << "::Initialize()::InitSteerChannel(" << label << "):  | Type: Enum" << endl;
    auto it = enumChoices_m.find(label);
    if (it != enumChoices_m.end()) {
        proxyWriter_m.includeEnum(label, it->second, static_cast<int>(e));
    } else {
        // Try type-based enum choices
        auto itt = enumChoicesByType_m.find(std::type_index(typeid(E)));
        if (itt != enumChoicesByType_m.end()) {
            proxyWriter_m.includeEnum(label, itt->second, static_cast<int>(e));
        } else {
            // Fallback to a checkbox if no choices registered
            proxyWriter_m.includeBool(label, false);
        }
    }
    conduit_cpp::Node scriptArgs = node_m["catalyst/scripts/script/args"];
    scriptArgs.append().set_string(label);
}

// Bool-like Switch: init (checkbox in GUI)
void CatalystAdaptor::InitSteerChannel( [[maybe_unused]] const bool& sw, const std::string& label ){
    catalystInfo_m << "::Initialize()::InitSteerChannel(" << label << "):  | Type: Switch" << endl;
    proxyWriter_m.includeBool(label, static_cast<bool>(sw));

    conduit_cpp::Node scriptArgs = node_m["catalyst/scripts/script/args"];
    scriptArgs.append().set_string(label);
}

// Button-like: init (push button in GUI)
void CatalystAdaptor::InitSteerChannel( [[maybe_unused]] const ippl::Button& btn, const std::string& label ){
    catalystInfo_m << "::Initialize()::InitSteerChannel(" << label << "):  | Type: Button" << endl;
    proxyWriter_m.includeButton(label);
    conduit_cpp::Node scriptArgs = node_m["catalyst/scripts/script/args"];
    scriptArgs.append().set_string(label);
}

// Vector steerable overloads
template<typename T, unsigned Dim_v>
void CatalystAdaptor::InitSteerChannel( [[maybe_unused]] const ippl::Vector<T, Dim_v>& steerableVecForwardpass, const std::string& label )
{
    catalystInfo_m << "::Initialize()::InitSteerChannel(" << label << "):  | Vector<" << typeid(T).name() << "," << Dim_v << ">" << endl;
    // Register this label as a vector channel in the proxy writer (limit to 3 comps in GUI)
    proxyWriter_m.includeVector<T, Dim_v>(label);
    (void)steerableVecForwardpass;

    // Ensure the Python pipeline receives this label after `--steer_channel_names`
    // so it creates the corresponding forward reader and unified sender wiring.
    conduit_cpp::Node scriptArgs = node_m["catalyst/scripts/script/args"];
    scriptArgs.append().set_string(label);
}



// =============================================================
// Generic std::vector<T> steerables (Array-of-struct members)
// Supports arithmetic, bool and ippl::Button as element types.
// =============================================================
template<typename Elem>
requires (std::is_arithmetic_v<std::decay_t<Elem>> || std::is_enum_v<std::decay_t<Elem>> || std::is_same_v<std::decay_t<Elem>, bool> || std::is_same_v<std::decay_t<Elem>, ippl::Button>)
void CatalystAdaptor::InitSteerChannel( [[maybe_unused]] const std::vector<Elem>& arr, const std::string& label )
{
    // Normalize: ensure 'array:' prefix for any std::vector steerable labels (user shouldn't add it)
    const std::string alabel = (label.rfind("array:", 0) == 0) ? label : std::string("array:") + label;
    catalystInfo_m << "::Initialize()::InitSteerChannel(" << alabel << "):  | Type: std::vector<elem> size=" << arr.size() << endl;
    // Derive namespace from canonical label of the form "array:<ns>.<member>"
    std::string ns = alabel;
    if (ns.rfind("array:", 0) == 0) ns = ns.substr(6);
    auto dp = ns.find('.');
    if (dp != std::string::npos) ns = ns.substr(0, dp);

    // Register this array label with ProxyWriter so per-namespace array proxies are generated.
    // The incoming label is expected to be of the form "array:<ns>.<member>" (constructed in RegisterStructMembers).
    // Scalar-like element arrays use the generic include() path; ProxyWriter will parse and mark as array.
    if constexpr (std::is_arithmetic_v<std::decay_t<Elem>>) {
        // Use the first element as a default if available; otherwise 0.
        Elem def{};
        if (!arr.empty()) def = arr.front();
        proxyWriter_m.include(def, alabel);
    } else if constexpr (std::is_enum_v<std::decay_t<Elem>>) {
        // Enum arrays: register as enum with choices; default from first element if present
        int def = !arr.empty() ? static_cast<int>(arr.front()) : 0;
        // Prefer label-scoped choices, then type-scoped, else fallback to a checkbox
        auto it = enumChoices_m.find(alabel);
        if (it != enumChoices_m.end()) {
            proxyWriter_m.includeEnum(alabel, it->second, def);
        } else {
            auto itt = enumChoicesByType_m.find(std::type_index(typeid(Elem)));
            if (itt != enumChoicesByType_m.end()) {
                proxyWriter_m.includeEnum(alabel, itt->second, def);
            } else {
                // No choices registered; use a checkbox as a minimal fallback UI
                proxyWriter_m.includeBool(alabel, false);
            }
        }
    } else if constexpr (std::is_same_v<std::decay_t<Elem>, bool>) {
        bool def = !arr.empty() ? static_cast<bool>(arr.front()) : false;
        proxyWriter_m.includeBool(alabel, def);
    } else if constexpr (std::is_same_v<std::decay_t<Elem>, ippl::Button>) {
        proxyWriter_m.includeButton(alabel);
    }

    // Inform proxy writer about desired initial size for this array namespace
    proxyWriter_m.setArrayInitialSize(ns, arr.size());

    // Inform Python pipeline about each label (still needed for backward mapping)
    conduit_cpp::Node scriptArgs = node_m["catalyst/scripts/script/args"];
    scriptArgs.append().set_string(alabel);

}

// Init: std::vector<ippl::Vector<T,Dim>> steerables
template<typename T, unsigned Dim_v>
void CatalystAdaptor::InitSteerChannel( [[maybe_unused]] const std::vector<ippl::Vector<T, Dim_v>>& arr, const std::string& label )
{
    // Normalize: ensure 'array:' prefix for any std::vector steerable labels
    const std::string alabel = (label.rfind("array:", 0) == 0) ? label : std::string("array:") + label;
    catalystInfo_m << "::Initialize()::InitSteerChannel(" << alabel << "):  | Type: std::vector<Vector<" << typeid(T).name() << "," << Dim_v << ">> size=" << arr.size() << endl;
    // Derive namespace from canonical label of the form "array:<ns>.<member>"
    std::string ns = alabel;
    if (ns.rfind("array:", 0) == 0) ns = ns.substr(6);
    auto dp = ns.find('.');
    if (dp != std::string::npos) ns = ns.substr(0, dp);

    // Register the vector array label with ProxyWriter (parsed as array namespace)
    proxyWriter_m.includeVector<T, Dim_v>(alabel);

    // Inform proxy writer about desired initial size for this array namespace
    proxyWriter_m.setArrayInitialSize(ns, arr.size());

    conduit_cpp::Node scriptArgs = node_m["catalyst/scripts/script/args"];
    scriptArgs.append().set_string(alabel);

}



// =====================================================================================
// SETTING UP CONDUIT NODE:
// =====================================================================================


template<typename T>
requires (!std::is_enum_v<std::decay_t<T>>)
void CatalystAdaptor::ForwardSteerChannel( const T& steerableScalarForwardpass,  const std::string& steerableSuffix ) 
{
        catalystInfo_m << "::Execute()::ForwardSteerChannel(" << steerableSuffix << ");  | Type: " << typeid(T).name() << endl;
        
    auto steerableChannel = node_m["catalyst/channels/steerableChannel_0D_mesh"];
        // auto steerableChannel = node_m["catalyst/channels/steerableChannels_forward_all"];

        steerableChannel["type"].set("mesh");
        auto steerableData = steerableChannel["data"];
        steerableData["coordsets/coords/type"].set_string("explicit");
        steerableData["coordsets/coords/values/x"].set( 0 );

        steerableData["topologies/sMesh_topo/type"].set("unstructured");
        steerableData["topologies/sMesh_topo/coordset"].set("coords");
        steerableData["topologies/sMesh_topo/elements/shape"].set("point");
        steerableData["topologies/sMesh_topo/elements/connectivity"].set( 0 );
        
        /* 3D double is not mandatory ?? and irrelevant we want to pass minimal vtk object with 1 data point */
        // steerableData["coordsets/coords/values/x"].set_float64_vector({ 0 });
        // steerableData["coordsets/coords/values/y"].set_float64_vector({ 0 });
        // steerableData["coordsets/coords/values/z"].set_float64_vector({ 0 });

        // steerableData["topologies/sMesh_topo/elements/connectivity"].set_int32_vector({ 0 });


        conduit_cpp::Node steerableField = steerableData["fields/steerableField_f_" + steerableSuffix];
        steerableField["association"].set("vertex");
        steerableField["topology"].set("sMesh_topo");
        steerableField["volume_dependent"].set("false");

        conduit_cpp::Node values = steerableField["values"];

        // std::is_scalar_v<std::decay_t<T>>
        if constexpr(std::is_enum_v<std::decay_t<T>>){
            values.set(static_cast<int>(steerableScalarForwardpass));
        }
        else if constexpr(std::is_scalar_v<T>){
            values.set(steerableScalarForwardpass);
            // lastForwardSteerVal[steerableSuffix] = static_cast<double>(steerableScalarForwardpass);
        }        
        else {
            throw IpplException("Stream::InSitu::CatalystAdaptor::ForwardSteerChannel", "Unsupported steerable type for channel: " + steerableSuffix);
        }
}

// Enum overload explicit (clarity)
template<typename E>
requires (std::is_enum_v<std::decay_t<E>>)
void CatalystAdaptor::ForwardSteerChannel( const E& e, const std::string& steerableSuffix )
{
    catalystInfo_m << "::Execute()::ForwardSteerChannel(" << steerableSuffix << ");  | Type: Enum" << endl;
    auto steerableChannel = node_m["catalyst/channels/steerableChannel_0D_mesh"];
    steerableChannel["type"].set("mesh");
    auto steerableData = steerableChannel["data"];    
    steerableData["coordsets/coords/type"].set_string("explicit");
    steerableData["coordsets/coords/values/x"].set( 0 );
    steerableData["topologies/sMesh_topo/type"].set("unstructured");
    steerableData["topologies/sMesh_topo/coordset"].set("coords");
    steerableData["topologies/sMesh_topo/elements/shape"].set("point");
    steerableData["topologies/sMesh_topo/elements/connectivity"].set( 0 );
    auto steerableField = steerableData["fields/steerableField_f_" + steerableSuffix];
    steerableField["association"].set("vertex");
    steerableField["topology"].set("sMesh_topo");
    steerableField["volume_dependent"].set("false");
    steerableField["values"].set(static_cast<int>(e));
}

// Bool-like Switch: forward as single-sample scalar (0/1)
void CatalystAdaptor::ForwardSteerChannel( const bool& sw, const std::string& steerableSuffix )
{
    catalystInfo_m << "::Execute()::ForwardSteerChannel(" << steerableSuffix << ");  | Type: bool/Switch" << endl;
    
    auto steerableChannel = node_m["catalyst/channels/steerableChannel_0D_mesh"];
    steerableChannel["type"].set("mesh");
    auto steerableData = steerableChannel["data"];
    steerableData["coordsets/coords/type"].set_string("explicit");
    steerableData["coordsets/coords/values/x"].set( 0 );

    steerableData["topologies/sMesh_topo/type"].set("unstructured");
    steerableData["topologies/sMesh_topo/coordset"].set("coords");
    steerableData["topologies/sMesh_topo/elements/shape"].set("point");
    steerableData["topologies/sMesh_topo/elements/connectivity"].set( 0 );

    conduit_cpp::Node steerableField = steerableData["fields/steerableField_f_" + steerableSuffix];
    steerableField["association"].set("vertex");
    steerableField["topology"].set("sMesh_topo");
    steerableField["volume_dependent"].set("false");
    
    int boolAsInt = sw ? 1 : 0;
    steerableField["values"].set(boolAsInt);
}

// Button-like: forward as single-sample scalar (0/1)
void CatalystAdaptor::ForwardSteerChannel( const ippl::Button& btn, const std::string& steerableSuffix )
{
    catalystInfo_m << "::Execute()::ForwardSteerChannel(" << steerableSuffix << ");  | Type: Button" << endl;
    auto steerableChannel = node_m["catalyst/channels/steerableChannel_0D_mesh"];
    steerableChannel["type"].set("mesh");
    auto steerableData = steerableChannel["data"];
    steerableData["coordsets/coords/type"].set_string("explicit");
    steerableData["coordsets/coords/values/x"].set( 0 );

    steerableData["topologies/sMesh_topo/type"].set("unstructured");
    steerableData["topologies/sMesh_topo/coordset"].set("coords");
    steerableData["topologies/sMesh_topo/elements/shape"].set("point");
    steerableData["topologies/sMesh_topo/elements/connectivity"].set( 0 );

    conduit_cpp::Node steerableField = steerableData["fields/steerableField_f_" + steerableSuffix];
    steerableField["association"].set("vertex");
    steerableField["topology"].set("sMesh_topo");
    steerableField["volume_dependent"].set("false");
    steerableField["values"].set(static_cast<int>(btn ? 1 : 0));
}

// ippl::Vector overload
template<typename T, unsigned Dim_v>
void CatalystAdaptor::ForwardSteerChannel( const ippl::Vector<T, Dim_v>& steerableVecForwardpass, const std::string& steerableSuffix )
{
    catalystInfo_m << "::Execute()::ForwardSteerChannel(" << steerableSuffix << ");  | Vector<" << typeid(T).name() << "," << Dim_v << ">" << endl;

    auto steerableChannel = node_m["catalyst/channels/steerableChannel_0D_mesh"];
    steerableChannel["type"].set("mesh");
    auto steerableData = steerableChannel["data"];
    steerableData["coordsets/coords/type"].set_string("explicit");
    steerableData["coordsets/coords/values/x"].set(0);

    steerableData["topologies/sMesh_topo/type"].set("unstructured");
    steerableData["topologies/sMesh_topo/coordset"].set("coords");
    steerableData["topologies/sMesh_topo/elements/shape"].set("point");
    steerableData["topologies/sMesh_topo/elements/connectivity"].set(0);

    // Single vector field with 3 components (x,y,z) under one name
    const std::string base = std::string("fields/steerableField_f_") + steerableSuffix;
    // Ensure gotX/Y/Z are defined before use, fix the summary logging, and print time per-map.
    auto fnode = steerableData[base];
    fnode["association"].set_string("vertex");
    fnode["topology"].set_string("sMesh_topo");
    fnode["volume_dependent"].set_string("false");

    if constexpr (std::is_integral_v<T>) {
        const int vx = static_cast<int>(steerableVecForwardpass[0]);
        fnode["values/x"].set(vx);
        if constexpr (Dim_v >= 2) {
            const int vy = static_cast<int>(steerableVecForwardpass[1]);
            fnode["values/y"].set(vy);
        }
        if constexpr (Dim_v >= 3) {
            const int vz = static_cast<int>(steerableVecForwardpass[2]);
            fnode["values/z"].set(vz);
        }
    } else {
        const double vx = static_cast<double>(steerableVecForwardpass[0]);
        fnode["values/x"].set(vx);
        if constexpr (Dim_v >= 2) {
            const double vy = static_cast<double>(steerableVecForwardpass[1]);
            fnode["values/y"].set(vy);
        }
        if constexpr (Dim_v >= 3) {
            const double vz = static_cast<double>(steerableVecForwardpass[2]);
            fnode["values/z"].set(vz);
        }
    }
}


// std::vector<T> forward: publish as 1D mesh array under fields/steerableField_f_<label>/values
template<typename Elem>
requires (std::is_arithmetic_v<std::decay_t<Elem>> || std::is_enum_v<std::decay_t<Elem>> || std::is_same_v<std::decay_t<Elem>, bool> || std::is_same_v<std::decay_t<Elem>, ippl::Button>)
void CatalystAdaptor::ForwardSteerChannel( const std::vector<Elem>& arr, const std::string& label )
{
    // Normalize: ensure 'array:' prefix so downstream pipeline/proxies match
    const std::string alabel = (label.rfind("array:", 0) == 0) ? label : std::string("array:") + label;
    catalystInfo_m << "::Execute()::ForwardSteerChannel(vector<elem>) " << alabel << " | N=" << arr.size() << endl;
    // Group by dynamic array prefix: everything before first underscore.
    std::string prefix = alabel;
    auto us_pos = prefix.find('.');
    if(us_pos != std::string::npos) prefix = prefix.substr(0, us_pos);
    std::string mesh_name = std::string("steerableChannel_1D_mesh_") + prefix;
    auto steerableChannel = node_m[std::string("catalyst/channels/") + mesh_name];
    steerableChannel["type"].set("mesh");
    auto steerableData = steerableChannel["data"];
    steerableData["coordsets/coords/type"].set_string("explicit");
    const size_t N = arr.size();
    {
        std::vector<double> xs; xs.reserve(N);
        for (size_t i = 0; i < N; ++i) xs.push_back(static_cast<double>(i));
        steerableData["coordsets/coords/values/x"].set(xs);
    }
    steerableData["topologies/sMesh_topo/type"].set("unstructured");
    steerableData["topologies/sMesh_topo/coordset"].set("coords");
    steerableData["topologies/sMesh_topo/elements/shape"].set("point");
    {
        std::vector<int32_t> conn; conn.reserve(N);
        for (int32_t i = 0; i < static_cast<int32_t>(N); ++i) conn.push_back(i);
        steerableData["topologies/sMesh_topo/elements/connectivity"].set(conn);
    }

    auto f = steerableData[std::string("fields/steerableField_f_") + alabel];
    f["association"].set("vertex");
    f["topology"].set("sMesh_topo");
    f["volume_dependent"].set("false");
    // store values as ints/doubles as appropriate
    if constexpr (std::is_enum_v<std::decay_t<Elem>>) {
        std::vector<int32_t> vals; vals.reserve(N);
        for (const auto& e : arr) vals.push_back(static_cast<int32_t>(e));
        f["values"].set(vals);
    } else {
        std::vector<double> vals; vals.reserve(N);
        for (const auto& e : arr) {
            if constexpr (std::is_same_v<std::decay_t<Elem>, bool>)             vals.push_back(e ? 1.0 : 0.0);
            else if constexpr (std::is_same_v<std::decay_t<Elem>, ippl::Button>) vals.push_back(static_cast<int>(e ? 1 : 0));
            else                                                                vals.push_back(static_cast<double>(e));
        }
        f["values"].set(vals);
    }
}

// std::vector<ippl::Vector<T,Dim>> forward: publish as 1D mesh with 3-component field arrays
template<typename T, unsigned Dim_v>
void CatalystAdaptor::ForwardSteerChannel( const std::vector<ippl::Vector<T, Dim_v>>& arr, const std::string& label )
{
    // Normalize: ensure 'array:' prefix so downstream pipeline/proxies match
    const std::string alabel = (label.rfind("array:", 0) == 0) ? label : std::string("array:") + label;
    catalystInfo_m << "::Execute()::ForwardSteerChannel(vector<Vector<" << typeid(T).name() << "," << Dim_v << ">>) " << alabel << " | N=" << arr.size() << endl;
    std::string prefix = alabel;
    auto us_pos = prefix.find('.');
    if(us_pos != std::string::npos) prefix = prefix.substr(0, us_pos);
    std::string mesh_name = std::string("steerableChannel_1D_mesh_") + prefix;
    auto steerableChannel = node_m[std::string("catalyst/channels/") + mesh_name];
    steerableChannel["type"].set("mesh");
    auto steerableData = steerableChannel["data"];
    steerableData["coordsets/coords/type"].set_string("explicit");
    const size_t N = arr.size();
    {
        std::vector<double> xs; xs.reserve(N);
        for (size_t i = 0; i < N; ++i) xs.push_back(static_cast<double>(i));
        steerableData["coordsets/coords/values/x"].set(xs);
    }
    steerableData["topologies/sMesh_topo/type"].set("unstructured");
    steerableData["topologies/sMesh_topo/coordset"].set("coords");
    steerableData["topologies/sMesh_topo/elements/shape"].set("point");
    {
        std::vector<int32_t> conn; conn.reserve(N);
        for (int32_t i = 0; i < static_cast<int32_t>(N); ++i) conn.push_back(i);
        steerableData["topologies/sMesh_topo/elements/connectivity"].set(conn);
    }

    auto f = steerableData[std::string("fields/steerableField_f_") + alabel];
    f["association"].set("vertex");
    f["topology"].set("sMesh_topo");
    f["volume_dependent"].set("false");
    if constexpr (std::is_integral_v<T>) {
        std::vector<int32_t> vx; vx.reserve(N);
        for (const auto& v : arr) vx.push_back(static_cast<int32_t>(v[0]));
        f["values/x"].set(vx);
        if constexpr (Dim_v >= 2) {
            std::vector<int32_t> vy; vy.reserve(N);
            for (const auto& v : arr) vy.push_back(static_cast<int32_t>(v[1]));
            f["values/y"].set(vy);
        }
        if constexpr (Dim_v >= 3) {
            std::vector<int32_t> vz; vz.reserve(N);
            for (const auto& v : arr) vz.push_back(static_cast<int32_t>(v[2]));
            f["values/z"].set(vz);
        }
    } else {
        std::vector<double> vx; vx.reserve(N);
        for (const auto& v : arr) vx.push_back(static_cast<double>(v[0]));
        f["values/x"].set(vx);
        if constexpr (Dim_v >= 2) {
            std::vector<double> vy; vy.reserve(N);
            for (const auto& v : arr) vy.push_back(static_cast<double>(v[1]));
            f["values/y"].set(vy);
        }
        if constexpr (Dim_v >= 3) {
            std::vector<double> vz; vz.reserve(N);
            for (const auto& v : arr) vz.push_back(static_cast<double>(v[2]));
            f["values/z"].set(vz);
        }
    }
}



// =====================================================================================
// FETCHING FROM CONDUIT NODE:
// =====================================================================================


/* overload for: Scalar, Bool(switch), Button */
template<typename T>
requires (!std::is_enum_v<std::decay_t<T>>)
void CatalystAdaptor::FetchSteerChannel( T& steerableScalarBackwardpass, const std::string& label) {
    // catalystInfo_m << "::Execute()::FetchSteerChannel(" << label  << ") | Type: " << typeid(T).name() << endl;

    std::string unified_path = std::string("catalyst/steerableChannel_backward_all/fields/") +
                               "steerableField_b_" + label + "/values";

    const std::string* chosen = nullptr;
    if (results_m.has_path(unified_path)) {
        chosen = &unified_path;
    } else {
        catalystInfo_m << "::Execute()::FetchSteerChannel(" << label << ") | no backward result present; skipping." << endl;
        return;
    }

    conduit_cpp::Node values_node = results_m[*chosen];
    if (!values_node.dtype().is_number()) {
        catalystInfo_m << "::Execute()::FetchSteerChannel(" << label << ") | backward value not numeric; skipping." << endl;
        return;
    }

    if constexpr (std::is_enum_v<std::decay_t<T>>)      steerableScalarBackwardpass = static_cast<T>(values_node.to_int32());
    else if constexpr (std::is_same_v<T,double>)        steerableScalarBackwardpass = values_node.to_double();
    else if constexpr (std::is_same_v<T,float>)         steerableScalarBackwardpass = static_cast<float>(values_node.to_float());
    else if constexpr (std::is_same_v<T,int>)           steerableScalarBackwardpass = static_cast<int>(values_node.to_int32());
    else if constexpr (std::is_same_v<T,unsigned>)      steerableScalarBackwardpass = static_cast<unsigned>(values_node.to_uint32());
    else if constexpr (std::is_same_v<T,bool>)          steerableScalarBackwardpass = static_cast<bool>(values_node.to_int32());
    else if constexpr (std::is_same_v<T,ippl::Button>)  steerableScalarBackwardpass = static_cast<bool>(values_node.to_int32());
    else {
        throw IpplException("Stream::InSitu::CatalystAdaptor::FetchSteerChannel(" + label +  ")", "Unsupported type for channel: " + label);
    }

    // Safe logging: avoid operator<< on non-streamable types like std::vector<Struct>
    if constexpr (is_std_vector_any<std::decay_t<T>>::value) {
        catalystInfo_m << "::Execute()::FetchSteerChannel(" << label << ") | Type: " << typeid(T).name() << " | received vector | size="
             << steerableScalarBackwardpass.size() << endl;
    } else {
        catalystInfo_m << "::Execute()::FetchSteerChannel(" << label << ") | Type: " << typeid(T).name() << " | received: " << steerableScalarBackwardpass << endl;
    }
}

// Enum overload explicit (clarity)
template<typename E>
requires (std::is_enum_v<std::decay_t<E>>)
void CatalystAdaptor::FetchSteerChannel( E& e, const std::string& label)
{
    // catalystInfo_m << "::Execute()::FetchSteerChannel(" << label  << ") | Type: Enum| Value sent:" << /* enumChoices_m[label].second  */to_string(e) << endl;
    std::string unified_path = std::string("catalyst/steerableChannel_backward_all/fields/") +
                               "steerableField_b_" + label + "/values";
    if (!results_m.has_path(unified_path)) {
        catalystInfo_m << "  no backward enum found for label '" << label << "'" << endl;
        return;
    }
    conduit_cpp::Node values_node = results_m[unified_path];
    if (!values_node.dtype().is_number()) return;
    e = static_cast<E>(values_node.to_int32());

    catalystInfo_m << "::Execute()::FetchSteerChannel(" << label  << ") | Type: Enum | received: " << to_string(e) << endl;
}


/* overload for: ippl::Vector */
template<typename T, unsigned Dim_v>
void CatalystAdaptor::FetchSteerChannel( ippl::Vector<T, Dim_v>& steerableVecBackwardpass, const std::string& label)
{
    const unsigned comps = Dim_v > 3 ? 3u : Dim_v;
    
    std::string unified_vec_values = std::string("catalyst/steerableChannel_backward_all/fields/") +
                                     "steerableField_b_" + label + "/values";
    const std::string* chosen = nullptr;
    if (results_m.has_path(unified_vec_values)) chosen = &unified_vec_values;
    
    if (chosen) {
        conduit_cpp::Node vnode = results_m[*chosen];
        
        // Handle 1D vector appearing as scalar
        if (comps == 1 && vnode.dtype().is_number()) {
             steerableVecBackwardpass[0] = static_cast<T>(vnode.to_double());
             catalystInfo_m << "::Execute()::FetchSteerChannel(" << label  << ") | Type: Vector<" << typeid(T).name() << "," << Dim_v << "> | received: " << steerableVecBackwardpass << endl;
             return;
        }

        bool idx_read = true;
        for (unsigned c = 0; c < comps; ++c) {
            std::string idx_path = *chosen + "/" + std::to_string(c);
            if (results_m.has_path(idx_path)) {
                conduit_cpp::Node ic = results_m[idx_path];
                if (ic.dtype().is_number()) {
                    steerableVecBackwardpass[c] = static_cast<T>(ic.to_double());
                } else {
                    idx_read = false;
                }
            } else {
                idx_read = false;
            }
        }
        if (idx_read) {
            catalystInfo_m << "::Execute()::FetchSteerChannel(" << label  << ") | Type: Vector<" << typeid(T).name() << "," << Dim_v << "> | received: " << steerableVecBackwardpass << endl;
        } else {
             catalystWarn_m << "  backward vector '" << label << "' missing components." << endl;
        }
    }
    else {
        catalystWarn_m << "  no backward vector found for label '" << label << "' under expected paths." << endl;
    }
}


// Fetch std::vector<T> from unified backward channel; resize destination to match
template<typename Elem>
requires (std::is_arithmetic_v<std::decay_t<Elem>> || std::is_enum_v<std::decay_t<Elem>> || std::is_same_v<std::decay_t<Elem>, bool> || std::is_same_v<std::decay_t<Elem>, ippl::Button>)
void CatalystAdaptor::FetchSteerChannel( std::vector<Elem>& out, const std::string& label)
{
    // Normalize: ensure 'array:' prefix matches fields created in proxies/pipeline
    const std::string alabel = (label.rfind("array:", 0) == 0) ? label : std::string("array:") + label;
    // catalystInfo_m << "::Execute()::FetchSteerChannel(vector<elem>) " << alabel << endl;
    const std::string path = std::string("catalyst/steerableChannel_backward_all/fields/") +
                             "steerableField_b_" + alabel + "/values";
    if (!results_m.has_path(path)) {
        catalystInfo_m << "  no backward array for '" << alabel << "'" << endl;
        return;
    }
    conduit_cpp::Node vals = results_m[path];
    // Support integer and floating arrays; fall back to child iteration if needed.
    conduit_cpp::DataType dt = vals.dtype();
    size_t n = dt.number_of_elements();
    if (n == 0) {
        // Some conduit arrays may appear as an object with numeric child nodes instead of a flat dtype
        size_t nc = vals.number_of_children();
        if (nc == 0) return;
        n = nc;
    }
    out.resize(n);

    auto assign_from_double = [&](size_t i, double d){
        if constexpr (std::is_same_v<std::decay_t<Elem>, bool>)              out[i] = (d != 0.0);
        else if constexpr (std::is_same_v<std::decay_t<Elem>, ippl::Button>) out[i] = (static_cast<int>(d) != 0);
        else if constexpr (std::is_enum_v<std::decay_t<Elem>>)               out[i] = static_cast<Elem>(static_cast<int>(d));
        else                                                                  out[i] = static_cast<Elem>(d);
    };

    // Prefer child iteration when available (safe for lists and objects)
    if (vals.number_of_children() > 0) {
        for (size_t i = 0; i < n; ++i) {
            assign_from_double(i, vals.child(i).to_double());
        }
        // return; // Don't return yet, we want to log
    } else {

        // Otherwise, try direct pointer access but guard with try/catch and a final safe fallback
        bool success = false;
        try {
            if (dt.is_double()) {
                const double* ptr = vals.as_double_ptr();
                for (size_t i = 0; i < n; ++i) assign_from_double(i, ptr[i]);
                success = true;
            }
            else if (dt.is_int32()) {
                const int32_t* ptr = vals.as_int32_ptr();
                for (size_t i = 0; i < n; ++i) assign_from_double(i, static_cast<double>(ptr[i]));
                success = true;
            }
            else if (dt.is_uint32()) {
                const uint32_t* ptr = vals.as_uint32_ptr();
                for (size_t i = 0; i < n; ++i) assign_from_double(i, static_cast<double>(ptr[i]));
                success = true;
            }
        } catch (const std::exception &e) {
            catalystInfo_m << "  [DEBUG] direct pointer read failed for '" << alabel << "': " << e.what() << endl;
            success = false;
        } catch (...) {
            catalystInfo_m << "  [DEBUG] direct pointer read failed for '" << alabel << "' (unknown error)" << endl;
            success = false;
        }

        if (!success) {
            // Final fallback: read each element by indexed child path (robust but slightly slower)
            for (size_t i = 0; i < n; ++i) {
                std::string child_path = path + "/" + std::to_string(i);
                if (results_m.has_path(child_path)) {
                    assign_from_double(i, results_m[child_path].to_double());
                } else {
                    // If even this fails, leave default value and warn
                    catalystInfo_m << "  [WARN] Could not read element " << i << " of '" << alabel << "' via fallback" << endl;
                }
            }
        }
    }

    // Log the received array
    catalystInfo_m << "::Execute()::FetchSteerChannel(" << alabel << ") | Type: vector<elem> | received: [";
    for (size_t i = 0; i < out.size(); ++i) {
        if (i > 0) catalystInfo_m << ", ";
        if constexpr (std::is_enum_v<std::decay_t<Elem>>) catalystInfo_m << to_string(out[i]);
        else catalystInfo_m << out[i];
    }
    catalystInfo_m << "]" << endl;
}

// Fetch std::vector<ippl::Vector<T,Dim>>
template<typename T, unsigned Dim_v>
void CatalystAdaptor::FetchSteerChannel( std::vector<ippl::Vector<T, Dim_v>>& out, const std::string& label)
{
    // Normalize: ensure 'array:' prefix matches fields created in proxies/pipeline
    const std::string alabel = (label.rfind("array:", 0) == 0) ? label : std::string("array:") + label;
    // catalystInfo_m << "::Execute()::FetchSteerChannel(vector<Vector<" << typeid(T).name() << "," << Dim_v << ">>) " << alabel << endl;
    const std::string root = std::string("catalyst/steerableChannel_backward_all/fields/") +
                             "steerableField_b_" + alabel + "/values";
    
    // Handle 1D vector array appearing as flat array
    if constexpr (Dim_v == 1) {
        if (results_m.has_path(root) && !results_m.has_path(root + "/0")) {
            conduit_cpp::Node vals = results_m[root];
            size_t n = vals.dtype().number_of_elements();
            if (n == 0 && vals.number_of_children() > 0) n = vals.number_of_children();
            
            out.resize(n);
            
            auto get_val = [&](size_t i) -> double {
                if (vals.number_of_children() > 0) return vals.child(i).to_double();
                conduit_cpp::DataType dt = vals.dtype();
                if (dt.is_double()) return vals.as_double_ptr()[i];
                if (dt.is_int32())  return static_cast<double>(vals.as_int32_ptr()[i]);
                if (dt.is_uint32()) return static_cast<double>(vals.as_uint32_ptr()[i]);
                try { return vals.child(i).to_double(); } catch (...) { return 0.0; }
            };

            for(size_t i=0; i<n; ++i) {
                out[i][0] = static_cast<T>(get_val(i));
            }
            
            catalystInfo_m << "::Execute()::FetchSteerChannel(" << alabel << ") | Type: vector<Vector<" << typeid(T).name() << "," << Dim_v << ">> | received: [";
            for (size_t i = 0; i < out.size(); ++i) {
                if (i > 0) catalystInfo_m << ", ";
                catalystInfo_m << out[i];
            }
            catalystInfo_m << "]" << endl;
            return;
        }
    }

    // Prefer named component arrays x/y/z
    bool has_xyz = results_m.has_path(root + "/0");
    if constexpr (Dim_v >= 2) has_xyz = has_xyz && results_m.has_path(root + "/1");
    if constexpr (Dim_v >= 3) has_xyz = has_xyz && results_m.has_path(root + "/2");

    if (!has_xyz) {
        catalystInfo_m << "  no backward vector array for '" << alabel << "' (expected components 0" 
             << (Dim_v >= 2 ? "/1" : "") << (Dim_v >= 3 ? "/2" : "") << ")" << endl;
        return;
    }
    conduit_cpp::Node xn = results_m[root + "/0"]; 
    conduit_cpp::Node yn;
    if constexpr (Dim_v >= 2) yn = results_m[root + "/1"];
    conduit_cpp::Node zn;
    if constexpr (Dim_v >= 3) zn = results_m[root + "/2"];

    const size_t nx = xn.dtype().number_of_elements();
    size_t N = nx;
    if constexpr (Dim_v >= 2) N = std::min(N, (size_t)yn.dtype().number_of_elements());
    if constexpr (Dim_v >= 3) N = std::min(N, (size_t)zn.dtype().number_of_elements());

    out.resize(N);
    auto get_elem = [](conduit_cpp::Node& n, size_t i) -> double {
        if (n.number_of_children() > 0) return n.child(i).to_double();
        conduit_cpp::DataType dt = n.dtype();
        if (dt.is_double()) return n.as_double_ptr()[i];
        if (dt.is_int32())  return static_cast<double>(n.as_int32_ptr()[i]);
        if (dt.is_uint32()) return static_cast<double>(n.as_uint32_ptr()[i]);
        // Slow fallback: build child path and read
        try { return n.child(i).to_double(); } catch (...) { return 0.0; }
    };
    for (size_t i = 0; i < N; ++i) {
        double vx = get_elem(xn, i);
        double vy = 0.0;
        if constexpr (Dim_v >= 2) vy = get_elem(yn, i);
        double vz = 0.0;
        if constexpr (Dim_v >= 3) vz = get_elem(zn, i);
        
        ippl::Vector<T, Dim_v> v{};
        v[0] = static_cast<T>(vx);
        if constexpr (Dim_v >= 2) v[1] = static_cast<T>(vy);
        if constexpr (Dim_v >= 3) v[2] = static_cast<T>(vz);
        out[i] = v;
    }

    catalystInfo_m << "::Execute()::FetchSteerChannel(" << alabel << ") | Type: vector<Vector<" << typeid(T).name() << "," << Dim_v << ">> | received: [";
    for (size_t i = 0; i < out.size(); ++i) {
        if (i > 0) catalystInfo_m << ", ";
        catalystInfo_m << out[i];
    }
    catalystInfo_m << "]" << endl;
}


}//ippl