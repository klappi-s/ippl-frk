// DOF Locations for Finite Element Spaces
//   Provides compile-time computation of DOF locations on reference elements
//   for different finite element spaces.
//
//   IMPORTANT: The DOF ordering in this file MUST match the ordering convention
//   defined in DOFHandler::fillLagrangeDOFMappingTable() (see DOFHandler.hpp).
//   Both use counter-clockwise ordering for edges, faces, and volume entities.

#ifndef IPPL_DOFLOCATIONS_H
#define IPPL_DOFLOCATIONS_H

#include "FEM/FiniteElementSpaceTraits.h"
#include "Types/Vector.h"

namespace ippl {

    // Helper to compute DOF locations for Lagrange elements at compile time
    // Ordering convention: vertices, then edges (X, Y, Z), then faces (XY, XZ, YZ), then volume
    // All entities follow counter-clockwise numbering (see DOFHandler.hpp for details)
    template <typename T, unsigned Dim, unsigned Order>
    struct LagrangeDOFLocations {
        using point_t = Vector<T, Dim>;
        using Traits = FiniteElementSpaceTraits<LagrangeSpaceTag, Dim, Order>;
        static constexpr unsigned NumDOFs = Traits::dofsPerElement;

        // Storage for all DOF locations on the reference element [0,1]^Dim
        point_t locations[NumDOFs];

        KOKKOS_FUNCTION LagrangeDOFLocations() : locations{} {
            // Initialize all locations
            size_t dofIdx = 0;

            // Vertex DOFs (at corners of the reference element)
            if constexpr (Dim == 1) {
                // 2 vertices: x=0, x=1
                locations[dofIdx++] = point_t{0.0};
                locations[dofIdx++] = point_t{1.0};
            } else if constexpr (Dim == 2) {
                // 4 vertices: (0,0), (1,0), (1,1), (0,1)
                locations[dofIdx++] = point_t{0.0, 0.0};
                locations[dofIdx++] = point_t{1.0, 0.0};
                locations[dofIdx++] = point_t{1.0, 1.0};
                locations[dofIdx++] = point_t{0.0, 1.0};
            } else if constexpr (Dim == 3) {
                // 8 vertices: standard cube corners
                locations[dofIdx++] = point_t{0.0, 0.0, 0.0};
                locations[dofIdx++] = point_t{1.0, 0.0, 0.0};
                locations[dofIdx++] = point_t{1.0, 1.0, 0.0};
                locations[dofIdx++] = point_t{0.0, 1.0, 0.0};
                locations[dofIdx++] = point_t{0.0, 0.0, 1.0};
                locations[dofIdx++] = point_t{1.0, 0.0, 1.0};
                locations[dofIdx++] = point_t{1.0, 1.0, 1.0};
                locations[dofIdx++] = point_t{0.0, 1.0, 1.0};
            }

            // Edge DOFs (for Order > 1)
            // These are equally spaced interior points along element edges
            if constexpr (Order > 1) {
                if constexpr (Dim == 1) {
                    // 1D: Interior points along the single edge
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{static_cast<T>(i) / static_cast<T>(Order)};
                    }
                } else if constexpr (Dim == 2) {
                    // 2D: EdgeX DOFs (horizontal edges: y=0 and y=1)
                    // Bottom edge (y=0): from vertex 0 to vertex 1
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{static_cast<T>(i) / static_cast<T>(Order), 0.0};
                    }
                    // Top edge (y=1): from vertex 3 to vertex 2
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{static_cast<T>(i) / static_cast<T>(Order), 1.0};
                    }

