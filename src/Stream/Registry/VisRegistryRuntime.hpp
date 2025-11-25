#pragma once
#include "Stream/Registry/VisRegistryRuntime.h"


namespace ippl {

    // LVALUE overload: bind callbacks to referenced object
    template<class T>
    requires(!is_allowed_shared_ptr<typename std::decay<T>::type>::value)
    void VisRegistryRuntime::add(const std::string& label, T& value) {
        using DecayT = std::decay_t<T>;
        Entry e; e.label = label;

        // Visualization types (fields/particles)
        if constexpr (AllowedVisType_v<T>) {
            e.do_init = [&value, label](InitVisitor_t& v) { v(label, value); };
            e.do_exec = [&value, label](ExecuteVisitor_t& v) { v(label, value); };
            entries_.push_back(std::move(e));
            index_exec_[label] = entries_.size() - 1;
            return;
        }

        // Standard steerables (scalars, vectors, buttons, enums, LinMap(s), etc.)
        if constexpr (AllowedSteerType_v<T>) {
            e.do_steer_init  = [&value, label](SteerInitVisitor_t& v)    { v(label, value); };
            e.do_steer_fwd   = [&value, label](SteerForwardVisitor_t& v) { v(label, value); };
            e.do_steer_fetch = [&value, label](SteerFetchVisitor_t& v)   { v(label, value); };
            entries_.push_back(std::move(e));
            return; // no execute index for pure steerables
        }

        // Registered user struct (simple steering aggregation)
        if (ippl::detail::StructMeta<DecayT>::registered) {
            e.do_steer_init  = [&value, label](SteerInitVisitor_t& v)    { ippl::detail::StructMeta<DecayT>::do_init(v, value, label); };
            e.do_steer_fwd   = [&value, label](SteerForwardVisitor_t& v) { ippl::detail::StructMeta<DecayT>::do_fwd(v, value, label); };
            e.do_steer_fetch = [&value, label](SteerFetchVisitor_t& v)   { ippl::detail::StructMeta<DecayT>::do_fetch(v, value, label); };
            entries_.push_back(std::move(e));
            return;
        }

        // Fallback: unsupported type -> throw (runtime instead of static_assert for flexibility)
    throw IpplException("VisRegistryRuntime::add", std::string("Unsupported value type for registry entry '") + label + "' (type=" + typeid(T).name() + ")");
    }


    // Overload: add shared_ptr<U> by binding to referenced object and keeping lifetime
    template<class U>
    void VisRegistryRuntime::add(const std::string& label, const std::shared_ptr<U>& ptr) {
        using DecayU = std::decay_t<U>;
        if (!ptr) return;
        // Allow visualisation, steerables, and registered structs.
        if constexpr (!(AllowedRegistryType_v<DecayU>)) {
            if (!ippl::detail::StructMeta<DecayU>::registered) {
                throw IpplException("VisRegistryRuntime::add(shared_ptr)", std::string("Unsupported shared_ptr type for entry '") + label + "' (type=" + typeid(U).name() + ")");
            }
        }
        add(label, *ptr); // delegates to lvalue path (already handles struct/meta)
        // keep alive by capturing shared_ptr in a no-op callback
        auto& e = entries_.back();
        auto keep = ptr; // copy
        // augment one of the callbacks or create a dummy fetch to hold lifetime
        if (!e.do_init) {
            e.do_init = [keep](InitVisitor_t&) {};
        } else {
            auto fn = e.do_init;
            e.do_init = [keep, fn](InitVisitor_t& v) { fn(v); };
        }
    }
    






// =====================================================================================
// Factory helpers
// Build a runtime registry from alternating (label, value) arguments.
// Usage:
//   auto reg = MakeVisRegistryRuntime(
//                 "particles", pcontainer,
//                 "E", std::ref(fieldE),
//                 "rho", std::ref(fieldRho),
//                 "magnetic_scale", std::ref(magnetic_scale));
// A shared_ptr-returning variant is also provided.

namespace detail {

    /**
     * @brief Base case for add_pairs: does nothing (end of recursion).
     * @param r The registry (unused).
     */
    inline void add_pairs(VisRegistryRuntime&) {}

    /**
     * @brief Recursively adds label-value pairs to a VisRegistryRuntime.
     *
     * This helper is used by MakeVisRegistryRuntime and MakeVisRegistryRuntimePtr to build a registry from alternating label-value arguments.
     *
     * @tparam L The type of the label (should be convertible to std::string).
     * @tparam V The type of the value (must satisfy AllowedRegistryTypeOrShared_v).
     * @tparam Rest The remaining label-value argument types.
     * @param r The registry to add to.
     * @param label The label for the entry.
     * @param value The value to register.
     * @param rest The remaining label-value arguments.
     */
    template<class L, class V, class... Rest>
    void add_pairs(VisRegistryRuntime& r, L&& label, V&& value, Rest&&... rest) {
        using DV = typename std::decay<V>::type;
        static_assert(AllowedRegistryTypeOrShared_v<DV> || std::is_enum_v<DV>,
                      "VisRegistryRuntime: unsupported value type in factory");

        // Materialize label as std::string to avoid ambiguous overloads
        std::string lbl{std::forward<L>(label)};

        if constexpr (std::is_lvalue_reference_v<V&&>) {
            // lvalue path: keep reference semantics for non-owning entries
            r.add(lbl, value); // prefers const std::string& overloads
        } 
        // else {
        //     // rvalue path: allow ownership-taking overload
        //     r.add(std::move(lbl), std::forward<V>(value));
        // }

        if constexpr (sizeof...(Rest) > 0) {
            static_assert(sizeof...(Rest) % 2 == 0, "Factory arguments must be label-value pairs");
            add_pairs(r, std::forward<Rest>(rest)...);
        }
    }
} // namespace detail

template<class... Args>
VisRegistryRuntime MakeVisRegistryRuntime(Args&&... args) {
    static_assert(sizeof...(Args) % 2 == 0, "MakeVisRegistryRuntime requires label-value pairs");
    VisRegistryRuntime reg;
    if constexpr (sizeof...(Args) > 0) {
        detail::add_pairs(reg, std::forward<Args>(args)...);
    }
    return reg;
}

template<class... Args>
std::shared_ptr<VisRegistryRuntime> MakeVisRegistryRuntimePtr(Args&&... args) {
    static_assert(sizeof...(Args) % 2 == 0, "MakeVisRegistryRuntimePtr requires label-value pairs");
    auto reg = std::make_shared<VisRegistryRuntime>();
    if constexpr (sizeof...(Args) > 0) {
        detail::add_pairs(*reg, std::forward<Args>(args)...);
    }
    return reg;

} // detail
} //ippl