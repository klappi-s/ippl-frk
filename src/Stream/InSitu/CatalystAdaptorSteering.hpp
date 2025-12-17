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
    // Lambdas capturing (name, memberPtr) pack for each steering phase.
    static inline std::function<void(CatalystAdaptor::SteerInitVisitor&, const T&, const std::string&)>    do_init;
    static inline std::function<void(CatalystAdaptor::SteerForwardVisitor&, const T&, const std::string&)> do_fwd;
    static inline std::function<void(CatalystAdaptor::SteerFetchVisitor&, T&, const std::string&)>         do_fetch;
    // Array-of-struct aggregation (vector<T>) – built at registration time.
    static inline std::function<void(CatalystAdaptor::SteerInitVisitor&, const std::vector<T>&, const std::string&)>    do_init_vec;
    static inline std::function<void(CatalystAdaptor::SteerForwardVisitor&, const std::vector<T>&, const std::string&)> do_fwd_vec;
    static inline std::function<void(CatalystAdaptor::SteerFetchVisitor&, std::vector<T>&, const std::string&)>         do_fetch_vec;
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

    // Visitor lambdas reuse generic applicator.
    detail::StructMeta<DecayT>::do_init = [pack](SteerInitVisitor& vis, const DecayT& obj, const std::string& root) mutable {
        std::apply([&](auto&&... all){ ippl::detail::apply_struct_members(vis, obj, ippl::detail::sanitize_label(root), all...); }, pack);
    };
    detail::StructMeta<DecayT>::do_fwd = [pack](SteerForwardVisitor& vis, const DecayT& obj, const std::string& root) mutable {
        std::apply([&](auto&&... all){ ippl::detail::apply_struct_members(vis, obj, ippl::detail::sanitize_label(root), all...); }, pack);
    };
    detail::StructMeta<DecayT>::do_fetch = [pack](SteerFetchVisitor& vis, DecayT& obj, const std::string& root) mutable {
        std::apply([&](auto&&... all){ ippl::detail::apply_struct_members(vis, obj, ippl::detail::sanitize_label(root), all...); }, pack);
    };

    // ================= Array-of-Struct (vector<T>) support =================
    detail::StructMeta<DecayT>::do_init_vec = [pack](SteerInitVisitor& vis, const std::vector<DecayT>& arr, const std::string& root) mutable {
        if (arr.empty()) return;
        constexpr size_t PC = PairCount;
        [&]<std::size_t... I>(std::index_sequence<I...>){
            ( [&](){
                using MemberPtrT = std::tuple_element_t<2*I+1, decltype(pack)>;
                MemberPtrT mptr = std::get<2*I+1>(pack);
                auto rawName = std::get<2*I>(pack);
                std::string memberLabel = "array:" + ippl::detail::sanitize_label(root) + '.' + ippl::detail::sanitize_label(std::string(rawName));
                using MType = std::remove_reference_t<decltype(std::declval<DecayT>().*mptr)>;
                std::vector<MType> collected; collected.reserve(arr.size());
                for (auto& el : arr) collected.push_back(el.*mptr);
                vis(memberLabel, collected);
            }(), ... );
        }(std::make_index_sequence<PC>{});
    };

    detail::StructMeta<DecayT>::do_fwd_vec = [pack](SteerForwardVisitor& vis, const std::vector<DecayT>& arr, const std::string& root) mutable {
        if (arr.empty()) return;
        constexpr size_t PC = PairCount;
        [&]<std::size_t... I>(std::index_sequence<I...>){
            ( [&](){
                using MemberPtrT = std::tuple_element_t<2*I+1, decltype(pack)>;
                MemberPtrT mptr = std::get<2*I+1>(pack);
                auto rawName = std::get<2*I>(pack);
                std::string memberLabel = "array:" + ippl::detail::sanitize_label(root) + '.' + ippl::detail::sanitize_label(std::string(rawName));
                using MType = std::remove_reference_t<decltype(std::declval<DecayT>().*mptr)>;
                std::vector<MType> collected; collected.reserve(arr.size());
                for (auto& el : arr) collected.push_back(el.*mptr);
                vis(memberLabel, collected);
            }(), ... );
        }(std::make_index_sequence<PC>{});
    };

    detail::StructMeta<DecayT>::do_fetch_vec = [pack](SteerFetchVisitor& vis, std::vector<DecayT>& arr, const std::string& root) mutable {
        constexpr size_t PC = PairCount;
        [&]<std::size_t... I>(std::index_sequence<I...>){
            ( [&](){
                using MemberPtrT = std::tuple_element_t<2*I+1, decltype(pack)>;
                MemberPtrT mptr = std::get<2*I+1>(pack);
                auto rawName = std::get<2*I>(pack);
                std::string memberLabel = "array:" + ippl::detail::sanitize_label(root) + '.' + ippl::detail::sanitize_label(std::string(rawName));
                using MType = std::remove_reference_t<decltype(std::declval<DecayT>().*mptr)>;
                std::vector<MType> tmp; tmp.reserve(arr.size());
                for (auto& el : arr) tmp.push_back(el.*mptr);
                vis(memberLabel, tmp); // fetch updates tmp
                if (tmp.size() != arr.size()) {
                    arr.resize(tmp.size()); // default construct new structs
                }
                // Write back member values
                for (std::size_t i = 0; i < tmp.size(); ++i) {
                    arr[i].*mptr = tmp[i];
                }
            }(), ... );
        }(std::make_index_sequence<PC>{});
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
    enumChoicesByType_[std::type_index(typeid(E))] = std::move(conv);
}




