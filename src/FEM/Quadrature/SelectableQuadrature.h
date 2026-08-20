//
// SelectableQuadrature — runtime choice of GL / GLL / midpoint 1D rules on a fixed NumNodes1D.
// Native storage interval follows each rule; getIntegrationNodes1D remaps to the caller interval.
//
#ifndef IPPL_SELECTABLE_QUADRATURE_H
#define IPPL_SELECTABLE_QUADRATURE_H

#include <algorithm>
#include <cctype>
#include <string>

#include "FEM/Quadrature/Quadrature.h"
#include "Nodes1D/Nodes1D.h"
#include "Utility/IpplException.h"

namespace ippl {

    enum class QuadratureNodeFamily {
        GaussLegendre,
        GaussLobatto,
        Midpoint,
    };

    inline QuadratureNodeFamily parseQuadratureNodeFamily(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (s == "gll" || s == "gauss_lobatto" || s == "lobatto") {
            return QuadratureNodeFamily::GaussLobatto;
        }
        if (s == "midpoint") {
            return QuadratureNodeFamily::Midpoint;
        }
        return QuadratureNodeFamily::GaussLegendre;
    }

    inline const char* toString(QuadratureNodeFamily f) {
        switch (f) {
            case QuadratureNodeFamily::GaussLobatto:
                return "gll";
            case QuadratureNodeFamily::Midpoint:
                return "midpoint";
            default:
                return "gauss_legendre";
        }
    }

    /**
     * @brief Quadrature rule whose 1D family can be switched at runtime (GL / GLL / midpoint).
     *
     * The base constructor is given Gauss-Legendre defaults; computeNodesAndWeights then
     * overwrites degree/a/b to match the selected family. GLL requires NumNodes1D >= 2
     * (checked at run time so N=1 Gauss-Legendre/midpoint instantiations still compile).
     */
    template <typename T, unsigned NumNodes1D, typename ElementType>
    class SelectableQuadrature : public Quadrature<T, NumNodes1D, ElementType> {
    public:
        using Family = QuadratureNodeFamily;

        SelectableQuadrature(const ElementType& ref_element, Family family = Family::GaussLegendre)
            : Quadrature<T, NumNodes1D, ElementType>(ref_element, 2 * NumNodes1D - 1, T(-1), T(1))
            , family_m(family) {
            setFamily(family);
        }

        Family getFamily() const { return family_m; }

        void setFamily(Family family) {
            family_m = family;
            computeNodesAndWeights();
        }

        void computeNodesAndWeights() override {
            if (family_m == Family::Midpoint) {
                this->degree_m             = 1;
                this->a_m                  = T(0);
                this->b_m                  = T(1);
                const T segment_length     = (this->b_m - this->a_m) / NumNodes1D;
                this->weights_m            = Vector<T, NumNodes1D>(segment_length);
                this->integration_nodes_m  = Vector<T, NumNodes1D>();
                for (unsigned i = 0; i < NumNodes1D; ++i) {
                    this->integration_nodes_m[i] =
                        T(0.5) * segment_length + i * segment_length + this->a_m;
                }
                return;
            }

            // GL / GLL native domain [-1, 1].
            this->a_m = T(-1);
            this->b_m = T(1);

            if (family_m == Family::GaussLobatto) {
                if constexpr (NumNodes1D < 2) {
                    throw IpplException("SelectableQuadrature::computeNodesAndWeights",
                                        "GLL quadrature requires NumNodes1D >= 2");
                } else {
                    nodes1d::computeGaussLobatto(this->integration_nodes_m, this->weights_m);
                    this->degree_m = 2 * NumNodes1D - 3;
                }
            } else {
                nodes1d::computeGaussLegendre(this->integration_nodes_m, this->weights_m);
                this->degree_m = 2 * NumNodes1D - 1;
            }
        }

    private:
        Family family_m;
    };

}  // namespace ippl

#endif
