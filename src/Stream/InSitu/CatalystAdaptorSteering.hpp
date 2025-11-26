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

// =====================================================================================
// SIMPLE STRUCT STEERING META (Initial Implementation)
// =====================================================================================
// Supports user-defined structs composed ONLY of existing steerable member types.
// No nesting, no arrays-of-struct (AoS/SoA) yet. Each member is exposed as:
//   <rootLabel>/<memberName>
// Member names are sanitized ( '/' -> '_' ). Registration builds three lambdas
// invoked by runtime registry visitors (Init/Fwd/Fetch).

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
// Generic std::vector<T> steerables (AoS-of-struct members)
// Supports arithmetic, bool and ippl::Button as element types.
// =============================================================
template<typename Elem>
requires (std::is_arithmetic_v<std::decay_t<Elem>> || std::is_enum_v<std::decay_t<Elem>> || std::is_same_v<std::decay_t<Elem>, bool> || std::is_same_v<std::decay_t<Elem>, ippl::Button>)
void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const std::vector<Elem>& arr, const std::string& label )
{
    ca_m << "::Initialize()::InitSteerableChannel(" << label << "):  | Type: std::vector<elem> size=" << arr.size() << endl;
    // Derive namespace from label of the form "array:<ns>.<member>"
    std::string ns = label;
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
        proxyWriter.include(def, label);
    } else if constexpr (std::is_enum_v<std::decay_t<Elem>>) {
        // Enum arrays: register as enum with choices; default from first element if present
        int def = !arr.empty() ? static_cast<int>(arr.front()) : 0;
        // Prefer label-scoped choices, then type-scoped, else fallback to a checkbox
        auto it = enumChoices_.find(label);
        if (it != enumChoices_.end()) {
            proxyWriter.includeEnum(label, it->second, def);
        } else {
            auto itt = enumChoicesByType_.find(std::type_index(typeid(Elem)));
            if (itt != enumChoicesByType_.end()) {
                proxyWriter.includeEnum(label, itt->second, def);
            } else {
                // No choices registered; use a checkbox as a minimal fallback UI
                proxyWriter.includeBool(label, false);
            }
        }
    } else if constexpr (std::is_same_v<std::decay_t<Elem>, bool>) {
        bool def = !arr.empty() ? static_cast<bool>(arr.front()) : false;
        proxyWriter.includeBool(label, def);
    } else if constexpr (std::is_same_v<std::decay_t<Elem>, ippl::Button>) {
        proxyWriter.includeButton(label);
    }

    // Inform proxy writer about desired initial size for this array namespace
    proxyWriter.setArrayInitialSize(ns, arr.size());

    // Inform Python pipeline about each label (still needed for backward mapping)
    conduit_cpp::Node script_args = node["catalyst/scripts/script/args"];
    script_args.append().set_string(label);

    // Create a dedicated forward mesh channel for this struct array prefix, once.
    // std::string mesh_name = std::string("steerable_channel_1D_mesh_") + prefix;
    // auto steerable_channel = node[std::string("catalyst/channels/") + mesh_name];
    // if(!steerable_channel.has_child("type")){
    //     steerable_channel["type"].set("mesh");
    //     auto data = steerable_channel["data"];
    //     data["coordsets/coords/type"].set_string("explicit");
    //     // minimal placeholder coordinates (will be resized in AddSteerableChannel)
    //     data["coordsets/coords/values/x"].set(std::vector<double>{0.0});
    //     data["topologies/sMesh_topo/type"].set("unstructured");
    //     data["topologies/sMesh_topo/coordset"].set("coords");
    //     data["topologies/sMesh_topo/elements/shape"].set("point");
    //     data["topologies/sMesh_topo/elements/connectivity"].set(std::vector<int32_t>{0});
    // }
}

