//
// Unit test Halo
//   Test halo cell functionality and communication, as well as field layout neighbor finding
//
#include "Ippl.h"

#include "TestUtils.h"
#include "gtest/gtest.h"
#include <FEM/Entity.h>
#include <tuple>

template <typename>
class HaloTest;

template <typename T, typename ExecSpace, unsigned Dim>
class HaloTest<Parameters<T, ExecSpace, Rank<Dim>>> : public ::testing::Test {
public:
    using value_type              = T;
    using exec_space              = ExecSpace;
    constexpr static unsigned dim = Dim;

    using mesh_type      = ippl::UniformCartesian<T, Dim>;
    using centering_type = typename mesh_type::DefaultCentering;
    using layout_type    = ippl::FieldLayout<Dim>;

    using vertex_only_entitys = std::tuple<ippl::Vertex<Dim>>;
    using femcontainer_vertex_only_type = ippl::FEMContainer<T, Dim, std::tuple<ippl::Vertex<Dim>>,
                                            std::tuple<std::integral_constant<unsigned, 1>>>; // Vertex-only DOFs

    using full_entitys = std::conditional_t<Dim == 1, 
                                        std::tuple<ippl::Vertex<Dim>, ippl::EdgeX<Dim>>,
                                    std::conditional_t<Dim == 2,
                                        std::tuple<ippl::Vertex<Dim>, ippl::EdgeX<Dim>, ippl::EdgeY<Dim>, ippl::FaceXY<Dim>>,
                                    std::tuple<ippl::Vertex<Dim>, ippl::EdgeX<Dim>, ippl::EdgeY<Dim>, ippl::EdgeZ<Dim>,
                                        ippl::FaceXY<Dim>, ippl::FaceXZ<Dim>, ippl::FaceYZ<Dim>, ippl::Hexahedron<Dim>>>>;

    using full_dofnums = std::conditional_t<Dim == 1,
                                        std::tuple<std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>>,
                                    std::conditional_t<Dim == 2,
                                        std::tuple<std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>>,
                                    std::tuple<std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>,
                                        std::integral_constant<unsigned, 1>>>>;

    using femcontainer_full_type =  ippl::FEMContainer<T, Dim, full_entitys, full_dofnums>; // Full DOFs

    HaloTest()
        : nPoints(getGridSizes<Dim>()) {
        for (unsigned d = 0; d < Dim; d++) {
            domain[d] = nPoints[d] / 10;
            std::cout << "Domain size in dim " << d << ": " << nPoints[d] << std::endl;
        }

        std::array<ippl::Index, Dim> indices;
        for (unsigned d = 0; d < Dim; d++) {
            indices[d] = ippl::Index(nPoints[d]);
        }
        auto owned = std::make_from_tuple<ippl::NDIndex<Dim>>(indices);

        ippl::Vector<T, Dim> hx;
        ippl::Vector<T, Dim> origin;

        std::array<bool, Dim> isParallel;
        isParallel.fill(true);  // Specifies SERIAL, PARALLEL dims
        for (unsigned d = 0; d < Dim; d++) {
            hx[d]     = domain[d] / nPoints[d];
            origin[d] = 0;
        }

        layout = std::make_shared<layout_type>(MPI_COMM_WORLD, owned, isParallel);
        mesh   = std::make_shared<mesh_type>(owned, hx, origin);
        
        // Set up DOF numbers for FEMContainer
        // Vertex-only FEMContainer: Only DOFs on vertices (comparable to Field)
        femContainerVertexOnly = std::make_shared<femcontainer_vertex_only_type>(*mesh, *layout);
        
        // Full FEMContainer: DOFs on all element types
        femContainerFull = std::make_shared<femcontainer_full_type>(*mesh, *layout);
    }

    std::shared_ptr<mesh_type> mesh;
    std::shared_ptr<layout_type> layout;
    std::shared_ptr<femcontainer_vertex_only_type> femContainerVertexOnly;
    std::shared_ptr<femcontainer_full_type> femContainerFull;
    std::array<size_t, Dim> nPoints;
    std::array<T, Dim> domain;
};

using Tests = TestParams::tests<1, 2, 3>;
TYPED_TEST_SUITE(HaloTest, Tests);


