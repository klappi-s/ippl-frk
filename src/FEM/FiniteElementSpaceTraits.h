// Finite Element Space Traits
//   Defines compile-time information about different finite element spaces
//   including entity types, DOF counts, and mapping information.

#ifndef IPPL_FINITEELEMENTSPACETRAITS_H
#define IPPL_FINITEELEMENTSPACETRAITS_H

#include "FEM/Entity.h"
#include "FEM/FEMHelperStructs.h"
#include <tuple>
#include <type_traits>

namespace ippl {

    // Forward declarations of space tags
    struct LagrangeSpaceTag {};
    struct NedelecSpaceTag {};

    // Helper template to compute entity count per element based on entity type and dimension
    template <typename EntityType, unsigned Dim>
    struct EntityCountPerElement;

    // Vertices: 2^Dim
    template <unsigned Dim>
    struct EntityCountPerElement<Vertex<Dim>, Dim> {
        static constexpr size_t value = (1 << Dim);  // 2^Dim
    };

    // Edges in 1D: 1 edge
    template <>
    struct EntityCountPerElement<EdgeX<1>, 1> {
        static constexpr size_t value = 1;
    };

    // Edges in 2D: 2 edges per direction
    template <>
    struct EntityCountPerElement<EdgeX<2>, 2> {
        static constexpr size_t value = 2;
    };
    template <>
    struct EntityCountPerElement<EdgeY<2>, 2> {
        static constexpr size_t value = 2;
    };

    // Edges in 3D: 4 edges per direction
    template <>
    struct EntityCountPerElement<EdgeX<3>, 3> {
        static constexpr size_t value = 4;
    };
    template <>
    struct EntityCountPerElement<EdgeY<3>, 3> {
        static constexpr size_t value = 4;
    };
    template <>
    struct EntityCountPerElement<EdgeZ<3>, 3> {
        static constexpr size_t value = 4;
    };

    // Faces in 2D: 1 face (the element itself)
    template <>
    struct EntityCountPerElement<FaceXY<2>, 2> {
        static constexpr size_t value = 1;
    };

    // Faces in 3D: 2 faces per orientation
    template <>
    struct EntityCountPerElement<FaceXY<3>, 3> {
        static constexpr size_t value = 2;
    };
    template <>
    struct EntityCountPerElement<FaceXZ<3>, 3> {
        static constexpr size_t value = 2;
    };
    template <>
    struct EntityCountPerElement<FaceYZ<3>, 3> {
        static constexpr size_t value = 2;
    };

    // Hexahedron in 3D: 1 per element (the element itself)
    template <>
    struct EntityCountPerElement<Hexahedron<3>, 3> {
        static constexpr size_t value = 1;
    };

    // Helper base class with common functionality
    template <typename SpaceTag_, typename EntityTypes_, typename DOFNums_, unsigned Dim_, unsigned Order_>
    struct FiniteElementSpaceTraitsBase {
        using SpaceTag = SpaceTag_;
        using EntityTypes = EntityTypes_;
        using DOFNums = DOFNums_;
        static constexpr unsigned Dim = Dim_;
        static constexpr unsigned Order = Order_;

        // Extract DOF count from the DOFNums tuple at compile time
        template <typename EntityType>
        static constexpr size_t entityDOFCount() {
            constexpr size_t index = TagIndex<EntityTypes>::template index<EntityType>();
            return std::tuple_element_t<index, DOFNums>::value;
        }

        // Helper to compute cumulative DOF offset for entities before a given index
        template <size_t TargetIndex, size_t CurrentIndex = 0>
        static constexpr size_t getCumulativeDOFCount() {
            if constexpr (CurrentIndex >= TargetIndex) {
                return 0;
            } else {
                using CurrentEntityType = std::tuple_element_t<CurrentIndex, EntityTypes>;
                constexpr size_t currentDOFsPerEntity = std::tuple_element_t<CurrentIndex, DOFNums>::value;
                constexpr size_t currentEntityCount = EntityCountPerElement<CurrentEntityType, Dim>::value;
                constexpr size_t currentTotal = currentEntityCount * currentDOFsPerEntity;
                return currentTotal + getCumulativeDOFCount<TargetIndex, CurrentIndex + 1>();
            }
        }

        // Get the starting local DOF index for a specific entity type
        template <typename EntityType>
        static constexpr size_t getEntityDOFStart() {
            // If entity doesn't exist in this space, return 0
            if constexpr (!TagIndex<EntityTypes>::template contains<EntityType>()) {
                return 0;
            } else {
                constexpr size_t index = TagIndex<EntityTypes>::template index<EntityType>();
                return getCumulativeDOFCount<index>();
            }
        }

        // Get the ending local DOF index (exclusive) for a specific entity type
        template <typename EntityType>
        static constexpr size_t getEntityDOFEnd() {
            // If entity doesn't exist in this space, return 0
            if constexpr (!TagIndex<EntityTypes>::template contains<EntityType>()) {
                return 0;
            } else {
                constexpr size_t index = TagIndex<EntityTypes>::template index<EntityType>();
                constexpr size_t dofCount = std::tuple_element_t<index, DOFNums>::value;
                constexpr size_t entityCount = EntityCountPerElement<EntityType, Dim>::value;
                return getEntityDOFStart<EntityType>() + (entityCount * dofCount);
            }
        }
    };

    // Primary template - this will cause a compilation error if used directly
    template <typename SpaceTag, unsigned Dim, unsigned Order>
    struct FiniteElementSpaceTraits;


