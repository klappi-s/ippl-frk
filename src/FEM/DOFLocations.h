// DOF Locations for Finite Element Spaces
//   Provides DOF locations on reference elements for Lagrange spaces.
//
//   IMPORTANT: The DOF ordering in this file MUST match the ordering convention
//   defined in DOFHandler::fillLagrangeDOFMappingTable() (see DOFHandler.hpp).
//   Both use counter-clockwise ordering for edges, faces, and volume entities.
//
//   1D coordinates along each axis come from a node family (equispaced or GLL on [0,1]).

#ifndef IPPL_DOFLOCATIONS_H
#define IPPL_DOFLOCATIONS_H

#include "FEM/FiniteElementSpaceTraits.h"
#include "FEM/InterpolationNodeFamilies.h"
#include "Nodes1D/Nodes1D.h"
#include "Types/Vector.h"

namespace ippl {

    template <typename T, unsigned Dim, unsigned Order>
    struct LagrangeDOFLocations {
        using point_t = Vector<T, Dim>;
        using Traits  = FiniteElementSpaceTraits<LagrangeSpaceTag, Dim, Order>;
        static constexpr unsigned NumDOFs   = Traits::dofsPerElement;
        static constexpr unsigned NumNodes1D = Order + 1;

        point_t locations[NumDOFs];
        Vector<T, NumNodes1D> nodes1d_m{};

        LagrangeDOFLocations()
            : locations{} {
            setFromFamily(LagrangeNodeFamily::GLL);
        }

