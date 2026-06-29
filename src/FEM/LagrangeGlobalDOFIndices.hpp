// IPPL lexicographic global DOF indices for LagrangeSpace_wfc (matches DOFHandler / DOFLocations).
#ifndef IPPL_LAGRANGEGLOBALDOFINDICES_HPP
#define IPPL_LAGRANGEGLOBALDOFINDICES_HPP

namespace ippl {
namespace detail {

template <unsigned Dim, unsigned Order, unsigned NumDOFs, typename indices_t>
KOKKOS_FUNCTION void fillLagrangeGlobalDOFIndices(Vector<size_t, NumDOFs>& globalDOFs,
                                                 const indices_t& elementPos,
                                                 const Vector<size_t, Dim>& nr_m) {
    globalDOFs = Vector<size_t, NumDOFs>(0);

    const size_t nx = (nr_m[0] - 1) * Order + 1;
    const size_t bx = elementPos[0] * Order;
    const size_t by = (Dim >= 2) ? elementPos[1] * Order : 0;
    const size_t bz = (Dim >= 3) ? elementPos[2] * Order : 0;

    auto dof_index = [&](size_t ix, size_t iy = 0, size_t iz = 0) -> size_t {
        if constexpr (Dim == 1) {
            (void)iy;
            (void)iz;
            return ix;
        } else if constexpr (Dim == 2) {
            (void)iz;
            return ix + nx * iy;
        } else {
            const size_t ny = (nr_m[1] - 1) * Order + 1;
            return ix + nx * (iy + ny * iz);
        }
    };

    size_t li = 0;

    if constexpr (Dim == 1) {
        globalDOFs[li++] = dof_index(bx);
        globalDOFs[li++] = dof_index(bx + Order);
        if constexpr (Order > 1) {
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx + i);
            }
        }
    } else if constexpr (Dim == 2) {
        globalDOFs[li++] = dof_index(bx, by);
        globalDOFs[li++] = dof_index(bx + Order, by);
        globalDOFs[li++] = dof_index(bx + Order, by + Order);
        globalDOFs[li++] = dof_index(bx, by + Order);
        if constexpr (Order > 1) {
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx + i, by);
            }
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx + i, by + Order);
            }
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx, by + i);
            }
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx + Order, by + i);
            }
            for (unsigned j = 1; j < Order; ++j) {
                for (unsigned i = 1; i < Order; ++i) {
                    globalDOFs[li++] = dof_index(bx + i, by + j);
                }
            }
        }
    } else {
        static constexpr size_t vx[8] = {0, 1, 1, 0, 0, 1, 1, 0};
        static constexpr size_t vy[8] = {0, 0, 1, 1, 0, 0, 1, 1};
        static constexpr size_t vz[8] = {0, 0, 0, 0, 1, 1, 1, 1};
        for (unsigned v = 0; v < 8; ++v) {
            globalDOFs[li++] =
                dof_index(bx + vx[v] * Order, by + vy[v] * Order, bz + vz[v] * Order);
        }
        if constexpr (Order > 1) {
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx + i, by, bz);
            }
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx + i, by + Order, bz);
            }
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx + i, by, bz + Order);
            }
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx + i, by + Order, bz + Order);
            }
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx, by + i, bz);
            }
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx + Order, by + i, bz);
            }
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx, by + i, bz + Order);
            }
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx + Order, by + i, bz + Order);
            }
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx, by, bz + i);
            }
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx + Order, by, bz + i);
            }
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx + Order, by + Order, bz + i);
            }
            for (unsigned i = 1; i < Order; ++i) {
                globalDOFs[li++] = dof_index(bx, by + Order, bz + i);
            }
            for (unsigned j = 1; j < Order; ++j) {
                for (unsigned i = 1; i < Order; ++i) {
                    globalDOFs[li++] = dof_index(bx + i, by + j, bz);
                }
            }
            for (unsigned j = 1; j < Order; ++j) {
                for (unsigned i = 1; i < Order; ++i) {
                    globalDOFs[li++] = dof_index(bx + i, by + j, bz + Order);
                }
            }
            for (unsigned k = 1; k < Order; ++k) {
                for (unsigned j = 1; j < Order; ++j) {
                    globalDOFs[li++] = dof_index(bx, by + j, bz + k);
                }
            }
            for (unsigned k = 1; k < Order; ++k) {
                for (unsigned j = 1; j < Order; ++j) {
                    globalDOFs[li++] = dof_index(bx + Order, by + j, bz + k);
                }
            }
            for (unsigned k = 1; k < Order; ++k) {
                for (unsigned i = 1; i < Order; ++i) {
                    globalDOFs[li++] = dof_index(bx + i, by, bz + k);
                }
            }
            for (unsigned k = 1; k < Order; ++k) {
                for (unsigned i = 1; i < Order; ++i) {
                    globalDOFs[li++] = dof_index(bx + i, by + Order, bz + k);
                }
            }
            for (unsigned k = 1; k < Order; ++k) {
                for (unsigned j = 1; j < Order; ++j) {
                    for (unsigned i = 1; i < Order; ++i) {
                        globalDOFs[li++] = dof_index(bx + i, by + j, bz + k);
                    }
                }
            }
        }
    }
}

}  // namespace detail
}  // namespace ippl

#endif
