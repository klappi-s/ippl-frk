#ifndef IPPL_FEM_VALIDATION_EXPLICITTOOLS_HPP
#define IPPL_FEM_VALIDATION_EXPLICITTOOLS_HPP

#include <cmath>
#include <functional>
#include <vector>

#include <mpi.h>

#include "Utility/ViewUtils.h"

namespace ippl::femvalidate {

template <typename LagrangeType>
std::vector<std::size_t> ownedElementIndicesHost(const LagrangeType& space) {
    const auto elemIndices = space.getDOFHandler().getElementIndices();
    auto host              = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), elemIndices);
    return std::vector<std::size_t>(host.data(), host.data() + host.extent(0));
}

namespace detail {

template <typename Field, typename EntityType, unsigned Dim>
double readViewDOF(const Field& field, const ippl::Vector<std::size_t, Dim>& ndIndex,
                   std::size_t localDof) {
    auto view = field.template getView<EntityType>();
    auto host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), view);
    if constexpr (Dim == 1) {
        return host(static_cast<int>(ndIndex[0]))[localDof];
    } else if constexpr (Dim == 2) {
        return host(static_cast<int>(ndIndex[0]), static_cast<int>(ndIndex[1]))[localDof];
    } else {
        return host(static_cast<int>(ndIndex[0]), static_cast<int>(ndIndex[1]),
                   static_cast<int>(ndIndex[2]))[localDof];
    }
}

template <typename Field, typename EntityType, unsigned Dim>
void writeViewDOF(Field& field, const ippl::Vector<std::size_t, Dim>& ndIndex,
                  std::size_t localDof, double value) {
    auto view = field.template getView<EntityType>();
    auto host = Kokkos::create_mirror_view(view);
    Kokkos::deep_copy(host, view);
    if constexpr (Dim == 1) {
        host(static_cast<int>(ndIndex[0]))[localDof] = value;
    } else if constexpr (Dim == 2) {
        host(static_cast<int>(ndIndex[0]), static_cast<int>(ndIndex[1]))[localDof] = value;
    } else {
        host(static_cast<int>(ndIndex[0]), static_cast<int>(ndIndex[1]),
             static_cast<int>(ndIndex[2]))[localDof] = value;
    }
    Kokkos::deep_copy(view, host);
}

template <typename Field, typename DOFHandler_t, std::size_t... Is>
double readDOFDispatch(const Field& field, std::size_t entityTypeIndex,
                       const ippl::Vector<std::size_t, DOFHandler_t::Dim>& nd, std::size_t localDof,
                       std::index_sequence<Is...>) {
    double val = 0.0;
    ((entityTypeIndex == Is
          ? (val = readViewDOF<Field, std::tuple_element_t<Is, typename DOFHandler_t::EntityTypes>,
                              DOFHandler_t::Dim>(field, nd, localDof),
             true)
          : false) ||
     ...);
    return val;
}

template <typename Field, typename DOFHandler_t, std::size_t... Is>
void writeDOFDispatch(Field& field, std::size_t entityTypeIndex,
                      const ippl::Vector<std::size_t, DOFHandler_t::Dim>& nd, std::size_t localDof,
                      double value, std::index_sequence<Is...>) {
    ((entityTypeIndex == Is
          ? (writeViewDOF<Field, std::tuple_element_t<Is, typename DOFHandler_t::EntityTypes>,
                              DOFHandler_t::Dim>(field, nd, localDof, value),
             true)
          : false) ||
     ...);
}

template <typename Field, typename DOFHandler_t>
double readFieldDOF(const Field& field, const DOFHandler_t& dofHandler, std::size_t elementIndex,
                    std::size_t localDof, int nghost) {
    const auto dofMap = dofHandler.getElementDOFMapping(localDof);
    const auto elemND = dofHandler.getLocalElementNDIndex(elementIndex, nghost);
    const auto nd     = elemND + dofMap.entityLocalIndex;
    return readDOFDispatch<Field, DOFHandler_t>(field, dofMap.entityTypeIndex, nd,
                                                dofMap.entityLocalDOF,
                                                std::make_index_sequence<DOFHandler_t::numEntityTypes>{});
}