// Init: std::vector<ippl::Vector<T,Dim>> steerables
template<typename T, unsigned Dim_v>
void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const std::vector<ippl::Vector<T, Dim_v>>& arr, const std::string& label )
{
    ca_m << "::Initialize()::InitSteerableChannel(" << label << "):  | Type: std::vector<Vector<" << typeid(T).name() << "," << Dim_v << ">> size=" << arr.size() << endl;
    // Derive namespace from label of the form "array:<ns>.<member>"
    std::string ns = label;
    if (ns.rfind("array:", 0) == 0) ns = ns.substr(6);
    auto dp = ns.find('.');
    if (dp != std::string::npos) ns = ns.substr(0, dp);

    // Register the vector array label with ProxyWriter (parsed as array namespace)
    proxyWriter.includeVector<T, Dim_v>(label);

    // Inform proxy writer about desired initial size for this array namespace
    proxyWriter.setArrayInitialSize(ns, arr.size());

    conduit_cpp::Node script_args = node["catalyst/scripts/script/args"];
    script_args.append().set_string(label);

    // std::string mesh_name = std::string("steerable_channel_1D_mesh_") + prefix;
    // auto steerable_channel = node[std::string("catalyst/channels/") + mesh_name];
    // if(!steerable_channel.has_child("type")){
    //     steerable_channel["type"].set("mesh");
    //     auto data = steerable_channel["data"];
    //     data["coordsets/coords/type"].set_string("explicit");
    //     data["coordsets/coords/values/x"].set(std::vector<double>{0.0});
    //     data["topologies/sMesh_topo/type"].set("unstructured");
    //     data["topologies/sMesh_topo/coordset"].set("coords");
    //     data["topologies/sMesh_topo/elements/shape"].set("point");
    //     data["topologies/sMesh_topo/elements/connectivity"].set(std::vector<int32_t>{0});
    // }
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


// std::vector<T> forward: publish as 1D mesh array under fields/steerable_field_f_<label>/values
template<typename Elem>
requires (std::is_arithmetic_v<std::decay_t<Elem>> || std::is_enum_v<std::decay_t<Elem>> || std::is_same_v<std::decay_t<Elem>, bool> || std::is_same_v<std::decay_t<Elem>, ippl::Button>)
void CatalystAdaptor::AddSteerableChannel( const std::vector<Elem>& arr, const std::string& label )
{
    ca_m << "::Execute()::AddSteerableChannel(vector<elem>) " << label << " | N=" << arr.size() << endl;
    // Group by dynamic array prefix: everything before first underscore.
    std::string prefix = label;
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

    auto f = steerable_data[std::string("fields/steerable_field_f_") + label];
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
    ca_m << "::Execute()::AddSteerableChannel(vector<Vector<" << typeid(T).name() << "," << Dim_v << ">>) " << label << " | N=" << arr.size() << endl;
    std::string prefix = label;
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

    auto f = steerable_data[std::string("fields/steerable_field_f_") + label];
    f["association"].set("vertex");
    f["topology"].set("sMesh_topo");
    f["volume_dependent"].set("false");
    std::vector<double> vx, vy, vz; vx.reserve(N); vy.reserve(N); vz.reserve(N);
    for (const auto& v : arr) {
        vx.push_back(static_cast<double>(v[0]));
        if constexpr (Dim_v >= 2) vy.push_back(static_cast<double>(v[1])); else vy.push_back(0.0);
        if constexpr (Dim_v >= 3) vz.push_back(static_cast<double>(v[2])); else vz.push_back(0.0);
    }
    f["values/x"].set(vx);
    f["values/y"].set(vy);
    f["values/z"].set(vz);
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

    // Safe logging: avoid operator<< on non-streamable types like std::vector<Struct>
    if constexpr (is_std_vector_any<std::decay_t<T>>::value) {
        ca_m << "::Execute()::FetchSteerableChannel(" << label << ") | received vector | size="
             << steerable_scalar_backwardpass.size() << endl;
    } else {
        ca_m << "::Execute()::FetchSteerableChannel(" << label << ") | received:" << steerable_scalar_backwardpass << endl;
    }
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
                    std::string idx_path = *chosen + "." + std::to_string(c);
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


// Fetch std::vector<T> from unified backward channel; resize destination to match
template<typename Elem>
requires (std::is_arithmetic_v<std::decay_t<Elem>> || std::is_enum_v<std::decay_t<Elem>> || std::is_same_v<std::decay_t<Elem>, bool> || std::is_same_v<std::decay_t<Elem>, ippl::Button>)
void CatalystAdaptor::FetchSteerableChannelValue( std::vector<Elem>& out, const std::string& label)
{
    ca_m << "::Execute()::FetchSteerableChannel(vector<elem>) " << label << endl;
    const std::string path = std::string("catalyst/steerable_channel_backward_all/fields/") +
                             "steerable_field_b_" + label + "/values";
    if (!results.has_path(path)) {
        ca_m << "  no backward array for '" << label << "'" << endl;
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
        return;
    }

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
        ca_m << "  [DEBUG] direct pointer read failed for '" << label << "': " << e.what() << endl;
        success = false;
    } catch (...) {
        ca_m << "  [DEBUG] direct pointer read failed for '" << label << "' (unknown error)" << endl;
        success = false;
    }

    if (success) return;

    // Final fallback: read each element by indexed child path (robust but slightly slower)
    for (size_t i = 0; i < n; ++i) {
        std::string child_path = path + "/" + std::to_string(i);
        if (results.has_path(child_path)) {
            assign_from_double(i, results[child_path].to_double());
        } else {
            // If even this fails, leave default value and warn
            ca_m << "  [WARN] Could not read element " << i << " of '" << label << "' via fallback" << endl;
        }
    }
}

// Fetch std::vector<ippl::Vector<T,Dim>>
template<typename T, unsigned Dim_v>
void CatalystAdaptor::FetchSteerableChannelValue( std::vector<ippl::Vector<T, Dim_v>>& out, const std::string& label)
{
    ca_m << "::Execute()::FetchSteerableChannel(vector<Vector<" << typeid(T).name() << "," << Dim_v << ">>) " << label << endl;
    const std::string root = std::string("catalyst/steerable_channel_backward_all/fields/") +
                             "steerable_field_b_" + label + "/values";
    
    
    
    //                          // Prefer named component arrays x/y/z
    // bool has_xyz = results.has_path(root + "/x") && results.has_path(root + "/y") && results.has_path(root + "/z");
    // if (!has_xyz) {
    //     ca_m << "  no backward vector array for '" << label << "' (expected x/y/z)" << endl;
    //     return;
    // }
    // conduit_cpp::Node xn = results[root + "/x"]; 
    // conduit_cpp::Node yn = results[root + "/y"]; 
    // conduit_cpp::Node zn = results[root + "/z"]; 

    // Prefer named component arrays x/y/z
    bool has_xyz = results.has_path(root + "/0") && results.has_path(root + "/1") && results.has_path(root + "/2");
    if (!has_xyz) {
        ca_m << "  no backward vector array for '" << label << "' (expected 0/1/2)" << endl;
        return;
    }
    conduit_cpp::Node xn = results[root + "/0"]; 
    conduit_cpp::Node yn = results[root + "/1"]; 
    conduit_cpp::Node zn = results[root + "/2"]; 





    const size_t nx = xn.dtype().number_of_elements();
    const size_t ny = yn.dtype().number_of_elements();
    const size_t nz = zn.dtype().number_of_elements();
    const size_t N = std::min({nx, ny, nz});
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
        double vy = get_elem(yn, i);
        double vz = get_elem(zn, i);
        ippl::Vector<T, Dim_v> v{};
        v[0] = static_cast<T>(vx);
        if constexpr (Dim_v >= 2) v[1] = static_cast<T>(vy);
        if constexpr (Dim_v >= 3) v[2] = static_cast<T>(vz);
        out[i] = v;
    }
}


}//ippl





