// =====================================================================================
// INITIALISATION:
// =====================================================================================

template<typename T>
requires (!std::is_enum_v<std::decay_t<T>>)
void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const T& steerable_scalar_forwardpass,  const std::string& label ){
    ca_m << "::Initialize()::InitSteerableChannel(" << label << "):  | Type: " << typeid(T).name() << endl;
    // Only invoke ProxyWriter scalar include for arithmetic types; others are placeholders.
    if constexpr (std::is_arithmetic_v<std::decay_t<T>>) {
        proxyWriter.include(steerable_scalar_forwardpass, label);
    } else {
        ca_warn << "ProxyWriter placeholder: include() for label '" << label
                << "' (type=" << typeid(T).name() << ") not implemented yet (TODO)." << endl;
    }
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
        // Try type-based enum choices
        auto itt = enumChoicesByType_.find(std::type_index(typeid(E)));
        if (itt != enumChoicesByType_.end()) {
            proxyWriter.includeEnum(label, itt->second, static_cast<int>(e));
        } else {
            // Fallback to a checkbox if no choices registered
            proxyWriter.includeBool(label, false);
        }
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



// =============================================================
// Generic std::vector<T> steerables (Array-of-struct members)
// Supports arithmetic, bool and ippl::Button as element types.
// =============================================================
template<typename Elem>
requires (std::is_arithmetic_v<std::decay_t<Elem>> || std::is_enum_v<std::decay_t<Elem>> || std::is_same_v<std::decay_t<Elem>, bool> || std::is_same_v<std::decay_t<Elem>, ippl::Button>)
void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const std::vector<Elem>& arr, const std::string& label )
{
    // Normalize: ensure 'array:' prefix for any std::vector steerable labels (user shouldn't add it)
    const std::string alabel = (label.rfind("array:", 0) == 0) ? label : std::string("array:") + label;
    ca_m << "::Initialize()::InitSteerableChannel(" << alabel << "):  | Type: std::vector<elem> size=" << arr.size() << endl;
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
        proxyWriter.include(def, alabel);
    } else if constexpr (std::is_enum_v<std::decay_t<Elem>>) {
        // Enum arrays: register as enum with choices; default from first element if present
        int def = !arr.empty() ? static_cast<int>(arr.front()) : 0;
        // Prefer label-scoped choices, then type-scoped, else fallback to a checkbox
        auto it = enumChoices_.find(alabel);
        if (it != enumChoices_.end()) {
            proxyWriter.includeEnum(alabel, it->second, def);
        } else {
            auto itt = enumChoicesByType_.find(std::type_index(typeid(Elem)));
            if (itt != enumChoicesByType_.end()) {
                proxyWriter.includeEnum(alabel, itt->second, def);
            } else {
                // No choices registered; use a checkbox as a minimal fallback UI
                proxyWriter.includeBool(alabel, false);
            }
        }
    } else if constexpr (std::is_same_v<std::decay_t<Elem>, bool>) {
        bool def = !arr.empty() ? static_cast<bool>(arr.front()) : false;
        proxyWriter.includeBool(alabel, def);
    } else if constexpr (std::is_same_v<std::decay_t<Elem>, ippl::Button>) {
        proxyWriter.includeButton(alabel);
    }

    // Inform proxy writer about desired initial size for this array namespace
    proxyWriter.setArrayInitialSize(ns, arr.size());

    // Inform Python pipeline about each label (still needed for backward mapping)
    conduit_cpp::Node script_args = node["catalyst/scripts/script/args"];
    script_args.append().set_string(alabel);

}