template <typename Field, typename DOFHandler_t>
void writeFieldDOF(Field& field, const DOFHandler_t& dofHandler, std::size_t elementIndex,
                   std::size_t localDof, int nghost, double value) {
    const auto dofMap = dofHandler.getElementDOFMapping(localDof);
    const auto elemND = dofHandler.getLocalElementNDIndex(elementIndex, nghost);
    const auto nd     = elemND + dofMap.entityLocalIndex;
    writeDOFDispatch<Field, DOFHandler_t>(field, dofMap.entityTypeIndex, nd, dofMap.entityLocalDOF,
                                          value,
                                          std::make_index_sequence<DOFHandler_t::numEntityTypes>{});
}

template <typename Field, typename DOFHandler_t>
class FieldHostWriter {
  public:
    using indices_t = ippl::Vector<std::size_t, DOFHandler_t::Dim>;

    explicit FieldHostWriter(Field& field) : field_m(field) {
        emplaceEntities(std::make_index_sequence<DOFHandler_t::numEntityTypes>{});
    }

    void write(const DOFHandler_t& dofHandler, std::size_t elementIndex, std::size_t localDof,
               int nghost, double value) {
        const auto dofMap = dofHandler.getElementDOFMapping(localDof);
        const auto elemND = dofHandler.getLocalElementNDIndex(elementIndex, nghost);
        const indices_t nd = elemND + dofMap.entityLocalIndex;
        writers_[dofMap.entityTypeIndex](nd, dofMap.entityLocalDOF, value);
    }

    void syncToDevice() {
        for (const auto& fn : sync_fns_) {
            fn();
        }
    }

  private:
    Field& field_m;
    std::vector<std::function<void(const indices_t&, std::size_t, double)>> writers_;
    std::vector<std::function<void()>> sync_fns_;

    template <typename EntityType>
    void emplaceEntity() {
        using View_t         = std::remove_cv_t<
            std::remove_reference_t<decltype(field_m.template getView<EntityType>())>>;
        using HostMirror_t   = typename View_t::HostMirror;
        auto host            = Kokkos::create_mirror_view(field_m.template getView<EntityType>());
        Kokkos::deep_copy(host, field_m.template getView<EntityType>());
        auto host_ptr = std::make_shared<HostMirror_t>(std::move(host));

        writers_.emplace_back([host_ptr](const indices_t& nd, std::size_t localDof, double value) {
            if constexpr (DOFHandler_t::Dim == 1) {
                (*host_ptr)(static_cast<int>(nd[0]))[localDof] = value;
            } else if constexpr (DOFHandler_t::Dim == 2) {
                (*host_ptr)(static_cast<int>(nd[0]), static_cast<int>(nd[1]))[localDof] = value;
            } else {
                (*host_ptr)(static_cast<int>(nd[0]), static_cast<int>(nd[1]),
                             static_cast<int>(nd[2]))[localDof] = value;
            }
        });
        sync_fns_.emplace_back([this, host_ptr]() {
            Kokkos::deep_copy(field_m.template getView<EntityType>(), *host_ptr);
        });
    }

    template <std::size_t... Is>
    void emplaceEntities(std::index_sequence<Is...>) {
        (emplaceEntity<std::tuple_element_t<Is, typename DOFHandler_t::EntityTypes>>(), ...);
    }
};

template <typename Field, typename DOFHandler_t>
class FieldHostReader {
  public:
    using indices_t = ippl::Vector<std::size_t, DOFHandler_t::Dim>;

    explicit FieldHostReader(const Field& field) : field_m(field) {
        emplaceEntities(std::make_index_sequence<DOFHandler_t::numEntityTypes>{});
    }

    double read(const DOFHandler_t& dofHandler, std::size_t elementIndex, std::size_t localDof,
                int nghost) const {
        const auto dofMap = dofHandler.getElementDOFMapping(localDof);
        const auto elemND = dofHandler.getLocalElementNDIndex(elementIndex, nghost);
        const indices_t nd = elemND + dofMap.entityLocalIndex;
        return readers_[dofMap.entityTypeIndex](nd, dofMap.entityLocalDOF);
    }