/*
TYPED_TEST(HaloTest, CheckNeighbors) {
    int myRank = ippl::Comm->rank();
    int nRanks = ippl::Comm->size();
    
    auto& femFull = this->femContainerFull;
    
    // Collect all non-null layouts and their neighbors from FEMContainer
    std::vector<decltype(auto)> allNeighbors(femFull->getSize());
    std::vector<unsigned> validLayoutIndices;
    
    // Check all possible layouts in the FEMContainer (up to 2^Dim layouts)
    for (unsigned layoutIdx = 0; layoutIdx < femFull->getSize(); ++layoutIdx) {
        auto layout = femFull->getLayout(layoutIdx);
        if (layout != nullptr && femFull->getNumDOFs(layoutIdx) > 0) {
            allNeighbors[layoutIdx] = layout->getNeighbors();
            validLayoutIndices.push_back(layoutIdx);
        }
    }
    
    // Verify that all non-null layouts have consistent neighbor structures
    if (allNeighbors.size() > 1) {
        for (unsigned i = 1; i < allNeighbors.size(); ++i) {
            ASSERT_EQ(allNeighbors[0].size(), allNeighbors[i].size()) 
                << "Layout " << validLayoutIndices[0] << " and layout " << validLayoutIndices[i] 
                << " have different neighbor vector sizes";
            
            for (unsigned j = 0; j < allNeighbors[0].size(); ++j) {
                ASSERT_EQ(allNeighbors[0][j].size(), allNeighbors[i][j].size())
                    << "Layout " << validLayoutIndices[0] << " and layout " << validLayoutIndices[i]
                    << " have different neighbor counts at cube index " << j;
                
                for (unsigned k = 0; k < allNeighbors[0][j].size(); ++k) {
                    ASSERT_EQ(allNeighbors[0][j][k], allNeighbors[i][j][k])
                        << "Layout " << validLayoutIndices[0] << " and layout " << validLayoutIndices[i]
                        << " have different neighbors at cube index " << j << ", neighbor " << k;
                }
            }
        }
    }

    // Print neighbor information once (using the first valid layout, or fallback to main layout)
    for (int rank = 0; rank < nRanks; ++rank) {
        if (rank == myRank) {
            const auto& neighbors = !allNeighbors.empty() ? allNeighbors[0] : this->layout.getNeighbors();
            std::cout << "FEMContainer rank " << myRank << " neighbor check:" << std::endl;
            std::cout << "Found " << allNeighbors.size() << " valid layouts with consistent neighbors" << std::endl;
            
            for (unsigned i = 0; i < neighbors.size(); i++) {
                const std::vector<int>& n = neighbors[i];
                if (!n.empty()) {
                    unsigned dim = 0;
                    for (unsigned idx = i; idx > 0; idx /= 3) {
                        dim += idx % 3 == 2;
                    }
                    std::cout << "My rank is " << myRank << " and my neighbors at the";
                    switch (dim) {
                        case 0:
                            std::cout << " vertex ";
                            break;
                        case 1:
                            std::cout << " edge ";
                            break;
                        case 2:
                            std::cout << " face ";
                            break;
                        case 3:
                            std::cout << " cube ";
                            break;
                        default:
                            std::cout << ' ' << dim << "-cube ";
                            break;
                    }
                    std::cout << "with index " << i << " in " << TestFixture::dim
                              << " dimensions are: ";
                    for (const auto& nrank : n) {
                        std::cout << nrank << ' ';
                    }
                    std::cout << std::endl;
                }
            }
        }
        ippl::Comm->barrier();
    }
}
*/

TYPED_TEST(HaloTest, FillHalo) {
    // Test halo operations on vertex-only FEMContainer (should work like a Field)
    auto& femVertex = this->femContainerVertexOnly;

    std::cout << "Testing FillHalo on vertex-only FEMContainer..." << std::endl;
    *femVertex = 42;
    femVertex->fillHalo();

    // Check that all halo values are filled correctly
    std::apply([&](auto... entity_types) {
        ((void)[&]<typename EntityType>(EntityType) {
            // Process each entity type in the vertex_only_entitys tuple
            auto view = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), femVertex->template getView<EntityType>());
            nestedViewLoop(view, 0, [&]<typename... Idx>(const Idx... args) {
                unsigned int nDOFs = femVertex-> template getNumDOFs<EntityType>();
                for (unsigned int i = 0; i < nDOFs; i++) {
                    assertEqual<typename TestFixture::value_type>(view(args...)[i], 42);
                }
            });
        }(entity_types), ...);
    }, femVertex->getEntityTypes());
    
    // Test halo operations on full FEMContainer (has DOFs on vertices, edges, faces, etc.)
    auto& femFull = this->femContainerFull;
    
    std::cout << "Testing FillHalo on full FEMContainer..." << std::endl;
    *femFull = 42;
    femFull->fillHalo();

    
    // Check that all halo values are filled correctly
    std::apply([&](auto... entity_types) {
        ((void)[&]<typename EntityType>(EntityType) {
            // Process each entity type in the vertex_only_entitys tuple
            auto view = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), femFull->template getView<EntityType>());
            nestedViewLoop(view, 0, [&]<typename... Idx>(const Idx... args) {
                unsigned int nDOFs = femFull-> template getNumDOFs<EntityType>();
                for (unsigned int i = 0; i < nDOFs; i++) {
                    assertEqual<typename TestFixture::value_type>(view(args...)[i], 42);
                }
            });
        }(entity_types), ...);
    }, femFull->getEntityTypes());
}