// inline void CatalystAdaptor::AddSteerableChannel( const ippl::LinMap& lm, const std::string& steerable_suffix )
// LinMap forward: delegate to scalar/vector overloads for each sub-element
// {
//     ca_m << "::Execute()::AddSteerableChannel(" << steerable_suffix << ");  | Type: LinMap" << endl;
//     AddSteerableChannel(lm.x_row, steerable_suffix + "_x_row");
//     AddSteerableChannel(lm.y_row, steerable_suffix + "_y_row");
//     AddSteerableChannel(lm.z_row, steerable_suffix + "_z_row");
//     AddSteerableChannel(lm.time , steerable_suffix + "_time");
// }

// inline void CatalystAdaptor::AddSteerableChannel( const ippl::LinMaps& lms, const std::string& steerable_suffix )
//  LinMaps forward: flatten into dynamic lists and set arrays under steerable_field_f_Map_*
// {
//     (void)steerable_suffix; // ignored for LinMaps; XML expects fixed Map_* names
//     ca_m << "::Execute()::AddSteerableChannel(LinMaps)" << endl;
//     auto steerable_channel = node["catalyst/channels/steerable_channel_1D_mesh"];
//     steerable_channel["type"].set("mesh");
//     auto steerable_data = steerable_channel["data"];
//     steerable_data["coordsets/coords/type"].set_string("explicit");
//     const size_t N = lms.time.size();
//     // Provide N dummy coordinates and connectivity for an unstructured point mesh with N points
//     {
//         std::vector<double> xs; xs.reserve(N);
//         for (size_t i = 0; i < N; ++i) xs.push_back(static_cast<double>(i));
//         steerable_data["coordsets/coords/values/x"].set(xs);
//     }
//     steerable_data["topologies/sMesh_topo/type"].set("unstructured");
//     steerable_data["topologies/sMesh_topo/coordset"].set("coords");
//     steerable_data["topologies/sMesh_topo/elements/shape"].set("point");
//     {
//         std::vector<int32_t> conn; conn.reserve(N);
//         for (int32_t i = 0; i < static_cast<int32_t>(N); ++i) conn.push_back(i);
//         steerable_data["topologies/sMesh_topo/elements/connectivity"].set(conn);
//     }
//     // time: N entries as scalar point-data
//     {
//         auto f = steerable_data["fields/steerable_field_f_Map_time"];
//         f["association"].set("vertex");
//         f["topology"].set("sMesh_topo");
//         f["volume_dependent"].set("false");
//         std::vector<double> timeVals; timeVals.reserve(N);
//         for (size_t i = 0; i < N; ++i) timeVals.push_back(lms.time[i]);
//         f["values"].set(timeVals);
//     }
//     // x_row, y_row, z_row: 3-component vector arrays; provide values/x,y,z of length N
//     auto set_vec3_field = [&](const std::string& base, const std::vector<ippl::Vector<double,3>>& rows){
//         auto f = steerable_data[std::string("fields/steerable_field_f_") + base];
//         f["association"].set("vertex");
//         f["topology"].set("sMesh_topo");
//         f["volume_dependent"].set("false");
//         std::vector<double> xs; xs.reserve(N);
//         std::vector<double> ys; ys.reserve(N);
//         std::vector<double> zs; zs.reserve(N);
//         for (size_t i = 0; i < N; ++i) {
//             const auto& v = rows[i];
//             xs.push_back(v[0]); ys.push_back(v[1]); zs.push_back(v[2]);
//         }
//         f["values/x"].set(xs);
//         f["values/y"].set(ys);
//         f["values/z"].set(zs);
//     };
//     set_vec3_field("Map_x_row", lms.x_row);
//     set_vec3_field("Map_y_row", lms.y_row);
//     set_vec3_field("Map_z_row", lms.z_row);
// }

