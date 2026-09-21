// Class FEMPoissonSolver_wFEMContainer
//   Track B Poisson solver: LagrangeSpace_wfc + FEMContainer + matrix-free CG/PCG.
//
//   Select via params["preconditioned"] = true|false (default: true).
//   When preconditioned, uses ippl::PCG<> + Preconditioner.h (jacobi, newton, chebyshev,
//   richardson, richardson_alt, gauss-seidel, ssor, multigrid (P1 h-MG; P2/P3 p-multigrid).
//   Default preconditioner_type is "jacobi" (SSOR is weak/harmful for higher-order FEM).
//   Multigrid uses Flexible CG + FEM rediscretization on all h-levels (Task 5.4).
//   Periodic Newton/Chebyshev use conservative positive-spectrum bounds.
//   Dirichlet bounds retain powermethod / adapted_powermethod estimates.

#ifndef IPPL_FEMPOISSONSOLVER_WFC_H
#define IPPL_FEMPOISSONSOLVER_WFC_H

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <string>
#include <sstream>
#include <iomanip>
#include "Utility/Timer.h"

#include "LinearSolvers/PCG.h"
#include "LinearSolvers/Preconditioner.h"
#include "LinearSolvers/PreconditionerValidation.h"
#include "LinearSolvers/MultigridFEMContainer_p.h"
#include "Poisson.h"
#include "EvalFunctor.h"
#include "FEM/LagrangeSpace_wFEMContainer.h"
#include "FEM/Quadrature/SelectableQuadrature.h"

namespace ippl {

    template <typename FieldLHS, typename FieldRHS = FieldLHS, unsigned Order = 1,
              unsigned QuadNumNodes = 5>
    class FEMPoissonSolver_wFEMContainer : public Poisson<FieldLHS, FieldRHS> {
        constexpr static unsigned Dim = FieldLHS::dim;
        using Tlhs                    = typename FieldLHS::value_type;

    public:
        using Base = Poisson<FieldLHS, FieldRHS>;
        using typename Base::lhs_type, typename Base::rhs_type;
        using MeshType = typename FieldRHS::Mesh_t;

        using CGSolverAlgorithm_t =
            CG<lhs_type, lhs_type, lhs_type, lhs_type, lhs_type, lhs_type, FieldLHS, FieldRHS>;

        using PCGSolverAlgorithm_t =
            PCG<lhs_type, lhs_type, lhs_type, lhs_type, lhs_type, lhs_type, FieldLHS, FieldRHS>;

        using ElementType =
            std::conditional_t<Dim == 1, ippl::EdgeElement<Tlhs>,
                               std::conditional_t<Dim == 2, ippl::QuadrilateralElement<Tlhs>,
                                                   ippl::HexahedralElement<Tlhs>>>;

        using QuadratureType = SelectableQuadrature<Tlhs, QuadNumNodes, ElementType>;

        using LagrangeType =
            LagrangeSpace_wfc<Tlhs, Dim, Order, ElementType, QuadratureType, FieldLHS, FieldRHS>;

        FEMPoissonSolver_wFEMContainer()
            : Base()
            , refElement_m()
            , quadrature_m(refElement_m, QuadratureNodeFamily::GaussLegendre)
            , lagrangeSpace_m(*(new MeshType(NDIndex<Dim>(Vector<unsigned, Dim>(0)),
                                             Vector<Tlhs, Dim>(0), Vector<Tlhs, Dim>(0))),
                              refElement_m, quadrature_m) {
            setDefaultParameters();
        }

        FEMPoissonSolver_wFEMContainer(lhs_type& lhs, rhs_type& rhs)
            : Base(lhs, rhs)
            , refElement_m()
            , quadrature_m(refElement_m, QuadratureNodeFamily::GaussLegendre)
            , lagrangeSpace_m(rhs.get_mesh(), refElement_m, quadrature_m, rhs.getLayout()) {
            static_assert(std::is_floating_point<Tlhs>::value, "Not a floating point type");
            setDefaultParameters();
        }

        void setRhs(rhs_type& rhs) override {
            Base::setRhs(rhs);
            invalidatePreparedData();
            lagrangeSpace_m.initialize(rhs.get_mesh(), rhs.getLayout());
        }

        LagrangeType& getSpace() { return lagrangeSpace_m; }

