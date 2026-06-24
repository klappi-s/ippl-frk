// File with entity definitions for FEM containers

#ifndef IPPL_ENTITY_H
#define IPPL_ENTITY_H

#include <array>
#include <concepts>

namespace ippl {

    // Entity for different types of entities in the mesh (vertices, edges, faces, cells).

    // These can be used to access fields with a centering on these entities.
    template <typename Derived, unsigned Dim>
    struct Entity {
        // Entities are only defined for 1 to 3 dimensions at the moment
        static_assert(Dim >= 1 && Dim <= 3, "Dimension must be between 1 and 3");

        // Function to get the direction array of the entity
        KOKKOS_INLINE_FUNCTION constexpr std::array<bool, Dim> getDir() {
            static_assert(requires {
                { Derived::dir } -> std::convertible_to<const std::array<bool, Dim>&>;
            }, "Derived Entity class must have a static member 'dir' of type std::array<bool, Dim>");
            return Derived::dir;
        }
    };

    template <unsigned Dim>
    struct Vertex : Entity<Vertex<Dim>, Dim> {
        public:        
        static constexpr std::array<bool, Dim> dir = [] {
            std::array<bool, Dim> arr = {};
            for (unsigned i = 0; i < Dim; ++i) {
                arr[i] = false;
            }
            return arr;
        }();
    };

    template <unsigned Dim>
    struct EdgeX : Entity<EdgeX<Dim>, Dim> {
        public:
        static constexpr std::array<bool, Dim> dir = [] {
            std::array<bool, Dim> arr = {};
            arr[0] = true;
            for (unsigned i = 1; i < Dim; ++i) {
                arr[i] = false;
            }
            return arr;
        }();
    };

    template <unsigned Dim>
    struct EdgeY : Entity<EdgeY<Dim>, Dim> {
        public:
        static constexpr std::array<bool, Dim> dir = [] {
            std::array<bool, Dim> arr = {};
            for (unsigned i = 0; i < Dim; ++i) {
                arr[i] = false;
            }
            if constexpr (Dim >= 2) {
                arr[1] = true;
            }
            return arr;
        }();
    };

    template <unsigned Dim>
    struct EdgeZ : Entity<EdgeZ<Dim>, Dim> {
        public:
        static constexpr std::array<bool, Dim> dir = [] {
            std::array<bool, Dim> arr = {};
            for (unsigned i = 0; i < Dim; ++i) {
                arr[i] = false;
            }
            if constexpr (Dim >= 3) {
                arr[2] = true;
            }
            return arr;
        }();
    };

    template <unsigned Dim>
    struct FaceXY : Entity<FaceXY<Dim>, Dim> {
        public:
        static constexpr std::array<bool, Dim> dir = [] {
            static_assert(Dim >= 2, "FaceXY is only defined for Dim >= 2");

            std::array<bool, Dim> arr = {};
            arr[0] = true;
            arr[1] = true;
            for (unsigned i = 2; i < Dim; ++i) {
                arr[i] = false;
            }
            return arr;
        }();
    };

    template <unsigned Dim>
    struct FaceXZ : Entity<FaceXZ<Dim>, Dim> {
        public:
        static constexpr std::array<bool, Dim> dir = [] {
            static_assert(Dim >= 2, "FaceXZ is only defined for Dim >= 2");
            std::array<bool, Dim> arr = {};
            for (unsigned i = 0; i < Dim; ++i) {
                    arr[i] = false;
            }
            if constexpr (Dim >= 2) {
                arr[0] = true;
            }
            if constexpr (Dim >= 3) {
                arr[2] = true;
            }
            return arr;
        }();
    };

    template <unsigned Dim>
    struct FaceYZ : Entity<FaceYZ<Dim>, Dim> {
        public:
        static constexpr std::array<bool, Dim> dir = [] {
            static_assert(Dim >= 2, "FaceYZ is only defined for Dim >= 2");
            std::array<bool, Dim> arr = {};
            for (unsigned i = 0; i < Dim; ++i) {
                arr[i] = false;
            }
            if constexpr (Dim >= 2) {
                arr[1] = true;
            }
            if constexpr (Dim >= 3) {
                arr[2] = true;
            }
            return arr;
        }();
    };

    template <unsigned Dim>
    struct Hexahedron : Entity<Hexahedron<Dim>, Dim> {
        public:
        static constexpr std::array<bool, Dim> dir = [] {
            static_assert(Dim == 3, "Hexahedron is only defined for Dim == 3");
            std::array<bool, Dim> arr = {};
            for (unsigned i = 0; i < Dim; ++i) {
                arr[i] = true;
            }
            return arr;
        }();
    };

}  // namespace ippl

#endif