// inline void CatalystAdaptor::AddSteerableChannel( const std::vector<ippl::LinMap>& lm_vec, const std::string& steerable_suffix )
// Add: std::vector<LinMap> forward pass -> convert AoS to SoA and delegate
// {
//     (void)steerable_suffix; // same semantics as LinMaps
//     ca_m << "::Execute()::AddSteerableChannel(vector<LinMap>)" << endl;
//     ippl::LinMaps tmp;
//     const size_t N = lm_vec.size();
//     tmp.time.reserve(N);
//     tmp.x_row.reserve(N);
//     tmp.y_row.reserve(N);
//     tmp.z_row.reserve(N);
//     for(const auto& m : lm_vec){
//         tmp.time.push_back(m.time);
//         tmp.x_row.push_back(m.x_row);
//         tmp.y_row.push_back(m.y_row);
//         tmp.z_row.push_back(m.z_row);
//     }
//     AddSteerableChannel(tmp, steerable_suffix);
// }





// inline void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const ippl::LinMap& lm, const std::string& label )
// LinMap steerable: grouped under one logical label, registers 3 vectors and a time scalar
// {
//     ca_m << "::Initialize()::InitSteerableChannel(" << label << "):  | Type: LinMap" << endl;
//     // Register as a LinMap group with initial defaults from the current values; time rendered as textbox
//     std::array<double,3> xd{ lm.x_row[0], lm.x_row[1], lm.x_row[2] };
//     std::array<double,3> yd{ lm.y_row[0], lm.y_row[1], lm.y_row[2] };
//     std::array<double,3> zd{ lm.z_row[0], lm.z_row[1], lm.z_row[2] };
//     proxyWriter.includeLinMapWithDefaults(label, xd, yd, zd, lm.time);
//     conduit_cpp::Node script_args = node["catalyst/scripts/script/args"];
//     script_args.append().set_string(label + "_x_row");
//     script_args.append().set_string(label + "_y_row");
//     script_args.append().set_string(label + "_z_row");
//     script_args.append().set_string(label + "_time");
// }

