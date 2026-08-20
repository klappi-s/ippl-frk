//
// Interpolation node families — ParameterList enums for interpolation DOF placement.
// Lagrange: equispaced vs GLL on [0, 1]. Other spaces (Nédélec, RT) get their own enums
// if they ever have more than one valid layout; lowest-order edge/face spaces do not.
//
#ifndef IPPL_INTERPOLATION_NODE_FAMILIES_H
#define IPPL_INTERPOLATION_NODE_FAMILIES_H

#include <algorithm>
#include <cctype>
#include <string>

namespace ippl {

    enum class LagrangeNodeFamily {
        Equispaced,
        GLL,
    };

    inline LagrangeNodeFamily parseLagrangeNodeFamily(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (s == "equispaced" || s == "uniform" || s == "equidistant") {
            return LagrangeNodeFamily::Equispaced;
        }
        // if (s == "gll" || s == "gauss_lobatto" || s == "lobatto")
        return LagrangeNodeFamily::GLL;
    }

    inline const char* toString(LagrangeNodeFamily f) {
        return (f == LagrangeNodeFamily::Equispaced) ? "equispaced" : "gll";
    }

}  // namespace ippl

#endif
