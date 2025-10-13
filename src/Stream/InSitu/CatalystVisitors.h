#pragma once
#include <filesystem>
#include <string>
#include <type_traits>
#include "Stream/Registry/VisRegistryRuntime.h"
#include "Stream/Registry/ViewRegistry.h"


#include <catalyst.hpp>
// Avoid circular include with CatalystAdaptor.h; forward-declare needed types/functions instead.
namespace conduit_cpp { class Node; }

namespace CatalystAdaptor {

// Forward declare utility free functions implemented in CatalystAdaptor.hpp
template<typename T, unsigned Dim, class... ViewArgs>
void init_entry(const ippl::Field<T, Dim, ViewArgs...>&, const std::string, conduit_cpp::Node&, const std::filesystem::path);
template<typename T, unsigned Dim, unsigned Dim_v, class... ViewArgs>
void init_entry(const ippl::Field<ippl::Vector<T, Dim_v>, Dim, ViewArgs...>&, const std::string, conduit_cpp::Node&, const std::filesystem::path);
template<typename T>
requires std::derived_from<std::decay_t<T>, ippl::ParticleBaseBase>
void init_entry(const T&, const std::string, conduit_cpp::Node&, const std::filesystem::path);

template<typename T, unsigned Dim, class... ViewArgs>
void execute_entry(const ippl::Field<T, Dim, ViewArgs...>&, const std::string, conduit_cpp::Node&, ViewRegistry&);
template<typename T, unsigned Dim, unsigned Dim_v, class... ViewArgs>
void execute_entry(const ippl::Field<ippl::Vector<T, Dim_v>, Dim, ViewArgs...>&, const std::string, conduit_cpp::Node&, ViewRegistry&);
template<typename T>
requires std::derived_from<std::decay_t<T>, ippl::ParticleBaseBase>
void execute_entry(const T&, const std::string, conduit_cpp::Node&, ViewRegistry&);

template<typename T>
void AddSteerableChannel(T, std::string, conduit_cpp::Node&);
template<typename T>
void FetchSteerableChannelValue(T&, std::string, conduit_cpp::Node&);

struct InitVisitor {
    conduit_cpp::Node& node;
    std::filesystem::path source_dir;
    template<class V, unsigned Dim, class... Rest>
    void operator()(const std::string& label, const ippl::Field<V, Dim, Rest...>& f) const {
        init_entry(f, label, node, source_dir);
    }
    template<class T, unsigned Dim, unsigned Dim_v, class... Rest>
    void operator()(const std::string& label, const ippl::Field<ippl::Vector<T, Dim_v>, Dim, Rest...>& f) const {
        init_entry(f, label, node, source_dir);
    }
    template<typename T>
    requires std::derived_from<std::decay_t<T>, ippl::ParticleBaseBase>
    void operator()(const std::string& label, const T& pc) const {
        init_entry(pc, label, node, source_dir);
    }
    template<class S> requires std::is_arithmetic_v<S>
    void operator()(const std::string& label, S value) const {
        // Optional: create steerable channel already at init time
        // AddSteerableChannel(value, label, node);
        (void)label; (void)value;
    }
};

struct ExecuteVisitor {
    conduit_cpp::Node& node;
    ViewRegistry& vreg;
    template<class V, unsigned Dim, class... Rest>
    void operator()(const std::string& label, const ippl::Field<V, Dim, Rest...>& f) const {
        execute_entry(f, label, node, vreg);
    }
    template<class T, unsigned Dim, unsigned Dim_v, class... Rest>
    void operator()(const std::string& label, const ippl::Field<ippl::Vector<T, Dim_v>, Dim, Rest...>& f) const {
        execute_entry(f, label, node, vreg);
    }
    template<typename T>
    requires std::derived_from<std::decay_t<T>, ippl::ParticleBaseBase>
    void operator()(const std::string& label, const T& pc) const {
        execute_entry(pc, label, node, vreg);
    }
};

// Forward steering: add steerable scalar channels only
struct SteerForwardVisitor {
    conduit_cpp::Node& node;
    template<class S> requires std::is_arithmetic_v<std::decay_t<S>>
    void operator()(const std::string& label, S value) const {
        AddSteerableChannel(value, label, node);
    }
    template<class T>
    requires (!std::is_arithmetic_v<std::decay_t<T>>)
    void operator()(const std::string&, const T&) const { /* ignore non-scalars */ }
};

// Backward steering fetch (mutates external scalars)
struct SteerFetchVisitor {
    conduit_cpp::Node& results;
    template<class S> requires std::is_arithmetic_v<std::decay_t<S>>
    void operator()(const std::string& label, S& value) const {
        // NOTE: runtime registry currently stores by value; to mutate you need stored reference or pointer.
        // If registry adjusted to store reference_wrapper<S>, update dispatch accordingly.
        FetchSteerableChannelValue(value, label, results);
    }
    template<class T>
    requires (!std::is_arithmetic_v<std::decay_t<T>>)
    void operator()(const std::string&, const T&) const { /* ignore non-scalars */ }
};

} // namespace CatalystAdaptor