        /**
         * @brief Rebuild tensor-product DOF locations from @ref nodes1d_m (on [0, 1]).
         */
        KOKKOS_FUNCTION void fillFromNodes1D() {
            size_t dofIdx = 0;

            if constexpr (Dim == 1) {
                locations[dofIdx++] = point_t{nodes1d_m[0]};
                locations[dofIdx++] = point_t{nodes1d_m[Order]};
            } else if constexpr (Dim == 2) {
                locations[dofIdx++] = point_t{nodes1d_m[0], nodes1d_m[0]};
                locations[dofIdx++] = point_t{nodes1d_m[Order], nodes1d_m[0]};
                locations[dofIdx++] = point_t{nodes1d_m[Order], nodes1d_m[Order]};
                locations[dofIdx++] = point_t{nodes1d_m[0], nodes1d_m[Order]};
            } else if constexpr (Dim == 3) {
                locations[dofIdx++] = point_t{nodes1d_m[0], nodes1d_m[0], nodes1d_m[0]};
                locations[dofIdx++] = point_t{nodes1d_m[Order], nodes1d_m[0], nodes1d_m[0]};
                locations[dofIdx++] = point_t{nodes1d_m[Order], nodes1d_m[Order], nodes1d_m[0]};
                locations[dofIdx++] = point_t{nodes1d_m[0], nodes1d_m[Order], nodes1d_m[0]};
                locations[dofIdx++] = point_t{nodes1d_m[0], nodes1d_m[0], nodes1d_m[Order]};
                locations[dofIdx++] = point_t{nodes1d_m[Order], nodes1d_m[0], nodes1d_m[Order]};
                locations[dofIdx++] = point_t{nodes1d_m[Order], nodes1d_m[Order], nodes1d_m[Order]};
                locations[dofIdx++] = point_t{nodes1d_m[0], nodes1d_m[Order], nodes1d_m[Order]};
            }

            if constexpr (Order > 1) {
                if constexpr (Dim == 1) {
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[i]};
                    }
                } else if constexpr (Dim == 2) {
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[i], nodes1d_m[0]};
                    }
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[i], nodes1d_m[Order]};
                    }
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[0], nodes1d_m[i]};
                    }
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[Order], nodes1d_m[i]};
                    }
                } else if constexpr (Dim == 3) {
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[i], nodes1d_m[0], nodes1d_m[0]};
                    }
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[i], nodes1d_m[Order], nodes1d_m[0]};
                    }
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[i], nodes1d_m[0], nodes1d_m[Order]};
                    }
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[i], nodes1d_m[Order], nodes1d_m[Order]};
                    }
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[0], nodes1d_m[i], nodes1d_m[0]};
                    }
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[Order], nodes1d_m[i], nodes1d_m[0]};
                    }
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[0], nodes1d_m[i], nodes1d_m[Order]};
                    }
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[Order], nodes1d_m[i], nodes1d_m[Order]};
                    }
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[0], nodes1d_m[0], nodes1d_m[i]};
                    }
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[Order], nodes1d_m[0], nodes1d_m[i]};
                    }
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[Order], nodes1d_m[Order], nodes1d_m[i]};
                    }
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{nodes1d_m[0], nodes1d_m[Order], nodes1d_m[i]};
                    }
                }
            }

            if constexpr (Order > 1 && Dim >= 2) {
                if constexpr (Dim == 2) {
                    for (unsigned j = 1; j < Order; ++j) {
                        for (unsigned i = 1; i < Order; ++i) {
                            locations[dofIdx++] = point_t{nodes1d_m[i], nodes1d_m[j]};
                        }
                    }
                } else if constexpr (Dim == 3) {
                    for (unsigned j = 1; j < Order; ++j) {
                        for (unsigned i = 1; i < Order; ++i) {
                            locations[dofIdx++] = point_t{nodes1d_m[i], nodes1d_m[j], nodes1d_m[0]};
                        }
                    }
                    for (unsigned j = 1; j < Order; ++j) {
                        for (unsigned i = 1; i < Order; ++i) {
                            locations[dofIdx++] = point_t{nodes1d_m[i], nodes1d_m[j], nodes1d_m[Order]};
                        }
                    }
                    for (unsigned k = 1; k < Order; ++k) {
                        for (unsigned j = 1; j < Order; ++j) {
                            locations[dofIdx++] = point_t{nodes1d_m[0], nodes1d_m[j], nodes1d_m[k]};
                        }
                    }
                    for (unsigned k = 1; k < Order; ++k) {
                        for (unsigned j = 1; j < Order; ++j) {
                            locations[dofIdx++] = point_t{nodes1d_m[Order], nodes1d_m[j], nodes1d_m[k]};
                        }
                    }
                    for (unsigned k = 1; k < Order; ++k) {
                        for (unsigned i = 1; i < Order; ++i) {
                            locations[dofIdx++] = point_t{nodes1d_m[i], nodes1d_m[0], nodes1d_m[k]};
                        }
                    }
                    for (unsigned k = 1; k < Order; ++k) {
                        for (unsigned i = 1; i < Order; ++i) {
                            locations[dofIdx++] = point_t{nodes1d_m[i], nodes1d_m[Order], nodes1d_m[k]};
                        }
                    }
                }
            }

            if constexpr (Order > 1 && Dim == 3) {
                for (unsigned k = 1; k < Order; ++k) {
                    for (unsigned j = 1; j < Order; ++j) {
                        for (unsigned i = 1; i < Order; ++i) {
                            locations[dofIdx++] = point_t{nodes1d_m[i], nodes1d_m[j], nodes1d_m[k]};
                        }
                    }
                }
            }
        }

        /**
         * @brief Rebuild 1D nodes on [0, 1] from a Lagrange interpolation family, then
         * tensor-product DOF locations. Host-only (GLL uses Nodes1D). Endpoints are 0 and 1.
         */
        void setFromFamily(LagrangeNodeFamily family) {
            static_assert(NumNodes1D >= 2, "Lagrange 1D nodes require N >= 2 (Order >= 1)");
            if (family == LagrangeNodeFamily::Equispaced) {
                for (unsigned i = 0; i < NumNodes1D; ++i) {
                    nodes1d_m[i] = static_cast<T>(i) / static_cast<T>(NumNodes1D - 1);
                }
            } else {
                Vector<T, NumNodes1D> weights;
                nodes1d::computeGaussLobatto(nodes1d_m, weights);
                nodes1d_m = nodes1d::affineMapPoint(nodes1d_m, T(-1), T(1), T(0), T(1));
                nodes1d_m[0]              = T(0);
                nodes1d_m[NumNodes1D - 1] = T(1);
            }
            fillFromNodes1D();
        }

        KOKKOS_FUNCTION const point_t& operator[](size_t idx) const { return locations[idx]; }
    };

}  // namespace ippl

#endif  // IPPL_DOFLOCATIONS_H