  private:
    const Field& field_m;
    std::vector<std::function<double(const indices_t&, std::size_t)>> readers_;

    template <typename EntityType>
    void emplaceEntity() {
        auto host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(),
                                                        field_m.template getView<EntityType>());
        auto host_ptr = std::make_shared<decltype(host)>(std::move(host));

        readers_.emplace_back([host_ptr](const indices_t& nd, std::size_t localDof) {
            if constexpr (DOFHandler_t::Dim == 1) {
                return (*host_ptr)(static_cast<int>(nd[0]))[localDof];
            } else if constexpr (DOFHandler_t::Dim == 2) {
                return (*host_ptr)(static_cast<int>(nd[0]), static_cast<int>(nd[1]))[localDof];
            } else {
                return (*host_ptr)(static_cast<int>(nd[0]), static_cast<int>(nd[1]),
                                   static_cast<int>(nd[2]))[localDof];
            }
        });
    }

    template <std::size_t... Is>
    void emplaceEntities(std::index_sequence<Is...>) {
        (emplaceEntity<std::tuple_element_t<Is, typename DOFHandler_t::EntityTypes>>(), ...);
    }
};

template <typename Field, typename DOFHandler_t>
class FieldHostCache {
  public:
    using indices_t = ippl::Vector<std::size_t, DOFHandler_t::Dim>;

    explicit FieldHostCache(Field& field) : field_m(field) {
        emplaceEntities(std::make_index_sequence<DOFHandler_t::numEntityTypes>{});
    }

    double read(const DOFHandler_t& dofHandler, std::size_t elementIndex, std::size_t localDof,
                int nghost) const {
        const auto dofMap = dofHandler.getElementDOFMapping(localDof);
        const auto elemND = dofHandler.getLocalElementNDIndex(elementIndex, nghost);
        const indices_t nd = elemND + dofMap.entityLocalIndex;
        return readers_[dofMap.entityTypeIndex](nd, dofMap.entityLocalDOF);
    }

    void write(const DOFHandler_t& dofHandler, std::size_t elementIndex, std::size_t localDof,
               int nghost, double value) {
        const auto dofMap = dofHandler.getElementDOFMapping(localDof);
        const auto elemND = dofHandler.getLocalElementNDIndex(elementIndex, nghost);
        const indices_t nd = elemND + dofMap.entityLocalIndex;
        writers_[dofMap.entityTypeIndex](nd, dofMap.entityLocalDOF, value);
    }

    void syncToDevice() {
        for (const auto& fn : sync_fns_) {
            fn();
        }
    }

  private:
    Field& field_m;
    std::vector<std::function<double(const indices_t&, std::size_t)>> readers_;
    std::vector<std::function<void(const indices_t&, std::size_t, double)>> writers_;
    std::vector<std::function<void()>> sync_fns_;

    template <typename EntityType>
    void emplaceEntity() {
        using View_t       = std::remove_cv_t<
            std::remove_reference_t<decltype(field_m.template getView<EntityType>())>>;
        using HostMirror_t = typename View_t::HostMirror;
        auto host          = Kokkos::create_mirror_view(field_m.template getView<EntityType>());
        Kokkos::deep_copy(host, field_m.template getView<EntityType>());
        auto host_ptr = std::make_shared<HostMirror_t>(std::move(host));

        readers_.emplace_back([host_ptr](const indices_t& nd, std::size_t localDof) {
            if constexpr (DOFHandler_t::Dim == 1) {
                return (*host_ptr)(static_cast<int>(nd[0]))[localDof];
            } else if constexpr (DOFHandler_t::Dim == 2) {
                return (*host_ptr)(static_cast<int>(nd[0]), static_cast<int>(nd[1]))[localDof];
            } else {
                return (*host_ptr)(static_cast<int>(nd[0]), static_cast<int>(nd[1]),
                                   static_cast<int>(nd[2]))[localDof];
            }
        });
        writers_.emplace_back([host_ptr](const indices_t& nd, std::size_t localDof, double value) {
            if constexpr (DOFHandler_t::Dim == 1) {
                (*host_ptr)(static_cast<int>(nd[0]))[localDof] = value;
            } else if constexpr (DOFHandler_t::Dim == 2) {
                (*host_ptr)(static_cast<int>(nd[0]), static_cast<int>(nd[1]))[localDof] = value;
            } else {
                (*host_ptr)(static_cast<int>(nd[0]), static_cast<int>(nd[1]),
                             static_cast<int>(nd[2]))[localDof] = value;
            }
        });
        sync_fns_.emplace_back([this, host_ptr]() {
            Kokkos::deep_copy(field_m.template getView<EntityType>(), *host_ptr);
        });
    }