        PoissonStiffnessMode getStiffnessMode() const {
            return parsePoissonStiffnessMode(
                this->params_m.template get<std::string>("poisson_stiffness_mode"));
        }

        /** Apply interpolation/quadrature ParameterList keys (also invoked at start of solve). */
        void configureNodeFamilies() { applyNodeFamilyParameters(); }

        // bool usesGllMassShortcut() const { return gllMassShortcutActive_m; }

        void solve() override { solveImpl(*this->rhs_mp, false); }

        // The caller retains an already assembled weak load; no second mass action.
        void solveLoadVector(const rhs_type& load) {
            auto work = load.deepCopy();
            solveImpl(work, true);
        }

        struct SolveStatistics {
            double operatorSetupSeconds=0, preconditionerSetupSeconds=0, krylovSeconds=0;
            unsigned operatorSetups=0, preconditionerSetups=0, operatorCalls=0;
            unsigned preconditionerOperatorCalls=0, preconditionerApplications=0;
            double preconditionerApplicationSeconds=0;
            double spectralLower=0, spectralUpper=0;
            unsigned triangularCalls=0, diagonalCalls=0;
            double operatorSeconds=0, triangularSeconds=0, diagonalSeconds=0;
            double diagonalSetupSeconds=0, spectralSetupSeconds=0;
        };
        const SolveStatistics& getSolveStatistics() const { return statistics_m; }
        lhs_type applyOperator(rhs_type field) const {
            const auto vertices=lagrangeSpace_m.getElementMeshVertexPoints(Vector<size_t,Dim>(0));
            PoissonEval raw(refElement_m.getInverseTransposeTransformationJacobian(vertices),
                Kokkos::abs(refElement_m.getDeterminantOfTransformationJacobian(vertices)),getStiffnessMode());
            PreparedEval eval{raw,elementMatrix_m};
            field.setFieldBC(this->lhs_mp->getFieldBC());
            field.fillHalo();
            return lagrangeSpace_m.evaluateAx(field,eval);
        }
        void invalidatePreparedData() {
            operatorKey_m.clear(); preconditionerKey_m.clear();
            elementMatrix_m.reset(); diagonal_m.reset(); constant_m.reset(); volumeWeights_m.reset();
            cg_algo_m.clearWorkspace(); pcg_algo_m.clearWorkspace();
        }