// Init: std::vector<ippl::Vector<T,Dim>> steerables
template<typename T, unsigned Dim_v>
void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const std::vector<ippl::Vector<T, Dim_v>>& arr, const std::string& label )
{
    // Normalize: ensure 'array:' prefix for any std::vector steerable labels
    const std::string alabel = (label.rfind("array:", 0) == 0) ? label : std::string("array:") + label;
    ca_m << "::Initialize()::InitSteerableChannel(" << alabel << "):  | Type: std::vector<Vector<" << typeid(T).name() << "," << Dim_v << ">> size=" << arr.size() << endl;
    // Derive namespace from canonical label of the form "array:<ns>.<member>"
    std::string ns = alabel;
    if (ns.rfind("array:", 0) == 0) ns = ns.substr(6);
    auto dp = ns.find('.');
    if (dp != std::string::npos) ns = ns.substr(0, dp);

    // Register the vector array label with ProxyWriter (parsed as array namespace)
    proxyWriter.includeVector<T, Dim_v>(alabel);

    // Inform proxy writer about desired initial size for this array namespace
    proxyWriter.setArrayInitialSize(ns, arr.size());

    conduit_cpp::Node script_args = node["catalyst/scripts/script/args"];
    script_args.append().set_string(alabel);

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

    if constexpr (std::is_integral_v<T>) {
        const int vx = static_cast<int>(steerable_vec_forwardpass[0]);
        fnode["values/x"].set(vx);
        if constexpr (Dim_v >= 2) {
            const int vy = static_cast<int>(steerable_vec_forwardpass[1]);
            fnode["values/y"].set(vy);
        }
        if constexpr (Dim_v >= 3) {
            const int vz = static_cast<int>(steerable_vec_forwardpass[2]);
            fnode["values/z"].set(vz);
        }
    } else {
        const double vx = static_cast<double>(steerable_vec_forwardpass[0]);
        fnode["values/x"].set(vx);
        if constexpr (Dim_v >= 2) {
            const double vy = static_cast<double>(steerable_vec_forwardpass[1]);
            fnode["values/y"].set(vy);
        }
        if constexpr (Dim_v >= 3) {
            const double vz = static_cast<double>(steerable_vec_forwardpass[2]);
            fnode["values/z"].set(vz);
        }
    }
}