    template <std::size_t... Is>
    void emplaceEntities(std::index_sequence<Is...>) {
        (emplaceEntity<std::tuple_element_t<Is, typename DOFHandler_t::EntityTypes>>(), ...);
    }
};

template <typename LagrangeType>
std::vector<std::vector<double>> assembleGlobalStiffness(
    const LagrangeType& space, const std::vector<std::vector<std::vector<double>>>& cell_A) {
    static constexpr unsigned nloc = LagrangeType::numElementDOFs;
    const std::size_t ndofs        = space.numGlobalDOFs();
    std::vector<std::vector<double>> A_global(ndofs, std::vector<double>(ndofs, 0.0));

    const auto owned_elements = ownedElementIndicesHost(space);

    for (const std::size_t e : owned_elements) {
        const auto g_dofs = space.getGlobalDOFIndices(e);
        for (unsigned i = 0; i < nloc; ++i) {
            for (unsigned j = 0; j < nloc; ++j) {
                A_global[g_dofs[i]][g_dofs[j]] += cell_A[e][i][j];
            }
        }
    }

    if (ippl::Comm->size() > 1) {
        std::vector<double> flat(ndofs * ndofs, 0.0);
        for (std::size_t i = 0; i < ndofs; ++i) {
            for (std::size_t j = 0; j < ndofs; ++j) {
                flat[i * ndofs + j] = A_global[i][j];
            }
        }
        MPI_Allreduce(MPI_IN_PLACE, flat.data(), static_cast<int>(flat.size()), MPI_DOUBLE, MPI_SUM,
                      MPI_COMM_WORLD);
        for (std::size_t i = 0; i < ndofs; ++i) {
            for (std::size_t j = 0; j < ndofs; ++j) {
                A_global[i][j] = flat[i * ndofs + j];
            }
        }
    }
    return A_global;
}

template <unsigned Dim, unsigned Order>
std::vector<double> applyZeroFaceGlobal(const std::vector<std::vector<double>>& A,
                                        const std::vector<double>& v,
                                        const std::array<unsigned, Dim>& nel) {
    const std::size_t ndofs = v.size();
    std::vector<double> y(ndofs, 0.0);
    for (std::size_t i = 0; i < ndofs; ++i) {
        if (ippl::femdata::detail::is_boundary_dof<Dim>(i, Order, nel)) {
            continue;
        }
        double sum = 0.0;
        for (std::size_t j = 0; j < ndofs; ++j) {
            if (ippl::femdata::detail::is_boundary_dof<Dim>(j, Order, nel)) {
                continue;
            }
            sum += A[i][j] * v[j];
        }
        y[i] = sum;
    }
    return y;
}