// inline void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const ippl::LinMaps& lms, const std::string& label )
// LinMaps steerable: dynamic lists (SoA) across all maps; no ProxyWriter involvement (XML-driven)
// {
//     ca_m << "::Initialize()::InitSteerableChannel(" << label << "):  | Type: LinMaps (dynamic lists)" << endl;
//     // For XML-driven approach, just inform the script about property names
//     conduit_cpp::Node script_args = node["catalyst/scripts/script/args"];
//     script_args.append().set_string("Map_x_row");
//     script_args.append().set_string("Map_y_row");
//     script_args.append().set_string("Map_z_row");
//     script_args.append().set_string("Map_time");
//     // Also publish initial forward arrays so that CatalystInitializePropertiesWithMesh
//     // can find them immediately on proxy creation (before first execute).
//     // This mirrors AddSteerableChannel(LinMaps) but runs during initialize.
//     const size_t N = lms.time.size();
//     auto steerable_channel = node["catalyst/channels/steerable_channel_1D_mesh"];
//     steerable_channel["type"].set("mesh");
//     auto steerable_data = steerable_channel["data"];
//     steerable_data["coordsets/coords/type"].set_string("explicit");
//     {
//         std::vector<double> xs; xs.reserve(N);
//         for (size_t i = 0; i < N; ++i) xs.push_back(static_cast<double>(i));
//         steerable_data["coordsets/coords/values/x"].set(xs);
//     }
//     steerable_data["topologies/sMesh_topo/type"].set("unstructured");
//     steerable_data["topologies/sMesh_topo/coordset"].set("coords");
//     steerable_data["topologies/sMesh_topo/elements/shape"].set("point");
//     {
//         std::vector<int32_t> conn; conn.reserve(N);
//         for (int32_t i = 0; i < static_cast<int32_t>(N); ++i) conn.push_back(i);
//         steerable_data["topologies/sMesh_topo/elements/connectivity"].set(conn);
//     }
//     // time field
//     {
//         auto f = steerable_data["fields/steerable_field_f_Map_time"];
//         f["association"].set("vertex");
//         f["topology"].set("sMesh_topo");
//         f["volume_dependent"].set("false");
//         std::vector<double> timeVals; timeVals.reserve(N);
//         for (size_t i = 0; i < N; ++i) timeVals.push_back(lms.time[i]);
//         f["values"].set(timeVals);
//     }
//     auto set_vec3_field = [&](const std::string& base, const std::vector<ippl::Vector<double,3>>& rows){
//         auto f = steerable_data[std::string("fields/steerable_field_f_") + base];
//         f["association"].set("vertex");
//         f["topology"].set("sMesh_topo");
//         f["volume_dependent"].set("false");
//         std::vector<double> xs; xs.reserve(N);
//         std::vector<double> ys; ys.reserve(N);
//         std::vector<double> zs; zs.reserve(N);
//         for (size_t i = 0; i < N; ++i) {
//             const auto& v = rows[i];
//             xs.push_back(v[0]); ys.push_back(v[1]); zs.push_back(v[2]);
//         }
//         f["values/x"].set(xs);
//         f["values/y"].set(ys);
//         f["values/z"].set(zs);
//     };
//     set_vec3_field("Map_x_row", lms.x_row);
//     set_vec3_field("Map_y_row", lms.y_row);
//     set_vec3_field("Map_z_row", lms.z_row);
// }

// inline void CatalystAdaptor::InitSteerableChannel( [[maybe_unused]] const std::vector<ippl::LinMap>& lm_vec, const std::string& label )
// Init: std::vector<LinMap> (AoS) -> reuse LinMaps (SoA) initialization for identical GUI wiring
// {
//     ca_m << "::Initialize()::InitSteerableChannel(" << label << "):  | Type: vector<LinMap> (AoS alias LinMaps)" << endl;
//     ippl::LinMaps tmp;
//     const size_t N = lm_vec.size();
//     tmp.time.reserve(N);
//     tmp.x_row.reserve(N);
//     tmp.y_row.reserve(N);
//     tmp.z_row.reserve(N);
//     for(const auto& m : lm_vec){
//         tmp.time.push_back(m.time);
//         tmp.x_row.push_back(m.x_row);
//         tmp.y_row.push_back(m.y_row);
//         tmp.z_row.push_back(m.z_row);
//     }
//     InitSteerableChannel(tmp, label);
// }