TYPED_TEST(HaloTest, AccumulateHalo) {
    constexpr unsigned Dim = TestFixture::dim;

    auto& femVertex = this->femContainerVertexOnly;

    *femVertex = 1;
    const unsigned int nghost = femVertex->getNghost();

    std::apply([&](auto... entity_types) {
        ((void)[&]<typename EntityType>(EntityType) {

            auto& layout = femVertex->template getLayout<EntityType>();
            unsigned int nDOFs = femVertex-> template getNumDOFs<EntityType>();

            auto mirror = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), femVertex->template getView<EntityType>());

            if (ippl::Comm->size() > 1) {
                const auto& neighbors   = layout.getNeighbors();
                ippl::NDIndex<Dim> lDom = layout.getLocalNDIndex();

                auto arrayToCube = [Dim]<size_t... Dims>(const std::index_sequence<Dims...>&,
                                                    const std::array<ippl::e_cube_tag, Dim>& tags) {
                    return ippl::detail::getCube<Dim>(tags[Dims]...);
                };
                auto indexToTags = [&]<size_t... Dims, typename... Tag>(const std::index_sequence<Dims...>&,
                                                                        Tag... tags) {
                    return std::array<ippl::e_cube_tag, Dim>{(tags == nghost ? ippl::LOWER
                                                            : tags == lDom[Dims].length() + nghost - 1
                                                                ? ippl::UPPER
                                                                : ippl::IS_PARALLEL)...};
                };

                nestedViewLoop(mirror, nghost, [&]<typename... Idx>(const Idx... args) {
                    auto encoding = indexToTags(std::make_index_sequence<Dim>{}, args...);
                    auto cube     = arrayToCube(std::make_index_sequence<Dim>{}, encoding);

                    // ignore all interior points
                    if (cube == ippl::detail::countHypercubes(Dim) - 1) {
                        return;
                    }

                    unsigned int n = 0;
                    nestedLoop<Dim>(
                        [&](unsigned dl) -> size_t {
                            return encoding[dl] == ippl::IS_PARALLEL ? 0 : (encoding[dl] + 1) * 10;
                        },
                        [&](unsigned dl) -> size_t {
                            return encoding[dl] == ippl::IS_PARALLEL ? 1 : (encoding[dl] + 1) * 10 + 2;
                        },
                        [&]<typename... Flag>(const Flag... flags) {
                            auto adjacent = ippl::detail::getCube<Dim>(
                                (flags == 0   ? ippl::IS_PARALLEL
                                : flags < 20 ? (flags & 1 ? ippl::LOWER : ippl::IS_PARALLEL)
                                            : (flags & 1 ? ippl::UPPER : ippl::IS_PARALLEL))...);
                            if (adjacent == ippl::detail::countHypercubes(Dim) - 1) {
                                return;
                            }
                            n += neighbors[adjacent].size();
                        });

                    if (n > 0) {
                        for(unsigned int i = 0; i < nDOFs; i++) {
                            mirror(args...)[i] = 1. / (n + 1);
                        }
                    }
                });
                Kokkos::deep_copy(femVertex->template getView<EntityType>(), mirror);
            }

            femVertex->fillHalo();
            femVertex->accumulateHalo();

            Kokkos::deep_copy(mirror, femVertex->template getView<EntityType>());

            nestedViewLoop(mirror, nghost, [&]<typename... Idx>(const Idx... args) {
                for(unsigned int i = 0; i < nDOFs; i++) {
                    assertEqual<typename TestFixture::value_type>(mirror(args...)[i], 1);
                }
            });
        }(entity_types), ...);
    }, femVertex->getEntityTypes());
}

int main(int argc, char* argv[]) {
    int success = 1;
    ippl::initialize(argc, argv);
    {
        ::testing::InitGoogleTest(&argc, argv);
        success = RUN_ALL_TESTS();
    }
    ippl::finalize();
    return success;
}