template <typename LagrangeType, typename FieldType>
std::vector<double> gatherFieldToGlobal(const LagrangeType& space, const FieldType& field) {
    using DOFHandler_t = typename LagrangeType::DOFHandler_t;
    static constexpr unsigned nloc = LagrangeType::numElementDOFs;

    const std::size_t ndofs  = space.numGlobalDOFs();
    std::vector<double> g(ndofs, 0.0);
    std::vector<double> cnt(ndofs, 0.0);

    const FieldHostReader<FieldType, DOFHandler_t> reader(field);
    const int nghost               = field.getNghost();
    const auto dofHandler          = space.getDOFHandler();
    const auto owned_elements      = ownedElementIndicesHost(space);

    for (const std::size_t e : owned_elements) {
        const auto g_dofs = space.getGlobalDOFIndices(e);
        for (unsigned l = 0; l < nloc; ++l) {
            const std::size_t gid = g_dofs[l];
            g[gid] += reader.read(dofHandler, e, l, nghost);
            cnt[gid] += 1.0;
        }
    }

    if (ippl::Comm->size() > 1) {
        MPI_Allreduce(MPI_IN_PLACE, g.data(), static_cast<int>(g.size()), MPI_DOUBLE, MPI_SUM,
                      MPI_COMM_WORLD);
        MPI_Allreduce(MPI_IN_PLACE, cnt.data(), static_cast<int>(cnt.size()), MPI_DOUBLE, MPI_SUM,
                      MPI_COMM_WORLD);
    }
    for (std::size_t i = 0; i < ndofs; ++i) {
        if (cnt[i] > 0.0) {
            g[i] /= cnt[i];
        }
    }
    return g;
}

template <typename FieldType, typename EntityType, unsigned numDOFs>
void accumulateOwnedEntityDiff(const FieldType& approx, const FieldType& ref, int nghost,
                               double& err2, double& ref2) {
    auto view_a = approx.template getView<EntityType>();
    auto view_r = ref.template getView<EntityType>();
    auto host_a = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), view_a);
    auto host_r = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), view_r);

    ippl::detail::nestedViewLoop(host_a, nghost, [&](const auto&... args) {
        const auto aval = host_a(args...);
        const auto rval = host_r(args...);
        for (unsigned ld = 0; ld < numDOFs; ++ld) {
            const double d = aval[ld] - rval[ld];
            err2 += d * d;
            ref2 += rval[ld] * rval[ld];
        }
    });
}

template <typename FieldType, typename DOFHandler_t, std::size_t... Is>
void accumulateOwnedLayoutDiffImpl(const FieldType& approx, const FieldType& ref, int nghost,
                                   double& err2, double& ref2, std::index_sequence<Is...>) {
    (accumulateOwnedEntityDiff<
         FieldType, std::tuple_element_t<Is, typename DOFHandler_t::EntityTypes>,
         std::tuple_element_t<Is, typename DOFHandler_t::DOFNums>::value>(approx, ref, nghost, err2,
                                                                          ref2),
     ...);
}