    private:
        using ElementMatrix=typename LagrangeType::ElementMatrix;
        using PoissonEval=EvalFunctor<Tlhs,Dim,LagrangeType::numElementDOFs>;
        struct PreparedEval : PoissonEval {
            std::shared_ptr<const ElementMatrix> matrix;
            const ElementMatrix* preparedMatrix() const { return matrix.get(); }
        };
        std::string operatorKey_m, preconditionerKey_m;
        std::shared_ptr<const ElementMatrix> elementMatrix_m;
        std::shared_ptr<lhs_type> diagonal_m,constant_m,volumeWeights_m;
        Tlhs constantNormSquared_m=0,volume_m=0;
        double spectralLower_m=0,spectralUpper_m=0;
        SolveStatistics statistics_m;
        bool detailedTiming_m=false;
        template<typename Action>
        lhs_type profileAction(Action&& action,unsigned& calls,double& seconds) {
            ++calls;
            Timer timer;
            if (detailedTiming_m) { Kokkos::fence(); timer.start(); }
            auto result=action();
            if (detailedTiming_m) { Kokkos::fence(); timer.stop(); seconds+=timer.elapsed(); }
            return result;
        }
        std::string operatorKey() const {
            std::ostringstream key;
            key << std::setprecision(17);
            for (auto name : {"interpolation_nodes","quadrature_nodes","poisson_stiffness_mode"})
                key << this->params_m.template get<std::string>(name) << ':';
            const auto& mesh=this->lhs_mp->get_mesh();
            const auto& layout=this->lhs_mp->getLayout();
            key << layout.isAllPeriodic_m;
            for (unsigned d=0; d<Dim; ++d)
                key << ':' << mesh.getMeshSpacing()[d] << ':' << mesh.getOrigin()[d]
                    << ':' << mesh.getGridsize()[d] << ':' << layout.getLocalNDIndex()[d].first()
                    << ':' << layout.getLocalNDIndex()[d].last();
            for (auto bc:this->lhs_mp->getFieldBCTypes()) key << ':' << int(bc);
            return key.str();
        }
        void solveImpl(rhs_type& rhs, bool assembledLoad) {
            statistics_m={};
            detailedTiming_m=this->params_m.template get<bool>("fem_detailed_timing");
            const auto reuse=this->params_m.template get<std::string>("fem_reuse");
            if (reuse!="off" && reuse!="operator" && reuse!="full")
                throw IpplException("FEMPoissonSolver", "Unknown fem_reuse mode.");
            const auto key=operatorKey();
            if (reuse=="off" || key!=operatorKey_m) {
                Timer timer; Kokkos::fence(); timer.start();
                invalidatePreparedData();
                lagrangeSpace_m.initialize(rhs.get_mesh(),rhs.getLayout());
                applyNodeFamilyParameters();
                operatorKey_m=key;
                Kokkos::fence(); timer.stop();
                statistics_m.operatorSetupSeconds=timer.elapsed();
                statistics_m.operatorSetups=1;
            }
            if (reuse!="full") { diagonal_m.reset(); preconditionerKey_m.clear(); }
            const auto stiffnessMode = getStiffnessMode();

            if (stiffnessMode != PoissonStiffnessMode::Standard) {
                const auto boundaries = this->lhs_mp->getFieldBCTypes();
                if (!std::all_of(boundaries.begin(), boundaries.end(), [](FieldBC bc) {
                        return bc == ZERO_FACE || bc == CONSTANT_FACE || bc == PERIODIC_FACE;
                    }))
                    throw IpplException("FEMPoissonSolver_wFEMContainer::solve",
                                        "constant_preserving requires uniform Dirichlet or periodic boundaries.");
                auto preconditioner = this->params_m.template get<std::string>("preconditioner_type");
                std::transform(preconditioner.begin(), preconditioner.end(), preconditioner.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (this->params_m.template get<bool>("preconditioned") && preconditioner == "multigrid")
                    throw IpplException("FEMPoissonSolver_wFEMContainer::solve",
                                        "constant_preserving is not validated with multigrid; "
                                        "set poisson_stiffness_mode=standard for multigrid solves.");
            }

            // The solution owns Dirichlet metadata; rhs still contains source samples.
            // Krylov directions have homogeneous BCs, while lhs retains prescribed DOFs.
            const auto solutionBC = this->lhs_mp->getFieldBC();
            auto bcTypes = this->lhs_mp->getFieldBCTypes();
            const bool dirichlet = std::all_of(bcTypes.begin(), bcTypes.end(), [](FieldBC bc) {
                return bc == ZERO_FACE || bc == CONSTANT_FACE;
            });
            if (dirichlet && this->lhs_mp->getLayout().isAllPeriodic_m)
                throw IpplException("FEMPoissonSolver", "Dirichlet conditions require nonperiodic FEM ownership.");
            if (dirichlet) {
                using ScalarField = Field<Tlhs, Dim, MeshType, Cell>;
                Tlhs prescribed = 0;
                for (unsigned face = 0; face < 2 * Dim; ++face) {
                    const Tlhs value = bcTypes[face] == ZERO_FACE ? Tlhs(0) :
                        dynamic_cast<ExtrapolateFace<ScalarField>&>(*solutionBC[face]).getOffset();
                    if (face == 0) prescribed = value;
                    if (value != prescribed)
                        throw IpplException("FEMPoissonSolver_wFEMContainer::solve",
                                            "Dirichlet faces must prescribe the same constant.");
                }
                detail::setFEMBoundaryDOFs(*(this->lhs_mp), *(this->lhs_mp), prescribed, false);
                bcTypes.fill(ZERO_FACE);
            } else if (!std::all_of(bcTypes.begin(), bcTypes.end(), [](FieldBC bc) {
                           return bc == PERIODIC_FACE;
                       })) {
                throw IpplException("FEMPoissonSolver_wFEMContainer::solve",
                                    "Only uniform Dirichlet or fully periodic boundaries are supported.");
            }
            rhs.setFieldBC(bcTypes);
            if (!dirichlet && !rhs.getLayout().isAllPeriodic_m)
                throw IpplException("FEMPoissonSolver_wFEMContainer::solve",
                    "Periodic FEM requires a periodic geometric vertex layout (isAllPeriodic=true).");
            if (!assembledLoad) {
                rhs.fillHalo();
                lagrangeSpace_m.evaluateLoadVector(rhs);
            }
            if (!dirichlet) {
                if (!constant_m) {
                    Timer timer; Kokkos::fence(); timer.start();
                    constant_m=std::make_shared<lhs_type>(rhs); *constant_m=Tlhs(1);
                    constantNormSquared_m=innerProduct(*constant_m,*constant_m);
                    volumeWeights_m=std::make_shared<lhs_type>(*constant_m);
                    volumeWeights_m->fillHalo(); lagrangeSpace_m.evaluateLoadVector(*volumeWeights_m);
                    volume_m=innerProduct(*constant_m,*volumeWeights_m);
                    Kokkos::fence(); timer.stop();
                    statistics_m.operatorSetupSeconds+=timer.elapsed();
                    statistics_m.operatorSetups=1;
                }
                const auto constant=constant_m;
                const Tlhs count=constantNormSquared_m;
                const Tlhs charge = innerProduct(*constant, rhs);
                if (std::abs(charge) > Tlhs(1e-12) *
                    std::max(Tlhs(1), norm(rhs) * std::sqrt(count)))
                    throw IpplException("FEMPoissonSolver_wFEMContainer::solve",
                                        "Periodic load has nonzero total charge.");
                auto project = [constant, count](lhs_type& value) {
                    value -= innerProduct(*constant, value) / count;
                };
                project(rhs);
                cg_algo_m.setProjector(project);
                pcg_algo_m.setProjector(project);
            } else {
                cg_algo_m.setProjector({});
                pcg_algo_m.setProjector({});
            }

            const Vector<size_t, Dim> zeroNdIndex = Vector<size_t, Dim>(0);

            const auto firstElementVertexPoints =
                lagrangeSpace_m.getElementMeshVertexPoints(zeroNdIndex);

            const Vector<Tlhs, Dim> DPhiInvT =
                refElement_m.getInverseTransposeTransformationJacobian(firstElementVertexPoints);

            const Tlhs absDetDPhi = Kokkos::abs(
                refElement_m.getDeterminantOfTransformationJacobian(firstElementVertexPoints));

            PoissonEval rawEval(DPhiInvT,absDetDPhi,stiffnessMode);
            if (reuse!="off" && !elementMatrix_m) {
                Timer timer; Kokkos::fence(); timer.start();
                elementMatrix_m=std::make_shared<ElementMatrix>(lagrangeSpace_m.assembleElementMatrix(rawEval));
                Kokkos::fence(); timer.stop();
                statistics_m.operatorSetupSeconds+=timer.elapsed();
                statistics_m.operatorSetups=1;
            }
            PreparedEval poissonEquationEval{rawEval,elementMatrix_m};

            const auto algoOperator = [poissonEquationEval, bcTypes, this](rhs_type field) -> lhs_type {
                if (pcg_algo_m.applyingPreconditioner()) ++statistics_m.preconditionerOperatorCalls;
                return profileAction([&] {
                    field.setFieldBC(bcTypes); field.fillHalo();
                    return lagrangeSpace_m.evaluateAx(field,poissonEquationEval);
                },statistics_m.operatorCalls,statistics_m.operatorSeconds);
            };
            const auto algoOperatorL = [poissonEquationEval, bcTypes, this](rhs_type field) -> lhs_type {
                return profileAction([&] {
                    field.setFieldBC(bcTypes); field.fillHalo();
                    return lagrangeSpace_m.evaluateAx_lower(field,poissonEquationEval);
                },statistics_m.triangularCalls,statistics_m.triangularSeconds);
            };
            const auto algoOperatorU = [poissonEquationEval, bcTypes, this](rhs_type field) -> lhs_type {
                return profileAction([&] {
                    field.setFieldBC(bcTypes); field.fillHalo();
                    return lagrangeSpace_m.evaluateAx_upper(field,poissonEquationEval);
                },statistics_m.triangularCalls,statistics_m.triangularSeconds);
            };
            const auto algoOperatorUL = [poissonEquationEval, bcTypes, this](rhs_type field) -> lhs_type {
                return profileAction([&] {
                    field.setFieldBC(bcTypes); field.fillHalo();
                    return lagrangeSpace_m.evaluateAx_upperlower(field,poissonEquationEval);
                },statistics_m.triangularCalls,statistics_m.triangularSeconds);
            };
            const auto algoOperatorInvD = [poissonEquationEval, bcTypes, this](rhs_type field) -> lhs_type {
                return profileAction([&] {
                    field.setFieldBC(bcTypes); field.fillHalo();
                    if (diagonal_m) return field / *diagonal_m;
                    return lagrangeSpace_m.evaluateAx_inversediag(field,poissonEquationEval);
                },statistics_m.diagonalCalls,statistics_m.diagonalSeconds);
            };
            const auto algoOperatorD = [poissonEquationEval, bcTypes, this](rhs_type field) -> lhs_type {
                return profileAction([&] {
                    field.setFieldBC(bcTypes); field.fillHalo();
                    if (diagonal_m) return field * *diagonal_m;
                    return lagrangeSpace_m.evaluateAx_diag(field,poissonEquationEval);
                },statistics_m.diagonalCalls,statistics_m.diagonalSeconds);
            };

            if (dirichlet) {
                this->lhs_mp->fillHalo();
                rhs = rhs -
                    lagrangeSpace_m.evaluateAx_lift(*(this->lhs_mp), poissonEquationEval);
            }

            const bool usePrecon = this->params_m.template get<bool>("preconditioned");

            static IpplTimings::TimerRef solveTimer = IpplTimings::getTimer("cg");
            IpplTimings::startTimer(solveTimer);

            if (usePrecon) {
                std::string preconditioner_type =
                    this->params_m.template get<std::string>("preconditioner_type");
                std::transform(preconditioner_type.begin(), preconditioner_type.end(),
                               preconditioner_type.begin(), [](unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });

                preconditioner_validation::throwIfUnknownType(
                    preconditioner_type, "FEMPoissonSolver_wFEMContainer::solve");

                Inform warn("FEMPoissonSolver_wFEMContainer");
                int level    = this->params_m.template get<int>("newton_level");
                int degree   = this->params_m.template get<int>("chebyshev_degree");
                int inner    = this->params_m.template get<int>("gauss_seidel_inner_iterations");
                int outer    = this->params_m.template get<int>("gauss_seidel_outer_iterations");
                double omega = this->params_m.template get<double>("ssor_omega");
                int richardson_iterations =
                    this->params_m.template get<int>("richardson_iterations");
                int communication     = pcg_preconditioner_defaults::communication;
                int mg_pre            = pcg_preconditioner_defaults::mg_pre_smooth;
                int mg_post           = pcg_preconditioner_defaults::mg_post_smooth;
                double mg_omega       = pcg_preconditioner_defaults::mg_omega;
                int mg_min_cells = pcg_preconditioner_defaults::mg_min_cells;

                preconditioner_validation::sanitizeParams(
                    preconditioner_type, warn, level, degree, richardson_iterations, inner, outer,
                    omega, &communication, mg_pre, mg_post, mg_omega, mg_min_cells);

                std::ostringstream signature;
                signature << std::setprecision(17) << operatorKey_m << ':' << preconditioner_type
                    << ':' << level << ':' << degree << ':' << richardson_iterations
                    << ':' << inner << ':' << outer << ':' << omega;
                const bool prepare=reuse!="full" || signature.str()!=preconditionerKey_m;
                Timer setupTimer;
                Kokkos::fence(); setupTimer.start();
                if (prepare && reuse=="full" && !diagonal_m) {
                    Timer child; child.start();
                    auto unit=rhs.deepCopy(); unit=Tlhs(1); unit.fillHalo();
                    diagonal_m=std::make_shared<lhs_type>(lagrangeSpace_m.evaluateAx_diag(unit,poissonEquationEval));
                    // Safe values on constrained rows: residuals vanish there.
                    if (dirichlet) detail::setFEMBoundaryDOFs(*diagonal_m,*diagonal_m,Tlhs(1),false);
                    diagonal_m->fillHalo();
                    Kokkos::fence(); child.stop(); statistics_m.diagonalSetupSeconds=child.elapsed();
                }
                Timer spectralTimer; spectralTimer.start();
                double alpha = 0.0;
                double beta  = 0.0;
                if (prepare && !dirichlet &&
                    (preconditioner_type=="newton" || preconditioner_type=="chebyshev")) {
                    // Conservative positive-spectrum bounds. Point evaluation for tensor
                    // degree p has squared L2 norm <= (p+1)^(2*Dim). Summing local
                    // nodal values and using periodic Poincare gives this lower bound
                    // on the algebraic mean-zero subspace. No nullspace power seed.
                    const auto matrix=lagrangeSpace_m.assembleElementMatrix(poissonEquationEval);
                    double rowBound=0, longest=0;
                    for (unsigned i=0; i<LagrangeType::numElementDOFs; ++i) {
                        double row=0;
                        for (unsigned j=0; j<LagrangeType::numElementDOFs; ++j) row+=std::abs(matrix[i][j]);
                        rowBound=std::max(rowBound,row);
                    }
                    for (unsigned d=0; d<Dim; ++d)
                        longest=std::max(longest,double(rhs.get_mesh().getMeshSpacing()[d]*(rhs.get_mesh().getGridsize()[d]-1)));
                    const double wave=2*Kokkos::numbers::pi_v<double>/longest;
                    alpha=0.5*wave*wave*absDetDPhi/std::pow(double(Order+1),3*Dim);
                    beta=1.01*(1u<<Dim)*rowBound;
                } else if (prepare && (preconditioner_type == "newton" || preconditioner_type == "chebyshev")) {
                    lhs_type x0(*(this->lhs_mp));
                    x0 = Tlhs(1);
                    x0.setFieldBC(bcTypes);
                    if (stiffnessMode != PoissonStiffnessMode::Standard)
                        detail::setFEMBoundaryDOFs(x0, x0, Tlhs(0), false);
                    // Cap power iterations: FEM operator is expensive vs FD laplace.
                    beta = powermethod(algoOperator, x0, /*max_iter=*/200, /*tol=*/1e-3);
                    x0   = Tlhs(1);
                    x0.setFieldBC(bcTypes);
                    if (stiffnessMode != PoissonStiffnessMode::Standard)
                        detail::setFEMBoundaryDOFs(x0, x0, Tlhs(0), false);
                    alpha = adapted_powermethod(algoOperator, x0, beta, /*max_iter=*/200,
                                                /*tol=*/1e-3);
                    if (!(alpha > 0.0 && beta > alpha)) {
                        warn << "Spectral estimate alpha=" << alpha << " beta=" << beta
                             << " looks invalid; falling back to jacobi-safe bounds." << endl;
                        // Conservative positive interval so Chebyshev/Newton init_fields
                        // do not divide by zero; quality may be poor but remains defined.
                        alpha = 1e-3;
                        beta  = 1.0;
                    }
                }

                spectralTimer.stop();
                if (prepare && (preconditioner_type=="newton" || preconditioner_type=="chebyshev"))
                    statistics_m.spectralSetupSeconds=spectralTimer.elapsed();
                if (prepare) { spectralLower_m=alpha; spectralUpper_m=beta; }
                statistics_m.spectralLower=spectralLower_m;
                statistics_m.spectralUpper=spectralUpper_m;
                if (!dirichlet && preconditioner_type=="multigrid")
                    throw IpplException("FEMPoissonSolver", "Periodic FEM multigrid is not supported.");
                if (preconditioner_type == "multigrid" && Order > 1) {
                    // p-multigrid: build Order→…→P1 chain; P1 bottom = geometric h-MG.
                    auto& mesh   = rhs.get_mesh();
                    auto& layout = rhs.getLayout();

                    using P1Traits = FiniteElementSpaceTraits<LagrangeSpaceTag, Dim, 1>;
                    using P1Cont   = typename DOFHandler<Tlhs, P1Traits>::FEMContainer_t;
                    using P1Space =
                        LagrangeSpace_wfc<Tlhs, Dim, 1, ElementType, QuadratureType, P1Cont, P1Cont>;
                    EvalFunctor<Tlhs, Dim, P1Space::numElementDOFs> eval1(
                        DPhiInvT, absDetDPhi, stiffnessMode);

                    auto space1 = std::make_shared<P1Space>(mesh, refElement_m, quadrature_m, layout);

                    auto build_p1_bottom = [&]() {
                        // P1 geometric h-MG as bottom solver (same as standalone P1 multigrid).
                        std::function<P1Cont(P1Cont)> op1b =
                            [eval1, bcTypes, space1](P1Cont field) -> P1Cont {
                            field.setFieldBC(bcTypes);
                            field.fillHalo();
                            return space1->evaluateAx(field, eval1);
                        };
                        std::function<P1Cont(P1Cont)> invD1b =
                            [eval1, bcTypes, space1](P1Cont field) -> P1Cont {
                            field.setFieldBC(bcTypes);
                            field.fillHalo();
                            return space1->evaluateAx_inversediag(field, eval1);
                        };
                        return std::make_unique<
                            fem_multigrid_preconditioner<P1Cont, std::function<P1Cont(P1Cont)>,
                                                         std::function<P1Cont(P1Cont)>>>(
                            std::move(op1b), std::move(invD1b), mg_pre, mg_post, mg_omega,
                            mg_min_cells, static_cast<bool>(communication));
                    };

                    // More smoothing on high-order levels than FD defaults
                    const unsigned pmg_pre  = static_cast<unsigned>(std::max(mg_pre, 4));
                    const unsigned pmg_post = static_cast<unsigned>(std::max(mg_post, 4));

                    if constexpr (Order == 2) {
                        auto bottom = build_p1_bottom();
                        using Bottom = typename decltype(bottom)::element_type;
                        auto pmg     = std::make_unique<
                            fem_pmultigrid_preconditioner<lhs_type, P1Cont, Bottom, 2, 1>>(
                            algoOperator, algoOperatorInvD, lagrangeSpace_m.getDOFHandler(),
                            space1->getDOFHandler(),
                            lagrangeSpace_m.getDOFHandler().getElementIndices(), std::move(bottom),
                            pmg_pre, pmg_post, mg_omega);
                        pcg_algo_m.setPreconditionerObject(std::move(pmg));
                    } else if constexpr (Order == 3) {
                        auto bottom = build_p1_bottom();
                        using Bottom = typename decltype(bottom)::element_type;
                        auto pmg     = std::make_unique<
                            fem_pmultigrid_preconditioner<lhs_type, P1Cont, Bottom, 3, 1>>(
                            algoOperator, algoOperatorInvD, lagrangeSpace_m.getDOFHandler(),
                            space1->getDOFHandler(),
                            lagrangeSpace_m.getDOFHandler().getElementIndices(), std::move(bottom),
                            pmg_pre, pmg_post, mg_omega);
                        pcg_algo_m.setPreconditionerObject(std::move(pmg));
                    } else {
                        throw IpplException("FEMPoissonSolver_wFEMContainer::solve",
                                            "p-multigrid supports Lagrange Order 2 or 3 only.");
                    }

                    this->params_m.update("flexible_cg", true);
                    pcg_algo_m.setOperator(algoOperator);
                    pcg_algo_m(*(this->lhs_mp), rhs, this->params_m);
                } else {
                    this->params_m.update("flexible_cg", preconditioner_type == "multigrid");
                    if (prepare) pcg_algo_m.setPreconditioner(algoOperator, algoOperatorL, algoOperatorU,
                                                 algoOperatorUL, algoOperatorInvD, algoOperatorD,
                                                 alpha, beta, preconditioner_type, level, degree,
                                                 richardson_iterations, inner, outer, omega, mg_pre,
                                                 mg_post, mg_omega, mg_min_cells);
                    pcg_algo_m.preparePreconditioner(*this->lhs_mp);
                    if (prepare) preconditionerKey_m=signature.str();
                    Kokkos::fence(); setupTimer.stop();
                    statistics_m.preconditionerSetupSeconds=setupTimer.elapsed();
                    statistics_m.preconditionerSetups=prepare;
                    Timer krylov; krylov.start();
                    pcg_algo_m.setOperator(algoOperator);
                    pcg_algo_m(*(this->lhs_mp), rhs, this->params_m);
                    Kokkos::fence(); krylov.stop();
                    statistics_m.krylovSeconds=krylov.elapsed();
                }
                iteration_count_m = pcg_algo_m.getIterationCount();
                residue_m         = pcg_algo_m.getResidue();
                statistics_m.preconditionerApplications=pcg_algo_m.getApplicationCount();
                statistics_m.preconditionerApplicationSeconds=pcg_algo_m.getApplicationSeconds();
            } else {
                Timer krylov; Kokkos::fence(); krylov.start();
                cg_algo_m.setOperator(algoOperator);
                cg_algo_m(*(this->lhs_mp), rhs, this->params_m);
                Kokkos::fence(); krylov.stop();
                statistics_m.krylovSeconds=krylov.elapsed();
                iteration_count_m = cg_algo_m.getIterationCount();
                residue_m         = cg_algo_m.getResidue();
            }

            this->lhs_mp->fillHalo();
            if (!dirichlet) *this->lhs_mp -= innerProduct(*volumeWeights_m,*this->lhs_mp)/volume_m;
            (this->lhs_mp)->fillHalo();
            IpplTimings::stopTimer(solveTimer);
        }

    public:
        int getIterationCount() const { return iteration_count_m; }
        Tlhs getResidue() const { return residue_m; }

        template <typename F>
        Tlhs getL2Error(const F& analytic) {
            return this->lagrangeSpace_m.computeErrorL2(*(this->lhs_mp), analytic);
        }

        Tlhs getAvg(bool Vol = false) {
            Tlhs avg = this->lagrangeSpace_m.computeAvg(*(this->lhs_mp));
            if (Vol) {
                lhs_type unit((this->lhs_mp)->get_mesh(), (this->lhs_mp)->getLayout());
                unit     = 1.0;
                Tlhs vol = this->lagrangeSpace_m.computeAvg(unit);
                return avg / vol;
            }
            return avg;
        }

    protected:
        CGSolverAlgorithm_t cg_algo_m;
        PCGSolverAlgorithm_t pcg_algo_m;

        int iteration_count_m = 0;
        Tlhs residue_m        = 0;

        virtual void setDefaultParameters() override {
            this->params_m.add("max_iterations", 1000);
            this->params_m.add("tolerance", (Tlhs)1e-13);
            this->params_m.add("preconditioned", true);
            this->params_m.add("fem_reuse",std::string("off"));
            this->params_m.add("fem_detailed_timing",false);
            this->params_m.add("preconditioner_type", "jacobi");
            this->params_m.add("poisson_stiffness_mode", std::string("constant_preserving"));
            this->params_m.add("flexible_cg", false);
            // Milder than FD/alpine defaults: spectral types use estimated eigenvalues.
            this->params_m.add("newton_level", 2);
            this->params_m.add("chebyshev_degree", 5);
            this->params_m.add("richardson_iterations",
                               pcg_preconditioner_defaults::richardson_iterations);
            this->params_m.add("gauss_seidel_inner_iterations",
                               pcg_preconditioner_defaults::gauss_seidel_inner);
            this->params_m.add("gauss_seidel_outer_iterations",
                               pcg_preconditioner_defaults::gauss_seidel_outer);
            this->params_m.add("ssor_omega", pcg_preconditioner_defaults::ssor_omega);
            this->params_m.add("interpolation_nodes", std::string("gll"));
            this->params_m.add("quadrature_nodes", std::string("gauss_legendre"));
        }

        /**
         * @brief Apply ParameterList interpolation/quadrature node families before assembly.
         */
        void applyNodeFamilyParameters() {
            const auto interp = parseLagrangeNodeFamily(
                this->params_m.template get<std::string>("interpolation_nodes"));
            const auto quad = parseQuadratureNodeFamily(
                this->params_m.template get<std::string>("quadrature_nodes"));

            lagrangeSpace_m.setInterpolationNodes(interp);
            quadrature_m.setFamily(quad);

            // Mass-matrix underintegration shortcut (GLL interp + GLL quad,
            // QuadNumNodes == Order+1) is deferred until a mass/reaction term exists.
            // Pure Poisson is stiffness-only, so this hook would only set a flag.
            // Restore when mass assembly is added.
            //
            // inline bool gllMassShortcutApplicable(LagrangeNodeFamily interp,
            //                                       QuadratureNodeFamily quad,
            //                                       unsigned order, unsigned quadNumNodes) {
            //     return interp == LagrangeNodeFamily::GLL
            //            && quad == QuadratureNodeFamily::GaussLobatto
            //            && quadNumNodes == order + 1;
            // }
            // gllMassShortcutActive_m =
            //     gllMassShortcutApplicable(interp, quad, Order, QuadNumNodes);
        }

        ElementType refElement_m;
        QuadratureType quadrature_m;
        LagrangeType lagrangeSpace_m;
        // bool gllMassShortcutActive_m = false;
    };

}  // namespace ippl

#endif
