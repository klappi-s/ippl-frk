#pragma once
#include <filesystem>
#include <string>
#include <type_traits>

#include <Stream/InSitu/CatalystAdaptor.h>
#include <catalyst.hpp>



namespace ippl{

/* in the advanced version we might want to get rid of this virtual function call */
struct CatalystAdaptor::InitVisitor {
    CatalystAdaptor& ca;

    template<class V, unsigned Dim, class... Rest>
    void operator()(const std::string& label, const ippl::Field<V, Dim, Rest...>& sf) const {
        ca.init_entry(sf, label);
    }
            
    template<class T, unsigned Dim, unsigned Dim_v, class... Rest>
    void operator()(const std::string& label, const ippl::Field<ippl::Vector<T, Dim_v>, Dim, Rest...>& vf) const {
        ca.init_entry(vf, label);
    }
    template<typename T>
    requires std::derived_from<std::decay_t<T>, ippl::ParticleBaseBase>
    void operator()(const std::string& label, const T& pc) const {
        ca.init_entry(pc, label);
    }
    // template<class S> requires std::is_arithmetic_v<S>
    // void operator()(const std::string& label, S value) const {
        // Optional: create steerable channel already at init time
        // AddSteerableChannel(value, label, node);
    //     (void)label; (void)value;
    // }
};

struct CatalystAdaptor::ExecuteVisitor {
    CatalystAdaptor& ca;

    template<class V, unsigned Dim, class... Rest>
    void operator()(const std::string& label, const ippl::Field<V, Dim, Rest...>& sf) const {
        ca.execute_entry(sf, label);
    }
    template<class T, unsigned Dim, unsigned Dim_v, class... Rest>
    void operator()(const std::string& label, const ippl::Field<ippl::Vector<T, Dim_v>, Dim, Rest...>& vf) const {
        ca.execute_entry(vf, label);
    }
    template<typename T>
    requires std::derived_from<std::decay_t<T>, ippl::ParticleBaseBase>
    void operator()(const std::string& label, const T& pc) const {
        ca.execute_entry(pc, label );
    }
};

// Initialize steerable channel
struct CatalystAdaptor::SteerInitVisitor {
    CatalystAdaptor& ca;

    template<class S> requires std::is_scalar_v<std::decay_t<S>>
    void operator()(const std::string& label, const S& value) const {
        ca.InitSteerableChannel(value, label);
    }


    template<class S> requires is_vector_v<std::decay_t<S>>
    void operator()(const std::string& label, const S& value) const {
        
        throw IpplException("CatalystAdaptor::InitSteerableChannel", "Steerable Vector has not yet been implemented " + label);
        /* pass 3 scalars??... */

    }

    template<class T>
    requires (!std::is_scalar_v<std::decay_t<T>> && !is_vector_v<std::decay_t<T>>)
    void operator()(const std::string& label , const T&) const {

        throw IpplException("CatalystAdaptor::AddSteerableChannel", "Unsupported steerable type for channel: " + label);
        
    }


};


// Forward steering: add steerable scalar channels only
struct CatalystAdaptor::SteerForwardVisitor {
    CatalystAdaptor& ca;

    template<class S> requires std::is_scalar_v<std::decay_t<S>>
    void operator()(const std::string& label, const S& value) const {
        ca.AddSteerableChannel(value, label);
    }


    template<class S> requires is_vector_v<std::decay_t<S>>
    void operator()(const std::string& label, const S& value) const {
        
        throw IpplException("CatalystAdaptor::AddSteerableChannel", "Steerable Vector has not yet been implemented " + label);
        /* pass 3 scalars??... */

    }

    template<class T>
    requires (!std::is_scalar_v<std::decay_t<T>> && !is_vector_v<std::decay_t<T>>)
    void operator()(const std::string& label , const T&) const {

        throw IpplException("CatalystAdaptor::AddSteerableChannel", "Unsupported steerable type for channel: " + label);
        
    }


};

// Backward steering fetch (mutates external scalars)
struct CatalystAdaptor::SteerFetchVisitor {
    CatalystAdaptor& ca;

    template<class S> requires std::is_arithmetic_v<std::decay_t<S>>
    void operator()(const std::string& label, S& value) const {
        ca.FetchSteerableChannelValue(value, label);
    }

    template<class T>
    requires (!std::is_arithmetic_v<std::decay_t<T>>)
    void operator()(const std::string&, const T&) const {
        /* ignore non-scalars */ 
    }
};

}



// NOTE: runtime registry currently stores by value; to mutate you need stored reference or pointer.
// If registry adjusted to store reference_wrapper<S>, update dispatch accordingly.