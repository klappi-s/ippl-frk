// File EvalFunctor.h
// Helper header defining the EvalFunctor struct
// for the FEMPoissonSolver and the PreconditionedFEMPoissonSolver.
// This EvalFunctor represents the action of the matrix A on a
// vector x for the Poisson equation in its FEM formulation Ax=b.
// This functor is passed to the LagrangeSpace in FEMPoissonSolver
// to be used for matrix-free evaluation.

#ifndef IPPL_EVALFUNCTOR_H
#define IPPL_EVALFUNCTOR_H

#include <stdexcept>
#include <string>
#include <string_view>

#include "FEM/RefShapeFunctionData.h"

namespace ippl {
    enum class PoissonStiffnessMode { Standard, ConstantPreserving };

    inline PoissonStiffnessMode parsePoissonStiffnessMode(std::string_view name) {
        if (name == "standard") return PoissonStiffnessMode::Standard;
        if (name == "constant_preserving") return PoissonStiffnessMode::ConstantPreserving;
        throw std::invalid_argument("Unknown poisson_stiffness_mode: " + std::string(name));
    }

    inline const char* poissonStiffnessModeName(PoissonStiffnessMode mode) {
        switch (mode) {
            case PoissonStiffnessMode::Standard: return "standard";
            case PoissonStiffnessMode::ConstantPreserving: return "constant_preserving";
        }
        throw std::invalid_argument("Invalid PoissonStiffnessMode");
    }

    template <typename Tlhs, unsigned Dim, unsigned numElemDOFs>
    struct EvalFunctor {
        const Vector<Tlhs, Dim> DPhiInvT;
        const Tlhs absDetDPhi;

        EvalFunctor(Vector<Tlhs, Dim> DPhiInvT, Tlhs absDetDPhi,
                    PoissonStiffnessMode mode = PoissonStiffnessMode::ConstantPreserving)
            : DPhiInvT(DPhiInvT)
            , absDetDPhi(absDetDPhi)
            , mode_m(mode) {}

        // Explicit opt-in consumed by Track B's element builder/full action.
        // Generic mass/reaction functors have no such policy.
        bool enforceZeroRowSum() const { return useCoefficientDifferences(); }
        bool useCoefficientDifferences() const {
            return mode_m == PoissonStiffnessMode::ConstantPreserving;
        }

        KOKKOS_FUNCTION auto operator()(
            const size_t& i, const size_t& j,
            const RefShapeFunctionData<Tlhs, Vector<Tlhs, Dim>, numElemDOFs>& qd) const {
            return dot((DPhiInvT * qd.deriv_q[j]), (DPhiInvT * qd.deriv_q[i])).apply() * absDetDPhi;
        }

    private:
        PoissonStiffnessMode mode_m;
    };
}

#endif
