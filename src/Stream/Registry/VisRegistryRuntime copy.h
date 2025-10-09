#pragma once
// VisRegistryRuntime: Non-templated heterogeneous registry for visualization & steering.
// Stores only three allowed categories:
//  * Scalars   (arithmetic types) – for steerable parameters or simple values
//  * Particles (deriving from ParticleBaseBase)
//  * Fields    (ippl::Field<V,Dim,...>)
// Provides visitor-based for_each with overload resolution on real concrete type.
// NOTE: To allow mutation of scalars through steering, store reference_wrapper<T>.

#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <type_traits>
#include <utility>
#include <functional>
#include <cassert>

// Forward declarations for categories we match
class ParticleBaseBase; // existing untemplated base

namespace ippl { // Field forward pattern (we only need its primary template name)
    template<class V, unsigned Dim, class... Rest>
    class Field;
}

// Category traits
namespace visreg {

template<class T>
inline constexpr bool is_scalar_v = std::is_arithmetic_v<std::remove_cvref_t<T>>;

template<class T>
inline constexpr bool is_particle_v = std::derived_from<std::remove_cvref_t<T>, ParticleBaseBase>;

template<class T>
struct is_field : std::false_type {};

template<class V, unsigned Dim, class... Rest>
struct is_field<ippl::Field<V, Dim, Rest...>> : std::true_type {};

template<class T>
inline constexpr bool is_field_v = is_field<std::remove_cvref_t<T>>::value;

template<class T>
concept AllowedVisType = is_scalar_v<T> || is_particle_v<T> || is_field_v<T>;

// Helper to unwrap reference_wrapper/pointers uniformly if later needed
template<class T>
struct access_traits {
    using value_type = T;
    static const T& get(const T& v) { return v; }
};

template<class T>
struct access_traits<std::reference_wrapper<T>> {
    using value_type = T;
    static T& get(std::reference_wrapper<T> r) { return r.get(); }
};

template<class T>
struct access_traits<T*> {
    using value_type = T;
    static T& get(T* p) { return *p; }
};

class VisRegistryRuntime {
    struct IEntry {
        std::string label;
        explicit IEntry(std::string L) : label(std::move(L)) {}
        virtual ~IEntry() = default;
        virtual const std::type_info& type() const noexcept = 0;
        virtual void invoke(auto&& f) const = 0; // visitor dispatch
    };

    template<class T>
    struct Entry final : IEntry {
        T value; // can be copy, pointer, reference_wrapper, shared_ptr, etc.
        template<class U>
        Entry(std::string L, U&& v) : IEntry(std::move(L)), value(std::forward<U>(v)) {}
        const std::type_info& type() const noexcept override { return typeid(std::remove_cvref_t<T>); }
        void invoke(auto&& f) const override {
            using RawT = std::remove_cvref_t<T>;
            auto&& obj = access_traits<T>::get(value);
            // Priority order: (label, exact), specialized families, label-less exact, fallback
            if constexpr (std::is_invocable_v<decltype(f), const std::string&, decltype(obj)>) {
                f(this->label, obj);
            } else if constexpr (is_particle_v<RawT> && std::is_invocable_v<decltype(f), const std::string&, const ParticleBaseBase&>) {
                f(this->label, static_cast<const ParticleBaseBase&>(obj));
            } else if constexpr (is_field_v<RawT> && std::is_invocable_v<decltype(f), const std::string&, decltype(obj)>) {
                f(this->label, obj); // already covered by first branch; kept for clarity
            } else if constexpr (is_scalar_v<RawT> && std::is_invocable_v<decltype(f), const std::string&, double>) {
                f(this->label, static_cast<double>(obj));
            } else if constexpr (std::is_invocable_v<decltype(f), decltype(obj)>) {
                f(obj);
            } else if constexpr (is_particle_v<RawT> && std::is_invocable_v<decltype(f), const ParticleBaseBase&>) {
                f(static_cast<const ParticleBaseBase&>(obj));
            } else {
                // Unhandled entry; silently ignore (could log)
            }
        }
    };

    std::vector<std::unique_ptr<IEntry>> entries_;

public:
    VisRegistryRuntime() = default;
    VisRegistryRuntime(const VisRegistryRuntime&) = delete;
    VisRegistryRuntime& operator=(const VisRegistryRuntime&) = delete;
    VisRegistryRuntime(VisRegistryRuntime&&) noexcept = default;
    VisRegistryRuntime& operator=(VisRegistryRuntime&&) noexcept = default;

    template<class T>
    requires AllowedVisType<T>
    void add(std::string label, T&& value) {
        entries_.push_back(std::make_unique<Entry<std::remove_cvref_t<T>>>(std::move(label), std::forward<T>(value)));
    }

    template<class F>
    void for_each(F&& f) const {
        for (auto const& e : entries_) e->invoke(f);
    }

    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }
};

// =====================================================================================
// Factory helpers
// Build a runtime registry from alternating (label, value) arguments.
// Usage:
//   auto reg = visreg::MakeVisRegistryRuntime(
//                 "particles", pcontainer,
//                 "E", std::ref(fieldE),
//                 "rho", std::ref(fieldRho),
//                 "magnetic_scale", std::ref(magnetic_scale));
// A shared_ptr-returning variant is also provided.

namespace detail {
    inline void add_pairs(VisRegistryRuntime&) {}

    template<class L, class V, class... Rest>
    void add_pairs(VisRegistryRuntime& r, L&& label, V&& value, Rest&&... rest) {
        static_assert(AllowedVisType<V>, "VisRegistryRuntime: unsupported value type in factory");
        r.add(std::string(std::forward<L>(label)), std::forward<V>(value));
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
}

} // namespace visreg