// LinMap fetch: fetch sub-elements back under suffixed labels
// inline void CatalystAdaptor::FetchSteerableChannelValue( ippl::LinMap& lm, const std::string& label)
// {
//     FetchSteerableChannelValue(lm.x_row, label + "_x_row");
//     FetchSteerableChannelValue(lm.y_row, label + "_y_row");
//     FetchSteerableChannelValue(lm.z_row, label + "_z_row");
//     FetchSteerableChannelValue(lm.time , label + "_time");
// }



// inline void CatalystAdaptor::FetchSteerableChannelValue( ippl::LinMaps& lms, const std::string& label)
// LinMaps fetch: read back arrays from results and populate lms; resize to match incoming lists
// {
//     (void)label; // ignored for LinMaps
//     ca_m << "::Execute()::FetchSteerableChannel(LinMaps)| count=" << lms.time.size()  << endl;
//     auto time_ =  results["catalyst/steerable_channel_backward_all/fields/steerable_field_b_Map_time/values"];
//     size_t N_old = lms.time.size();
//     size_t N_new = time_.dtype().number_of_elements();
//     size_t N = std::max(N_old,N_new);
//     auto fetch_scalar_array = [&](const std::string& name, std::vector<double>& out) -> bool {
//         std::string path = std::string("catalyst/steerable_channel_backward_all/fields/") +
//                            "steerable_field_b_" + name + "/values";
//         if (!results.has_path(path)) return false;
//         // values is a list of doubles: [t0, t1, ...] or an object with numeric keys
//         conduit_cpp::Node vals = results[path];
//         size_t n = 0;
//         n = vals.dtype().number_of_elements();
//         if (n == 0) return false;
//         out.resize(n);
//         for (size_t i = 0; i < n; ++i) out[i] =  vals.as_double_ptr()[i];
//         return true;
//     };
//     auto fetch_vec3_array = [&](const std::string& where,
//                                 std::vector<double>& _x0,
//                                 std::vector<double>& _y1,
//                                 std::vector<double>& _z2) -> bool { 
//         // Case A: named components x/y/z
//         bool has_indexed = results.has_path(where + "/0") && results.has_path(where + "/1") && results.has_path(where + "/2");
//         // bool has_xyz = results.has_path(root + "/x") && results.has_path(root + "/y") && results.has_path(root + "/z");
//         std::cout << where << "has indexed:"  << has_indexed << std::endl;
//         if (has_indexed) {                                
//                 conduit_cpp::Node vals = results[where];
//                 conduit_cpp::Node vals_0 = results[where + "/0"];
//                 // conduit_cpp::Node vals_1 = results[where + "/1"];
//                 // conduit_cpp::Node vals_2 = results[where + "/2"];
//                 for (size_t i = 0; i < N; ++i) {
//                     _x0[i] = vals_0.as_double_ptr()[3*i];
//                     _y1[i] = vals_0.as_double_ptr()[1+i*3];
//                     _z2[i] = vals_0.as_double_ptr()[2+i*3];
//                 }
//             return true;
//         }
//         return false;
//     };
//
//
//
//
//     std::vector<double> timeVals;
//     std::vector<double> x_x(N), x_y(N), x_z(N);
//     std::vector<double> y_x(N), y_y(N), y_z(N);
//     std::vector<double> z_x(N), z_y(N), z_z(N);
//    
//     const std::string where = std::string("catalyst/steerable_channel_backward_all/fields/");
//     bool gotTime = fetch_scalar_array("Map_time", timeVals);
//
//     bool gotX = fetch_vec3_array(where + "steerable_field_b_Map_x_row/values", x_x, x_y, x_z);
//     bool gotY = fetch_vec3_array(where + "steerable_field_b_Map_y_row/values", y_x, y_y, y_z);
//     bool gotZ = fetch_vec3_array(where + "steerable_field_b_Map_z_row/values", z_x, z_y, z_z);
//
//
//             // If nothing present, keep prior values (pre-GUI case)
//             if (!(gotTime || gotX || gotY || gotZ)) {
//                 return;
//             }
//             // Replace current content with exactly what the client sent (prevents mixing)
//             lms.time.assign(N_new, 0.0);
//             lms.x_row.assign(N_new, ippl::Vector<double,3>({0.0,0.0,0.0}));
//             lms.y_row.assign(N_new, ippl::Vector<double,3>({0.0,0.0,0.0}));
//             lms.z_row.assign(N_new, ippl::Vector<double,3>({0.0,0.0,0.0}));
//
//             for (size_t i = 0; i < N_new; ++i) {
//                 if (gotTime && i < timeVals.size()) lms.time[i] = timeVals[i];
//                 if (gotX && i < x_x.size()) lms.x_row[i] = ippl::Vector<double,3>({x_x[i], x_y[i], x_z[i]});
//                 if (gotY && i < y_x.size()) lms.y_row[i] = ippl::Vector<double,3>({y_x[i], y_y[i], y_z[i]});
//                 if (gotZ && i < z_x.size()) lms.z_row[i] = ippl::Vector<double,3>({z_x[i], z_y[i], z_z[i]});
//
//                 /* alternative: */
//                 // if (gotX && i < x_x.size()) fetch_vec3_array(where + "steerable_field_b_Map_x_row/values", lms.x_row[0] = ippl::Vector<double,3>({x_x[i], x_y[i], x_z[i]});
//                 // if (gotY && i < y_x.size()) fetch_vec3_array(lms.y_row[i] = ippl::Vector<double,3>({y_x[i], y_y[i], y_z[i]});
//                 // if (gotZ && i < z_x.size()) fetch_vec3_array(lms.z_row[i] = ippl::Vector<double,3>({z_x[i], z_y[i], z_z[i]});
//             }
//
//     for (size_t i = 0; i < lms.time.size(); ++i) {
//         std::cout << i << ":  time" " " << lms.time[i] << std::endl;
//         std::cout << "Map:" << std::endl;
//         std::cout << lms.x_row[i][0] << " " << lms.x_row[i][1] << " " << lms.x_row[i][2] << std::endl;
//         std::cout << lms.y_row[i][0] << " " << lms.y_row[i][1] << " " << lms.y_row[i][2] << std::endl;
//         std::cout << lms.z_row[i][0] << " " << lms.z_row[i][1] << " " << lms.z_row[i][2] << std::endl;
//     }
//     ca_m << "::Execute()::FetchSteerableChannel(LinMaps)| after fetch | count=" << lms.time.size() << endl;
// }