                    // 2D: EdgeY DOFs (vertical edges: x=0 and x=1)
                    // Left edge (x=0): from vertex 0 to vertex 3
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{0.0, static_cast<T>(i) / static_cast<T>(Order)};
                    }
                    // Right edge (x=1): from vertex 1 to vertex 2
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{1.0, static_cast<T>(i) / static_cast<T>(Order)};
                    }
                } else if constexpr (Dim == 3) {
                    // 3D: EdgeX DOFs (horizontal edges: y=0, z=0 and y=1, z=0 and y=1, z=1 and y=0, z=1)
                    // Bottom front edge (y=0, z=0): from vertex 0 to vertex 1
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{static_cast<T>(i) / static_cast<T>(Order), 0.0, 0.0};
                    }
                    // Bottom back edge (y=1, z=0): from vertex 3 to vertex 2
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{static_cast<T>(i) / static_cast<T>(Order), 1.0, 0.0};
                    }
                    // Top back edge (y=1, z=1): from vertex 7 to vertex 6
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{static_cast<T>(i) / static_cast<T>(Order), 1.0, 1.0};
                    }
                    // Top front edge (y=0, z=1): from vertex 4 to vertex 5
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{static_cast<T>(i) / static_cast<T>(Order), 0.0, 1.0};
                    }

                    // 3D: EdgeY DOFs (horizontal edges: x=0, z=0 and x=1, z=0 and x=1, z=1 and x=0, z=1)
                    // Left edge (x=0, z=0): from vertex 0 to vertex 3
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{0.0, static_cast<T>(i) / static_cast<T>(Order), 0.0};
                    }
                    // Right edge (x=1, z=0): from vertex 1 to vertex 2
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{1.0, static_cast<T>(i) / static_cast<T>(Order), 0.0};
                    }
                    // Right edge (x=1, z=1): from vertex 5 to vertex 6
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{1.0, static_cast<T>(i) / static_cast<T>(Order), 1.0};
                    }
                    // Left edge (x=0, z=1): from vertex 4 to vertex 7
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{0.0, static_cast<T>(i) / static_cast<T>(Order), 1.0};
                    }

                    // 3D: EdgeZ DOFs (vertical edges: x=0, y=0 and x=1, y=0 and x=1, y=1 and x=0, y=1)
                    // Front left edge (x=0, y=0): from vertex 0 to vertex 4
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{0.0, 0.0, static_cast<T>(i) / static_cast<T>(Order)};
                    }
                    // Front right edge (x=1, y=0): from vertex 1 to vertex 5
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{1.0, 0.0, static_cast<T>(i) / static_cast<T>(Order)};
                    }
                    // Back right edge (x=1, y=1): from vertex 2 to vertex 6
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{1.0, 1.0, static_cast<T>(i) / static_cast<T>(Order)};
                    }
                    // Back left edge (x=0, y=1): from vertex 3 to vertex 7
                    for (unsigned i = 1; i < Order; ++i) {
                        locations[dofIdx++] = point_t{0.0, 1.0, static_cast<T>(i) / static_cast<T>(Order)};
                    }
                }
            }

            // Face DOFs (for Order > 1 and Dim >= 2)
            // These are interior points on element faces
            if constexpr (Order > 1 && Dim >= 2) {
                if constexpr (Dim == 2) {
                    // 2D: Interior face DOFs (the face is the element itself)
                    for (unsigned j = 1; j < Order; ++j) {
                        for (unsigned i = 1; i < Order; ++i) {
                            locations[dofIdx++] = point_t{
                                static_cast<T>(i) / static_cast<T>(Order),
                                static_cast<T>(j) / static_cast<T>(Order)
                            };
                        }
                    }
                } else if constexpr (Dim == 3) {
                    // 3D face DOFs (6 faces for a hexahedron)
                    // Ordering matches DOFHandler: FaceXY (2), FaceXZ (2), FaceYZ (2)

                    // FaceXY (perpendicular to Z-axis)
                    // Face at z=0 (bottom)
                    for (unsigned j = 1; j < Order; ++j) {
                        for (unsigned i = 1; i < Order; ++i) {
                            locations[dofIdx++] = point_t{
                                static_cast<T>(i) / static_cast<T>(Order),
                                static_cast<T>(j) / static_cast<T>(Order),
                                0.0
                            };
                        }
                    }
                    // Face at z=1 (top)
                    for (unsigned j = 1; j < Order; ++j) {
                        for (unsigned i = 1; i < Order; ++i) {
                            locations[dofIdx++] = point_t{
                                static_cast<T>(i) / static_cast<T>(Order),
                                static_cast<T>(j) / static_cast<T>(Order),
                                1.0
                            };
                        }
                    }

                    // FaceXZ (perpendicular to Y-axis)
                    // Face at y=0 (front)
                    for (unsigned k = 1; k < Order; ++k) {
                        for (unsigned i = 1; i < Order; ++i) {
                            locations[dofIdx++] = point_t{
                                static_cast<T>(i) / static_cast<T>(Order),
                                0.0,
                                static_cast<T>(k) / static_cast<T>(Order)
                            };
                        }
                    }
                    // Face at y=1 (back)
                    for (unsigned k = 1; k < Order; ++k) {
                        for (unsigned i = 1; i < Order; ++i) {
                            locations[dofIdx++] = point_t{
                                static_cast<T>(i) / static_cast<T>(Order),
                                1.0,
                                static_cast<T>(k) / static_cast<T>(Order)
                            };
                        }
                    }

                    // FaceYZ (perpendicular to X-axis)
                    // Face at x=0 (left)
                    for (unsigned k = 1; k < Order; ++k) {
                        for (unsigned j = 1; j < Order; ++j) {
                            locations[dofIdx++] = point_t{
                                0.0,
                                static_cast<T>(j) / static_cast<T>(Order),
                                static_cast<T>(k) / static_cast<T>(Order)
                            };
                        }
                    }
                    // Face at x=1 (right)
                    for (unsigned k = 1; k < Order; ++k) {
                        for (unsigned j = 1; j < Order; ++j) {
                            locations[dofIdx++] = point_t{
                                1.0,
                                static_cast<T>(j) / static_cast<T>(Order),
                                static_cast<T>(k) / static_cast<T>(Order)
                            };
                        }
                    }
                }
            }

            // Volume DOFs (for Order > 1 and Dim == 3)
            if constexpr (Order > 1 && Dim == 3) {
                // 3D volume DOFs (interior points within the hexahedron)
                for (unsigned k = 1; k < Order; ++k) {
                    for (unsigned j = 1; j < Order; ++j) {
                        for (unsigned i = 1; i < Order; ++i) {
                            locations[dofIdx++] = point_t{
                                static_cast<T>(i) / static_cast<T>(Order),
                                static_cast<T>(j) / static_cast<T>(Order),
                                static_cast<T>(k) / static_cast<T>(Order)
                            };
                        }
                    }
                }
            }
        }

        // Access operator
        KOKKOS_FUNCTION const point_t& operator[](size_t idx) const {
            return locations[idx];
        }
    };

}  // namespace ippl

#endif  // IPPL_DOFLOCATIONS_H