    ///////////////////////////////////////////////////////////////////////
    // Lagrange Space Traits //////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////

    // Lagrange 1D Order 1 (special case - only vertices)
    template <>
    struct FiniteElementSpaceTraits<LagrangeSpaceTag, 1, 1>
        : FiniteElementSpaceTraitsBase<LagrangeSpaceTag,
                                        std::tuple<Vertex<1>>,
                                        std::tuple<std::integral_constant<unsigned, 1>>,
                                        1, 1> {
        static constexpr unsigned dofsPerElement = 2 * 1;  // 2 vertices * 1 DOF each
    };

    // Lagrange 1D Order >= 2 (general case)
    template <unsigned Order>
    struct FiniteElementSpaceTraits<LagrangeSpaceTag, 1, Order>
        : FiniteElementSpaceTraitsBase<LagrangeSpaceTag,
                                        std::tuple<Vertex<1>, EdgeX<1>>,
                                        std::tuple<std::integral_constant<unsigned, 1>,
                                                   std::integral_constant<unsigned, Order-1>>,
                                        1, Order> {
        static_assert(Order >= 2, "Use Order 1 specialization for first-Order elements");
        // 2 vertices * 1 DOF + 1 edge * (Order-1) DOFs
        static constexpr unsigned dofsPerElement = 2 * 1 + 1 * (Order-1);
    };

    // Lagrange 2D Order 1 (special case - only vertices)
    template <>
    struct FiniteElementSpaceTraits<LagrangeSpaceTag, 2, 1>
        : FiniteElementSpaceTraitsBase<LagrangeSpaceTag,
                                        std::tuple<Vertex<2>>,
                                        std::tuple<std::integral_constant<unsigned, 1>>,
                                        2, 1> {
        static constexpr unsigned dofsPerElement = 4 * 1;  // 4 vertices * 1 DOF each
    };

    // Lagrange 2D Order >= 2 (general case)
    template <unsigned Order>
    struct FiniteElementSpaceTraits<LagrangeSpaceTag, 2, Order>
        : FiniteElementSpaceTraitsBase<LagrangeSpaceTag,
                                        std::tuple<Vertex<2>, EdgeX<2>, EdgeY<2>, FaceXY<2>>,
                                        std::tuple<std::integral_constant<unsigned, 1>,
                                                   std::integral_constant<unsigned, Order-1>,
                                                   std::integral_constant<unsigned, Order-1>,
                                                   std::integral_constant<unsigned, (Order-1)*(Order-1)>>,
                                        2, Order> {
        static_assert(Order >= 2, "Use Order 1 specialization for first-Order elements");
        // 4 vertices * 1 DOF + 2 EdgesX * (Order-1) DOFs + 2 EdgesY * (Order-1) DOFs + 1 face * (Order-1)^2 DOFs
        static constexpr unsigned dofsPerElement = 4 * 1 + 2 * (Order-1) + 2 * (Order-1) + 1 * (Order-1) * (Order-1);
    };

    // Lagrange 3D Order 1 (special case - only vertices)
    template <>
    struct FiniteElementSpaceTraits<LagrangeSpaceTag, 3, 1>
        : FiniteElementSpaceTraitsBase<LagrangeSpaceTag,
                                        std::tuple<Vertex<3>>,
                                        std::tuple<std::integral_constant<unsigned, 1>>,
                                        3, 1> {
        static constexpr unsigned dofsPerElement = 8 * 1;  // 8 vertices * 1 DOF each
    };

    // Lagrange 3D Order >= 2 (general case)
    template <unsigned Order>
    struct FiniteElementSpaceTraits<LagrangeSpaceTag, 3, Order>
        : FiniteElementSpaceTraitsBase<LagrangeSpaceTag,
                                        std::tuple<Vertex<3>, EdgeX<3>, EdgeY<3>, EdgeZ<3>,
                                                  FaceXY<3>, FaceXZ<3>, FaceYZ<3>, Hexahedron<3>>,
                                        std::tuple<std::integral_constant<unsigned, 1>,
                                                   std::integral_constant<unsigned, Order-1>,
                                                   std::integral_constant<unsigned, Order-1>,
                                                   std::integral_constant<unsigned, Order-1>,
                                                   std::integral_constant<unsigned, (Order-1)*(Order-1)>,
                                                   std::integral_constant<unsigned, (Order-1)*(Order-1)>,
                                                   std::integral_constant<unsigned, (Order-1)*(Order-1)>,
                                                   std::integral_constant<unsigned, (Order-1)*(Order-1)*(Order-1)>>,
                                        3, Order> {
        static_assert(Order >= 2, "Use Order 1 specialization for first-Order elements");
        // 8 vertices * 1 DOF + 4 EdgesX * (Order-1) + 4 EdgesY * (Order-1) + 4 EdgesZ * (Order-1)
        // + 2 FacesXY * (Order-1)^2 + 2 FacesXZ * (Order-1)^2 + 2 FacesYZ * (Order-1)^2 + 1 volume * (Order-1)^3
        static constexpr unsigned dofsPerElement = 8 * 1 + 4 * (Order-1) + 4 * (Order-1) + 4 * (Order-1)
                                                   + 2 * (Order-1) * (Order-1) + 2 * (Order-1) * (Order-1) + 2 * (Order-1) * (Order-1)
                                                   + 1 * (Order-1) * (Order-1) * (Order-1);
    };

}  // namespace ippl

#endif  // IPPL_FINITEELEMENTSPACETRAITS_H