// inline void CatalystAdaptor::FetchSteerableChannelValue( std::vector<ippl::LinMap>& lm_vec, const std::string& label)
// Fetch: std::vector<LinMap> <- LinMaps
// {
//     (void)label; // same fixed-channel semantics
//     ca_m << "::Execute()::FetchSteerableChannel(vector<LinMap>) | prior_count=" << lm_vec.size() << endl;
//     // ippl::LinMaps tmp;
//     // // Seed tmp with current size (not strictly needed, fetch will overwrite based on results)
//     // tmp.time.reserve(lm_vec.size());
//     // tmp.x_row.reserve(lm_vec.size());
//     // tmp.y_row.reserve(lm_vec.size());
//     // tmp.z_row.reserve(lm_vec.size());
//     // // Perform fetch on LinMaps representation
//     // FetchSteerableChannelValue(tmp, label);
//     // // Convert back to AoS
//     // const size_t N = tmp.time.size();
//     // lm_vec.clear();
//     // lm_vec.reserve(N);
//     // for(size_t i=0;i<N;++i){
//     //     ippl::LinMap m;
//     //     m.time  = tmp.time[i];
//     //     m.x_row = tmp.x_row[i];
//     //     m.y_row = tmp.y_row[i];
//     //     m.z_row = tmp.z_row[i];
//     //     lm_vec.push_back(std::move(m));
//     // }
//     // DOESNT WORK ....
//     auto time_ =  results["catalyst/steerable_channel_backward_all/fields/steerable_field_b_Map_time/values"];
//     size_t N_old = lm_vec.size();
//     size_t N_new = time_.dtype().number_of_elements();
//     size_t N = std::max(N_old,N_new);
//     auto fetch_scalar_array = [&](const std::string& name, std::vector<double>& out) -> bool {
//         std::string path = std::string("catalyst/steerable_channel_backward_all/fields/") +
//                            "steerable_field_b_" + name + "/values";
//         if (!results.has_path(path)) return false;
//         // values is a list of doubles: [t0, t1, ...] or an object with numeric keys
//         conduit_cpp::Node vals = results[path];
//         size_t n = 0;
//         n = vals.dtype().number_of_elements();
//         if (n == 0) return false;
//         out.resize(n);
//         // for (size_t i = 0; i < n; ++i) out[i] =  vals.as_double_ptr()[i];
//         for (size_t i = 0; i < n; ++i) out[i] =  vals.as_int32_ptr()[i];
//         return true;
//     };
//     auto fetch_vec3_array = [&](const std::string& where,
//                                 std::vector<double>& _x0,
//                                 std::vector<double>& _y1,
//                                 std::vector<double>& _z2) -> bool {
//        
//         // Case A: named components x/y/z
//         bool has_indexed = results.has_path(where + "/0") && results.has_path(where + "/1") && results.has_path(where + "/2");
//         // bool has_xyz = results.has_path(root + "/x") && results.has_path(root + "/y") && results.has_path(root + "/z");
//         std::cout << where << "has indexed:"  << has_indexed << std::endl;
//         if (has_indexed) {                                
//                 conduit_cpp::Node vals = results[where];
//                 conduit_cpp::Node vals_0 = results[where + "/0"];
//                 // conduit_cpp::Node vals_1 = results[where + "/1"];
//                 // conduit_cpp::Node vals_2 = results[where + "/2"];
//                 for (size_t i = 0; i < N; ++i) {
//                     _x0[i] = vals_0.as_double_ptr()[3*i];
//                     _y1[i] = vals_0.as_double_ptr()[1+i*3];
//                     _z2[i] = vals_0.as_double_ptr()[2+i*3];
//                 }
//             return true;
//         }
//         return false;
//     };
//
//     std::vector<double> timeVals;
//     std::vector<double> x_x(N), x_y(N), x_z(N);
//     std::vector<double> y_x(N), y_y(N), y_z(N);
//     std::vector<double> z_x(N), z_y(N), z_z(N);  
//     const std::string where = std::string("catalyst/steerable_channel_backward_all/fields/");
//     bool gotTime = fetch_scalar_array("Map_time", timeVals);
//
//     bool gotX = fetch_vec3_array(where + "steerable_field_b_Map_x_row/values", x_x, x_y, x_z);
//     bool gotY = fetch_vec3_array(where + "steerable_field_b_Map_y_row/values", y_x, y_y, y_z);
//     bool gotZ = fetch_vec3_array(where + "steerable_field_b_Map_z_row/values", z_x, z_y, z_z);
//
//
//             // If nothing present, keep prior values (pre-GUI case)
//             if (!(gotTime || gotX || gotY || gotZ)) {
//                 return;
//             }
//             // Replace current content with exactly what the client sent (prevents mixing)
//             lm_vec.assign(N_new, ippl::LinMap() );
//             for (size_t i = 0; i < N_new; ++i) {
//                 if (gotTime && i < timeVals.size()) lm_vec[i].time = timeVals[i];
//                 if (gotX && i < x_x.size()) lm_vec[i].x_row = ippl::Vector<double,3>({x_x[i], x_y[i], x_z[i]});
//                 if (gotY && i < y_x.size()) lm_vec[i].y_row = ippl::Vector<double,3>({y_x[i], y_y[i], y_z[i]});
//                 if (gotZ && i < z_x.size()) lm_vec[i].z_row = ippl::Vector<double,3>({z_x[i], z_y[i], z_z[i]});
//                 /* alternative: */
//                 // if (gotX && i < x_x.size()) fetch_vec3_array(where + "steerable_field_b_Map_x_row/values", lms.x_row[0] = ippl::Vector<double,3>({x_x[i], x_y[i], x_z[i]});
//                 // if (gotY && i < y_x.size()) fetch_vec3_array(lms.y_row[i] = ippl::Vector<double,3>({y_x[i], y_y[i], y_z[i]});
//                 // if (gotZ && i < z_x.size()) fetch_vec3_array(lms.z_row[i] = ippl::Vector<double,3>({z_x[i], z_y[i], z_z[i]});
//             }
//     // for (size_t i = 0; i < lms.time.size(); ++i) {
//     //     std::cout << i << ":  time" " " << lms.time[i] << std::endl;
//     //     std::cout << "Map:" << std::endl;
//     //     std::cout << lm_vec.x_row[i][0] << " " << lm_vec.x_row[i][1] << " " << lm_vec.x_row[i][2] << std::endl;
//     //     std::cout << lm_vec.y_row[i][0] << " " << lm_vec.y_row[i][1] << " " << lm_vec.y_row[i][2] << std::endl;
//     //     std::cout << lm_vec.z_row[i][0] << " " << lm_vec.z_row[i][1] << " " << lm_vec.z_row[i][2] << std::endl;
//     // }
//     ca_m << "::Execute()::FetchSteerableChannel(vector<LinMap>) | new_count=" << lm_vec.size() << endl;
// }
