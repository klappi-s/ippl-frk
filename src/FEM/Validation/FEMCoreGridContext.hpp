#ifndef IPPL_FEM_VALIDATION_COREGRIDCONTEXT_HPP
#define IPPL_FEM_VALIDATION_COREGRIDCONTEXT_HPP

namespace ippl::femvalidate {

template <unsigned Dim, unsigned Order, unsigned QuadNodes>
struct CoreGridContext {
    static constexpr unsigned quad_nodes = QuadNodes;

    using T            = double;
    using ElementType  = std::conditional_t<
        Dim == 1, ippl::EdgeElement<T>,
        std::conditional_t<Dim == 2, ippl::QuadrilateralElement<T>, ippl::HexahedralElement<T>>>;
    using QuadratureType = ippl::GaussLegendreQuadrature<T, QuadNodes, ElementType>;
    using DOFHandler_t   = ippl::DOFHandler<T, ippl::FiniteElementSpaceTraits<ippl::LagrangeSpaceTag, Dim, Order>>;
    using FieldType      = typename DOFHandler_t::FEMContainer_t;
    using LagrangeType   = ippl::LagrangeSpace_wfc<T, Dim, Order, ElementType, QuadratureType, FieldType, FieldType>;

    static constexpr unsigned nel_core = 4;

    using Mesh_t = ippl::UniformCartesian<T, Dim>;

    Mesh_t mesh;
    ippl::FieldLayout<Dim> layout;
    ElementType ref_element;
    QuadratureType quadrature;
    LagrangeType space;

    CoreGridContext()
        : mesh(make_mesh())
        , layout(make_layout())
        , ref_element()
        , quadrature(ref_element)
        , space(mesh, ref_element, quadrature, layout) {}

    static Mesh_t make_mesh() {
        ippl::Vector<unsigned, Dim> nodesVec{};
        ippl::Vector<T, Dim> cellSpacing{};
        ippl::Vector<T, Dim> origin{};
        for (unsigned d = 0; d < Dim; ++d) {
            nodesVec[d]    = nel_core + 1;
            cellSpacing[d] = T(1) / static_cast<T>(nel_core);
            origin[d]      = T(0);
        }
        return Mesh_t(ippl::NDIndex<Dim>(nodesVec), cellSpacing, origin);
    }

    static ippl::FieldLayout<Dim> make_layout() {
        ippl::Vector<unsigned, Dim> nodesVec{};
        for (unsigned d = 0; d < Dim; ++d) {
            nodesVec[d] = nel_core + 1;
        }
        std::array<bool, Dim> isParallel{};
        isParallel.fill(true);
        return ippl::FieldLayout<Dim>(MPI_COMM_WORLD, ippl::NDIndex<Dim>(nodesVec), isParallel);
    }
};

}  // namespace ippl::femvalidate

#endif
