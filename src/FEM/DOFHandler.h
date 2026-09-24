// Class DOFHandler
//   This class is responsible for handling the degrees of freedom (DOFs) in a finite
//   element mesh and space. It provides methods to access and manipulate DOFs,
//   including mapping between local DOF indices and their global position in the FEMContainer.

#ifndef IPPL_DOFHANDLER_H
#define IPPL_DOFHANDLER_H

#include <Kokkos_Core.hpp>

#include "FEM/FiniteElementSpaceTraits.h"
#include "FEM/FEMContainer.h"
#include "Utility/IpplException.h"
#include <array>

namespace ippl {

    /**
     * @brief DOFHandler maps local element DOFs to entity types and local entity indices
     *
     * The DOFHandler's main job is to answer: "Given an element and a local DOF number,
     * which entity type does this DOF belong to, and what is the local index within that entity?"
     *
     * @tparam T The floating point type
     * @tparam SpaceTraits The finite element space traits (FiniteElementSpaceTraits<SpaceTag, Dim, Order>)
     */
    template <typename T, typename SpaceTraits_>
    class DOFHandler {
    public:
        // Space traits
        using SpaceTraits = SpaceTraits_;
        using EntityTypes = typename SpaceTraits::EntityTypes;
        using DOFNums = typename SpaceTraits::DOFNums;

        // Compatible FEMContainer type
        static constexpr unsigned Dim = SpaceTraits::Dim;
        using FEMContainer_t = FEMContainer<T, Dim, EntityTypes, DOFNums>;

        static constexpr unsigned dofsPerElement = SpaceTraits::dofsPerElement;
        static constexpr unsigned numEntityTypes = std::tuple_size_v<EntityTypes>;

        // Mesh types
        using Mesh_t = UniformCartesian<T, Dim>;
        using Layout_t = FieldLayout<Dim>;
        using indices_t = Vector<size_t, Dim>;

        /**
         * @brief Structure to hold DOF mapping information
         */
        struct DOFMapping {
            size_t entityTypeIndex;                         // Index in the EntityTypes tuple
            indices_t entityLocalIndex;                     // Offset from the NDIndex of the element to the NDIndex of the DOF (0 or 1 in each dimension)
            size_t entityLocalDOF;                          // Local DOF number within the entity
            unsigned directionMask = 0;                    // Bit d: entity extends along axis d
        };

        using DOFMapping_t = DOFMapping;

        // Kernel argument: device-accessible mappings and geometry only. The owning
        // handler, its mesh pointer and its host mirror remain on the host.
        struct DeviceStruct {
            using DOFMapping_t = DOFMapping;
            using SpaceTraits = SpaceTraits_;
            Kokkos::View<const DOFMapping*> mappings;
            indices_t numElements, localFirst;

            KOKKOS_INLINE_FUNCTION DOFMapping getElementDOFMapping(size_t i) const {
                return mappings(i);
            }
            KOKKOS_INLINE_FUNCTION indices_t getElementNDIndex(size_t index) const {
                indices_t result;
                for (unsigned d = 0; d < Dim; ++d) {
                    result[d] = index % numElements[d];
                    index /= numElements[d];
                }
                return result;
            }
            KOKKOS_INLINE_FUNCTION indices_t getLocalElementNDIndex(size_t index, int ghosts) const {
                return getElementNDIndex(index) - localFirst + indices_t(ghosts);
            }
            KOKKOS_INLINE_FUNCTION bool isDOFOnBoundary(size_t element, size_t dof, unsigned d) const {
                const auto map = mappings(dof);
                if (map.directionMask & (1u << d)) return false;
                const auto coordinate = getElementNDIndex(element)[d] + map.entityLocalIndex[d];
                return coordinate == 0 || coordinate == numElements[d];
            }
            KOKKOS_INLINE_FUNCTION bool isDOFOnBoundary(size_t element, size_t dof) const {
                for (unsigned d = 0; d < Dim; ++d)
                    if (isDOFOnBoundary(element, dof, d)) return true;
                return false;
            }
            template <typename Entity>
            static constexpr size_t getEntityDOFStart() { return SpaceTraits::template getEntityDOFStart<Entity>(); }
            template <typename Entity>
            static constexpr size_t getEntityDOFEnd() { return SpaceTraits::template getEntityDOFEnd<Entity>(); }
        };

        DeviceStruct getDeviceMirror() const {
            indices_t first;
            for (unsigned d = 0; d < Dim; ++d) first[d] = lElemDom_m[d].first();
            return {dofMappingTable_m, ne_m, first};
        }

        ///////////////////////////////////////////////////////////////////////
        // Constructors ///////////////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////////////
        
        DOFHandler();
        DOFHandler(Mesh_t& mesh, const Layout_t& layout);

        void initialize(Mesh_t& mesh, const Layout_t& layout);

        ///////////////////////////////////////////////////////////////////////
        // Core DOF Mapping Functions ////////////////////////////////////////
        ///////////////////////////////////////////////////////////////////////

        /**
         * @brief Map local element DOF to entity information
         *
         * Works on both host and device. Uses host mirror on host, device view on device.
         *
         * @param localElementDOF Local DOF number within the element (0 to dofsPerElement-1)
         * @return DOFMapping containing entity type index, entity local DOF, and entity local index
         */
        KOKKOS_INLINE_FUNCTION DOFMapping getElementDOFMapping(const size_t& localElementDOF) const;

        /**
         * @brief Get element NDIndex from linear element index
         *
         * @param elementIndex Linear element index
         * @return NDIndex of the element in the mesh
         */
        KOKKOS_FUNCTION indices_t getElementNDIndex(const size_t& elementIndex) const;