// std::vector<T> forward: publish as 1D mesh array under fields/steerable_field_f_<label>/values
template<typename Elem>
requires (std::is_arithmetic_v<std::decay_t<Elem>> || std::is_enum_v<std::decay_t<Elem>> || std::is_same_v<std::decay_t<Elem>, bool> || std::is_same_v<std::decay_t<Elem>, ippl::Button>)
void CatalystAdaptor::AddSteerableChannel( const std::vector<Elem>& arr, const std::string& label )
{
    // Normalize: ensure 'array:' prefix so downstream pipeline/proxies match
    const std::string alabel = (label.rfind("array:", 0) == 0) ? label : std::string("array:") + label;
    ca_m << "::Execute()::AddSteerableChannel(vector<elem>) " << alabel << " | N=" << arr.size() << endl;
    // Group by dynamic array prefix: everything before first underscore.
    std::string prefix = alabel;
    auto us_pos = prefix.find('.');
    if(us_pos != std::string::npos) prefix = prefix.substr(0, us_pos);
    std::string mesh_name = std::string("steerable_channel_1D_mesh_") + prefix;
    auto steerable_channel = node[std::string("catalyst/channels/") + mesh_name];
    steerable_channel["type"].set("mesh");
    auto steerable_data = steerable_channel["data"];
    steerable_data["coordsets/coords/type"].set_string("explicit");
    const size_t N = arr.size();
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

    auto f = steerable_data[std::string("fields/steerable_field_f_") + alabel];
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
void CatalystAdaptor::AddSteerableChannel( const std::vector<ippl::Vector<T, Dim_v>>& arr, const std::string& label )
{
    // Normalize: ensure 'array:' prefix so downstream pipeline/proxies match
    const std::string alabel = (label.rfind("array:", 0) == 0) ? label : std::string("array:") + label;
    ca_m << "::Execute()::AddSteerableChannel(vector<Vector<" << typeid(T).name() << "," << Dim_v << ">>) " << alabel << " | N=" << arr.size() << endl;
    std::string prefix = alabel;
    auto us_pos = prefix.find('.');
    if(us_pos != std::string::npos) prefix = prefix.substr(0, us_pos);
    std::string mesh_name = std::string("steerable_channel_1D_mesh_") + prefix;
    auto steerable_channel = node[std::string("catalyst/channels/") + mesh_name];
    steerable_channel["type"].set("mesh");
    auto steerable_data = steerable_channel["data"];
    steerable_data["coordsets/coords/type"].set_string("explicit");
    const size_t N = arr.size();
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

    auto f = steerable_data[std::string("fields/steerable_field_f_") + alabel];
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
void CatalystAdaptor::FetchSteerableChannelValue( T& steerable_scalar_backwardpass, const std::string& label) {
    // ca_m << "::Execute()::FetchSteerableChannel(" << label  << ") | Type: " << typeid(T).name() << endl;

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

    // Safe logging: avoid operator<< on non-streamable types like std::vector<Struct>
    if constexpr (is_std_vector_any<std::decay_t<T>>::value) {
        ca_m << "::Execute()::FetchSteerableChannel(" << label << ") | Type: " << typeid(T).name() << " | received vector | size="
             << steerable_scalar_backwardpass.size() << endl;
    } else {
        ca_m << "::Execute()::FetchSteerableChannel(" << label << ") | Type: " << typeid(T).name() << " | received: " << steerable_scalar_backwardpass << endl;
    }
}

// Enum overload explicit (clarity)
template<typename E>
requires (std::is_enum_v<std::decay_t<E>>)
void CatalystAdaptor::FetchSteerableChannelValue( E& e, const std::string& label)
{
    // ca_m << "::Execute()::FetchSteerableChannel(" << label  << ") | Type: Enum| Value sent:" << /* enumChoices_[label].second  */to_string(e) << endl;
    std::string unified_path = std::string("catalyst/steerable_channel_backward_all/fields/") +
                               "steerable_field_b_" + label + "/values";
    if (!results.has_path(unified_path)) {
        ca_m << "  no backward enum found for label '" << label << "'" << endl;
        return;
    }
    conduit_cpp::Node values_node = results[unified_path];
    if (!values_node.dtype().is_number()) return;
    e = static_cast<E>(values_node.to_int32());

    ca_m << "::Execute()::FetchSteerableChannel(" << label  << ") | Type: Enum | received: " << to_string(e) << endl;
}


/* overload for: ippl::Vector */
template<typename T, unsigned Dim_v>
void CatalystAdaptor::FetchSteerableChannelValue( ippl::Vector<T, Dim_v>& steerable_vec_backwardpass, const std::string& label)
{
    const unsigned comps = Dim_v > 3 ? 3u : Dim_v;
    
    std::string unified_vec_values = std::string("catalyst/steerable_channel_backward_all/fields/") +
                                     "steerable_field_b_" + label + "/values";
    const std::string* chosen = nullptr;
    if (results.has_path(unified_vec_values)) chosen = &unified_vec_values;
    
    if (chosen) {
        conduit_cpp::Node vnode = results[*chosen];
        
        // Handle 1D vector appearing as scalar
        if (comps == 1 && vnode.dtype().is_number()) {
             steerable_vec_backwardpass[0] = static_cast<T>(vnode.to_double());
             ca_m << "::Execute()::FetchSteerableChannel(" << label  << ") | Type: Vector<" << typeid(T).name() << "," << Dim_v << "> | received: " << steerable_vec_backwardpass << endl;
             return;
        }

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
            ca_m << "::Execute()::FetchSteerableChannel(" << label  << ") | Type: Vector<" << typeid(T).name() << "," << Dim_v << "> | received: " << steerable_vec_backwardpass << endl;
        } else {
             ca_warn << "  backward vector '" << label << "' missing components." << endl;
        }
    }
    else {
        ca_warn << "  no backward vector found for label '" << label << "' under expected paths." << endl;
    }
}


// Fetch std::vector<T> from unified backward channel; resize destination to match
template<typename Elem>
requires (std::is_arithmetic_v<std::decay_t<Elem>> || std::is_enum_v<std::decay_t<Elem>> || std::is_same_v<std::decay_t<Elem>, bool> || std::is_same_v<std::decay_t<Elem>, ippl::Button>)
void CatalystAdaptor::FetchSteerableChannelValue( std::vector<Elem>& out, const std::string& label)
{
    // Normalize: ensure 'array:' prefix matches fields created in proxies/pipeline
    const std::string alabel = (label.rfind("array:", 0) == 0) ? label : std::string("array:") + label;
    // ca_m << "::Execute()::FetchSteerableChannel(vector<elem>) " << alabel << endl;
    const std::string path = std::string("catalyst/steerable_channel_backward_all/fields/") +
                             "steerable_field_b_" + alabel + "/values";
    if (!results.has_path(path)) {
        ca_m << "  no backward array for '" << alabel << "'" << endl;
        return;
    }
    conduit_cpp::Node vals = results[path];
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
            ca_m << "  [DEBUG] direct pointer read failed for '" << alabel << "': " << e.what() << endl;
            success = false;
        } catch (...) {
            ca_m << "  [DEBUG] direct pointer read failed for '" << alabel << "' (unknown error)" << endl;
            success = false;
        }

        if (!success) {
            // Final fallback: read each element by indexed child path (robust but slightly slower)
            for (size_t i = 0; i < n; ++i) {
                std::string child_path = path + "/" + std::to_string(i);
                if (results.has_path(child_path)) {
                    assign_from_double(i, results[child_path].to_double());
                } else {
                    // If even this fails, leave default value and warn
                    ca_m << "  [WARN] Could not read element " << i << " of '" << alabel << "' via fallback" << endl;
                }
            }
        }
    }

    // Log the received array
    ca_m << "::Execute()::FetchSteerableChannel(" << alabel << ") | Type: vector<elem> | received: [";
    for (size_t i = 0; i < out.size(); ++i) {
        if (i > 0) ca_m << ", ";
        if constexpr (std::is_enum_v<std::decay_t<Elem>>) ca_m << to_string(out[i]);
        else ca_m << out[i];
    }
    ca_m << "]" << endl;
}