template <typename FieldType, typename DOFHandler_t>
double ownedLayoutRelativeError(const FieldType& approx, const FieldType& ref) {
    const int nghost = approx.getNghost();
    double err2      = 0.0;
    double ref2      = 0.0;
    accumulateOwnedLayoutDiffImpl<FieldType, DOFHandler_t>(
        approx, ref, nghost, err2, ref2,
        std::make_index_sequence<DOFHandler_t::numEntityTypes>{});
    if (ippl::Comm->size() > 1) {
        MPI_Allreduce(MPI_IN_PLACE, &err2, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        MPI_Allreduce(MPI_IN_PLACE, &ref2, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    }
    return std::sqrt(err2) / std::max(std::sqrt(ref2), 1e-30);
}

template <typename LagrangeType>
double relativeGlobalVectorL2Error(const std::vector<double>& approx,
                                   const std::vector<double>& reference,
                                   const std::function<bool(std::size_t)>& skip_dof) {
    double err2 = 0.0;
    double ref2 = 0.0;
    const std::size_t n = reference.size();
    for (std::size_t g = 0; g < n; ++g) {
        if (skip_dof && skip_dof(g)) {
            continue;
        }
        const double d = approx[g] - reference[g];
        err2 += d * d;
        ref2 += reference[g] * reference[g];
    }
    return std::sqrt(err2) / std::max(std::sqrt(ref2), 1e-30);
}

template <typename LagrangeType, typename FieldType>
double relativeFieldNormError(const LagrangeType& space, const FieldType& approx,
                              const FieldType& reference,
                              const std::function<bool(std::size_t)>& skip_dof) {
    (void)space;
    (void)skip_dof;
    if (ippl::Comm->size() > 1) {
        using DOFHandler_t = typename LagrangeType::DOFHandler_t;
        return ownedLayoutRelativeError<FieldType, DOFHandler_t>(approx, reference);
    }
    const FieldType diff = approx - reference;
    const double ref_norm = reference.norm();
    return diff.norm() / std::max(ref_norm, 1e-30);
}

}  // namespace detail

template <typename LagrangeType, typename ElementType>
EvalFunctor<typename LagrangeType::FEMContainer_t::value_type, LagrangeType::dim,
            LagrangeType::numElementDOFs>
makePoissonEvalFunctor(const LagrangeType& space, const ElementType& ref_element) {
    using T = typename LagrangeType::FEMContainer_t::value_type;
    constexpr unsigned Dim = LagrangeType::dim;
    const ippl::Vector<std::size_t, Dim> zeroNdIndex = ippl::Vector<std::size_t, Dim>(0);
    const auto verts = space.getElementMeshVertexPoints(zeroNdIndex);
    const ippl::Vector<T, Dim> DPhiInvT = ref_element.getInverseTransposeTransformationJacobian(verts);
    const T absDet =
        Kokkos::abs(ref_element.getDeterminantOfTransformationJacobian(verts));
    return EvalFunctor<T, Dim, LagrangeType::numElementDOFs>(DPhiInvT, absDet);
}

template <typename LagrangeType, typename ElementType, typename QuadratureType>
std::vector<std::vector<std::vector<double>>> assembleCellStiffnessMatrices(
    const LagrangeType& space, const ElementType& ref_element, const QuadratureType& quadrature) {
    using T = typename LagrangeType::FEMContainer_t::value_type;
    constexpr unsigned Dim = LagrangeType::dim;
    static constexpr unsigned nloc = LagrangeType::numElementDOFs;

    const auto weights = quadrature.getWeightsForRefElement();
    const auto qpts    = quadrature.getIntegrationNodesForRefElement();

    std::vector<ippl::Vector<ippl::Vector<T, Dim>, nloc>> grad_q(QuadratureType::numElementNodes);
    for (unsigned k = 0; k < QuadratureType::numElementNodes; ++k) {
        for (unsigned i = 0; i < nloc; ++i) {
            grad_q[k][i] = space.evaluateRefElementShapeFunctionGradient(i, qpts[k]);
        }
    }

    const std::size_t ncells = space.numElements();
    const auto owned_elements = ownedElementIndicesHost(space);
    std::vector<std::vector<std::vector<double>>> cell_A(
        ncells, std::vector<std::vector<double>>(nloc, std::vector<double>(nloc, 0.0)));

    for (const std::size_t e : owned_elements) {
        const auto elem_nd = space.getElementNDIndex(e);
        const auto verts   = space.getElementMeshVertexPoints(elem_nd);
        const auto DPhiInvT =
            ref_element.getInverseTransposeTransformationJacobian(verts);
        const T absDet =
            Kokkos::abs(ref_element.getDeterminantOfTransformationJacobian(verts));
        EvalFunctor<T, Dim, nloc> eval_phys(DPhiInvT, absDet);

        for (unsigned i = 0; i < nloc; ++i) {
            for (unsigned j = 0; j < nloc; ++j) {
                for (unsigned k = 0; k < QuadratureType::numElementNodes; ++k) {
                    cell_A[e][i][j] += weights[k] * eval_phys(i, j, grad_q[k]);
                }
            }
        }
    }
    return cell_A;
}

template <typename LagrangeType, typename FieldType>
void scatterGlobalPattern(FieldType& field, const LagrangeType& space,
                          const std::function<double(std::size_t)>& pattern) {
    const std::size_t ndofs = space.numGlobalDOFs();

    Kokkos::View<double*> coeffs("global_coeffs", ndofs);
    {
        auto host = Kokkos::create_mirror_view(coeffs);
        for (std::size_t g = 0; g < ndofs; ++g) {
            host(g) = pattern(g);
        }
        Kokkos::deep_copy(coeffs, host);
    }

    field = typename FieldType::value_type(0);
    Kokkos::fence();
    space.fillFromGlobalCoefficients(field, coeffs);
}

}  // namespace ippl::femvalidate

#endif
