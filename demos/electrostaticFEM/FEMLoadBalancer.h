#pragma once
#include "Decomposition/OrthogonalRecursiveBisection.h"
namespace electrostatic_fem {
    template <unsigned Dim>
    class FEMLoadBalancer {
        using ScalarField = ippl::Field<double, Dim, ippl::UniformCartesian<double, Dim>, Cell>;
        // ORB is the existing repartitioning engine. Enabling it additionally needs
        // validated FEMContainer redistribution; retaining old fields is unsafe.
        ippl::OrthogonalRecursiveBisection<ScalarField> orb_m;

    public:
        double imbalance(size_t local, size_t total) const {
            size_t largest = 0;
            ippl::Comm->allreduce(local, largest, 1, std::greater<size_t>());
            return total ? double(largest) * ippl::Comm->size() / total - 1 : 0;
        }
        void repartition() {
            throw IpplException(
                "FEMLoadBalancer",
                "FEMContainer redistribution must be implemented before enabling ORB.");
        }
    };
}  // namespace electrostatic_fem