        /**
         * @brief Get local element NDIndex from linear element index
         *
         * This converts a global linear element index to the local NDIndex within the
         * MPI subdomain, accounting for the local domain offset. The result is in the
         * local coordinate system of this rank.
         *
         * @param elementIndex Linear element index (global)
         * @return NDIndex of the element in the local subdomain
         */
        KOKKOS_FUNCTION indices_t getLocalElementNDIndex(const size_t& elementIndex, int nghost) const;

        /**
         * @brief Check if a DOF is on the domain boundary in a specific dimension
         *
         * Determines if a DOF within an element lies on the global domain boundary
         * in the specified dimension. This allows checking boundaries per dimension,
         * which is useful when different boundary conditions are applied in different
         * dimensions.
         *
         * @param elementIndex Linear element index (global)
         * @param localDOF Local DOF index within the element (0 to dofsPerElement-1)
         * @param dim The dimension to check (0 to Dim-1)
         * @return true if the DOF is on the boundary in the specified dimension, false otherwise
         */
        KOKKOS_FUNCTION bool isDOFOnBoundary(const size_t& elementIndex, const size_t& localDOF,
                                              const unsigned& dim) const;

        /**
         * @brief Check if a DOF is on the domain boundary in any dimension.
         *
         * Determines if a DOF within an element lies on the global domain boundary.
         *
         * @param elementIndex Linear element index (global)
         * @param localDOF Local DOF index within the element (0 to dofsPerElement-1)
         * @return true if the DOF is on the boundary, false otherwise
         */
        KOKKOS_FUNCTION bool isDOFOnBoundary(const size_t& elementIndex, const size_t& localDOF) const;

        /**
         * @brief Get the total number of elements in the mesh
         *
         * @return Total number of elements
         */
        KOKKOS_FUNCTION size_t getNumElements() const {
            size_t numElements = 1;
            for (unsigned d = 0; d < Dim; ++d) {
                numElements *= ne_m[d];
            }
            return numElements;
        }

        /**
         * @brief Get a view of local element indices for this MPI rank
         *
         * This creates a Kokkos view containing element indices for the local subdomain
         * defined by the layout. In parallel MPI execution, each rank gets only its
         * local subset of elements. Upper boundary elements are excluded to avoid
         * double-counting between ranks.
         *
         * @return Kokkos::View containing element indices for local subdomain
         */
        Kokkos::View<size_t*> getElementIndices() const;

        /**
         * @brief Get the starting local DOF index for a specific entity type
         *
         * Since DOFs are ordered by entity type (vertices, then edges, then faces, etc.),
         * this returns the first DOF index belonging to the given entity type.
         *
         * @tparam EntityType The entity type
         * @return Starting index for this entity type's DOFs
         */
        template <typename EntityType>
        static constexpr size_t getEntityDOFStart() {
            return SpaceTraits::template getEntityDOFStart<EntityType>();
        }

        /**
         * @brief Get the ending local DOF index (exclusive) for a specific entity type
         *
         * Returns one past the last DOF index for this entity type.
         * Use with getEntityDOFStart() to iterate: for (size_t i = start; i < end; ++i)
         *
         * @tparam EntityType The entity type
         * @return Ending index (exclusive) for this entity type's DOFs
         */
        template <typename EntityType>
        static constexpr size_t getEntityDOFEnd() {
            return SpaceTraits::template getEntityDOFEnd<EntityType>();
        }

        ///////////////////////////////////////////////////////////////////////
        // FEMContainer Creation /////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////////////

        const NDIndex<Dim>& getLocalElementDomain() const { return lElemDom_m; }

    private:
        ///////////////////////////////////////////////////////////////////////
        // Member Variables ///////////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////////////
        
        Mesh_t* mesh_m;

        // Number of elements in each direction
        Vector<size_t, Dim> ne_m;

        // Local element domain
        NDIndex<Dim> lElemDom_m;

        // DOF mapping table (device view)
        using DOFMappingView_t = Kokkos::View<DOFMapping*>;
        DOFMappingView_t dofMappingTable_m;

        // DOF mapping table (host mirror for host access)
        // Kokkos 5: use host_mirror_type (HostMirror requires DEPRECATED_CODE_4).
        typename DOFMappingView_t::host_mirror_type dofMappingTable_h;

        ///////////////////////////////////////////////////////////////////////
        // Space-Specific DOF Mapping Table Filling ///////////////////////////
        ///////////////////////////////////////////////////////////////////////

        /**
         * @brief Fill DOF mapping table for Lagrange elements
         */
        void fillLagrangeDOFMappingTable(typename DOFMappingView_t::host_mirror_type& hostTable) const;

        /**
         * @brief Fill DOF mapping table for Nédélec elements
         */
        void fillNedelecDOFMappingTable(typename DOFMappingView_t::host_mirror_type& hostTable) const;
    };

    ///////////////////////////////////////////////////////////////////////
    // Type Aliases for Convenience ///////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////

    template <typename T, unsigned Dim, unsigned Order>
    using LagrangeDOFHandler = DOFHandler<T, FiniteElementSpaceTraits<LagrangeSpaceTag, Dim, Order>>;

    template <typename T, unsigned Dim, unsigned Order>
    using NedelecDOFHandler = DOFHandler<T, FiniteElementSpaceTraits<NedelecSpaceTag, Dim, Order>>;

}  // namespace ippl

#include "DOFHandler.hpp"

#endif  // IPPL_DOFHANDLER_H