// Fetch std::vector<ippl::Vector<T,Dim>>
template<typename T, unsigned Dim_v>
void CatalystAdaptor::FetchSteerableChannelValue( std::vector<ippl::Vector<T, Dim_v>>& out, const std::string& label)
{
    // Normalize: ensure 'array:' prefix matches fields created in proxies/pipeline
    const std::string alabel = (label.rfind("array:", 0) == 0) ? label : std::string("array:") + label;
    // ca_m << "::Execute()::FetchSteerableChannel(vector<Vector<" << typeid(T).name() << "," << Dim_v << ">>) " << alabel << endl;
    const std::string root = std::string("catalyst/steerable_channel_backward_all/fields/") +
                             "steerable_field_b_" + alabel + "/values";
    
    // Handle 1D vector array appearing as flat array
    if constexpr (Dim_v == 1) {
        if (results.has_path(root) && !results.has_path(root + "/0")) {
            conduit_cpp::Node vals = results[root];
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
            
            ca_m << "::Execute()::FetchSteerableChannel(" << alabel << ") | Type: vector<Vector<" << typeid(T).name() << "," << Dim_v << ">> | received: [";
            for (size_t i = 0; i < out.size(); ++i) {
                if (i > 0) ca_m << ", ";
                ca_m << out[i];
            }
            ca_m << "]" << endl;
            return;
        }
    }

    // Prefer named component arrays x/y/z
    bool has_xyz = results.has_path(root + "/0");
    if constexpr (Dim_v >= 2) has_xyz = has_xyz && results.has_path(root + "/1");
    if constexpr (Dim_v >= 3) has_xyz = has_xyz && results.has_path(root + "/2");

    if (!has_xyz) {
        ca_m << "  no backward vector array for '" << alabel << "' (expected components 0" 
             << (Dim_v >= 2 ? "/1" : "") << (Dim_v >= 3 ? "/2" : "") << ")" << endl;
        return;
    }
    conduit_cpp::Node xn = results[root + "/0"]; 
    conduit_cpp::Node yn;
    if constexpr (Dim_v >= 2) yn = results[root + "/1"];
    conduit_cpp::Node zn;
    if constexpr (Dim_v >= 3) zn = results[root + "/2"];

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

    ca_m << "::Execute()::FetchSteerableChannel(" << alabel << ") | Type: vector<Vector<" << typeid(T).name() << "," << Dim_v << ">> | received: [";
    for (size_t i = 0; i < out.size(); ++i) {
        if (i > 0) ca_m << ", ";
        ca_m << out[i];
    }
    ca_m << "]" << endl;
}


}//ippl