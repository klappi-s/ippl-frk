// Optional host diagnostics for the manual, single-rank 1D convergence study.
#ifndef IPPL_POISSON_ACCURACY_DIAGNOSTICS_1D_HPP
#define IPPL_POISSON_ACCURACY_DIAGNOSTICS_1D_HPP

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace ippl::poisson_convergence_wfc {

// Use the same entity mappings as the existing oracle test gather, without a
// GoogleTest dependency. This diagnostic deliberately rejects multi-rank runs.
template <typename Space, typename Field>
std::vector<double> gatherAccuracy1D(Space& space, Field& field) {
    using Handler = typename Space::DOFHandler_t;
    const auto& handler = space.getDOFHandler();
    const auto elements = Kokkos::create_mirror_view_and_copy(
        Kokkos::HostSpace(), handler.getElementIndices());
    std::vector<double> values(space.numGlobalDOFs());
    auto entity = [&]<typename Entity>() {
        const auto host = Kokkos::create_mirror_view_and_copy(
            Kokkos::HostSpace(), field.template getView<Entity>());
        for (size_t e = 0; e < elements.extent(0); ++e) {
            const auto element = elements(e);
            const auto local = handler.getLocalElementNDIndex(element, field.getNghost());
            const auto global = space.getGlobalDOFIndices(element);
            for (size_t i = Handler::template getEntityDOFStart<Entity>();
                 i < Handler::template getEntityDOFEnd<Entity>(); ++i) {
                const auto map = handler.getElementDOFMapping(i);
                values[global[i]] = ippl::apply(host, local + map.entityLocalIndex)[map.entityLocalDOF];
            }
        }
    };
    [&]<typename... Entities>(std::tuple<Entities...>) {
        (entity.template operator()<Entities>(), ...);
    }(typename Handler::EntityTypes{});
    return values;
}

template <typename Solver, typename Field, typename Exact>
void writeAccuracyDiagnostics1D(Solver& solver, Field& lhs, Field& load,
                               const Exact& exact, const char* source,
                               unsigned order, unsigned numNodes,
                               const std::string& interpolation,
                               const std::string& quadratureFamily) {
    using Space = typename Solver::LagrangeType;
    using Element = typename Solver::ElementType;
    using Quadrature = typename Solver::QuadratureType;
    auto& space = solver.getSpace();
    const auto spacing = lhs.get_mesh().getMeshSpacing(0);
    const auto inverse = ippl::Vector<double, 1>(1.0 / spacing);
    ippl::EvalFunctor<double, 1, Space::numElementDOFs> eval(inverse, spacing);
    auto action = space.evaluateAx(lhs, eval);
    auto residual = load.deepCopy();
    residual = load - action;
    const double loadNorm = ippl::norm(load);
    const double freshNorm = ippl::norm(residual);

    const auto values = gatherAccuracy1D(space, lhs);
    const auto loads = gatherAccuracy1D(space, load);
    const auto actions = gatherAccuracy1D(space, action);
    std::vector<double> coordinates(values.size());
    for (unsigned cell = 0; cell + 1 < numNodes; ++cell) {
        const auto dofs = space.getGlobalDOFIndices(cell);
        const auto vertices = space.getElementMeshVertexPoints(ippl::Vector<size_t, 1>(cell));
        Element element;
        for (size_t i = 0; i < Space::numElementDOFs; ++i)
            coordinates[dofs[i]] = element.localToGlobal(
                vertices, space.getRefElementDOFLocation(i))[0];
    }
    Kokkos::View<double*> coefficients("exact interpolant", values.size());
    auto host = Kokkos::create_mirror_view(coefficients);
    for (size_t i = 0; i < values.size(); ++i)
        host(i) = exact(ippl::Vector<double, 1>(coordinates[i]));
    Kokkos::deep_copy(coefficients, host);
    auto interpolant = lhs.deepCopy();
    space.fillFromGlobalCoefficients(interpolant, coefficients);
    interpolant.fillHalo();
    const double interpolationError = space.computeErrorL2(interpolant, exact);

    const std::string stem = std::string(source) + "_p" + std::to_string(order)
                             + "_n" + std::to_string(numNodes);
    std::ofstream output("accuracy_" + stem + ".csv");
    if (!output) throw std::runtime_error("cannot write accuracy coefficient diagnostic");
    output << "x,u,load,action,exact_nodal\n" << std::setprecision(17);
    for (size_t i = 0; i < values.size(); ++i)
        output << coordinates[i] << ',' << values[i] << ',' << loads[i] << ','
               << actions[i] << ',' << host(i) << '\n';

    const auto summaryPath = std::filesystem::path("accuracy_summary.csv");
    const bool newFile = !std::filesystem::exists(summaryPath);
    std::ofstream summary(summaryPath, std::ios::app);
    if (!summary) throw std::runtime_error("cannot write accuracy summary");
    if (newFile)
        summary << "source,order,num_nodes,absolute_l2,interpolation_l2,recursive_residual,"
                   "fresh_residual,rhs_norm,relative_fresh_residual\n";
    summary << std::setprecision(17) << source << ',' << order << ',' << numNodes << ','
            << solver.getL2Error(exact) << ',' << interpolationError << ','
            << solver.getResidue() << ',' << freshNorm << ',' << loadNorm << ','
            << freshNorm / loadNorm << '\n';

    // Recover the actual physical element stiffness using single-cell NO_FACE
    // evaluateAx columns. This avoids duplicating production quadrature arithmetic.
    const std::string matrixPath = "element_p" + std::to_string(order)
                                   + "_n" + std::to_string(numNodes) + ".csv";
    if (std::filesystem::exists(matrixPath)) return;
    const ippl::NDIndex<1> domain(ippl::Vector<unsigned, 1>(2));
    ippl::UniformCartesian<double, 1> mesh(domain, ippl::Vector<double, 1>(spacing),
                                         ippl::Vector<double, 1>(0));
    ippl::FieldLayout<1> layout(MPI_COMM_WORLD, domain, std::array<bool, 1>{true});
    Element element;
    Quadrature quadrature(element, ippl::parseQuadratureNodeFamily(quadratureFamily));
    Space single(mesh, element, quadrature, layout);
    single.setInterpolationNodes(ippl::parseLagrangeNodeFamily(interpolation));
    Field input(mesh, layout, 1);
    input.setFieldBC(std::array<ippl::FieldBC, 2>{ippl::NO_FACE, ippl::NO_FACE});
    Kokkos::View<double*> column("element column", order + 1);
    auto columnHost = Kokkos::create_mirror_view(column);
    std::vector<std::vector<double>> matrix(order + 1, std::vector<double>(order + 1));
    for (unsigned j = 0; j <= order; ++j) {
        for (unsigned i = 0; i <= order; ++i) columnHost(i) = i == j ? 1.0 : 0.0;
        Kokkos::deep_copy(column, columnHost);
        input = 0.0;
        single.fillFromGlobalCoefficients(input, column);
        auto result = single.evaluateAx(input, eval);
        const auto entries = gatherAccuracy1D(single, result);
        for (unsigned i = 0; i <= order; ++i) matrix[i][j] = entries[i];
    }
    std::ofstream matrixOutput(matrixPath);
    if (!matrixOutput) throw std::runtime_error("cannot write element matrix diagnostic");
    matrixOutput << std::setprecision(17);
    for (const auto& row : matrix) {
        for (unsigned j = 0; j <= order; ++j)
            matrixOutput << (j ? "," : "") << row[j];
        matrixOutput << '\n';
    }
}
}  // namespace ippl::poisson_convergence_wfc
#endif
