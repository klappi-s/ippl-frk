#pragma once
#include "Ippl.h"
namespace electrostatic_fem {
    template <unsigned Dim>
    class FEMParticles
        : public ippl::ParticleBase<
              ippl::ParticleSpatialLayout<double, Dim, ippl::UniformCartesian<double, Dim>>> {
        using Layout =
            ippl::ParticleSpatialLayout<double, Dim, ippl::UniformCartesian<double, Dim>>;
        Layout layout_m;

    public:
        ippl::ParticleAttrib<double> charge;
        ippl::ParticleAttrib<ippl::Vector<double, Dim>> velocity, electric;
        FEMParticles(ippl::UniformCartesian<double, Dim>& mesh, ippl::FieldLayout<Dim>& layout)
            : layout_m(layout, mesh, true, ippl::CountExchange::P2P) {
            this->initialize(layout_m);
            this->addAttribute(charge);
            this->addAttribute(velocity);
            this->addAttribute(electric);
            this->setParticleBC(ippl::BC::PERIODIC);
        }
    };
}  // namespace electrostatic_fem
