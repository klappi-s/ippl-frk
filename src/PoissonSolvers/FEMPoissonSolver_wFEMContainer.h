// Class FEMPoissonSolver_wFEMContainer
//   Track B Poisson solver: LagrangeSpace_wfc + FEMContainer + matrix-free CG/PCG.
//
//   Select via params["preconditioned"] = true|false (default: true).
//   When preconditioned, uses ippl::PCG<> + Preconditioner.h (jacobi, newton, chebyshev,
//   richardson, richardson_alt, gauss-seidel, ssor, multigrid (P1 h-MG; P2/P3 p-multigrid).
//   Default preconditioner_type is "ssor".
//   Newton/Chebyshev spectral bounds are estimated via powermethod / adapted_powermethod.

#ifndef IPPL_FEMPOISSONSOLVER_WFC_H
#define IPPL_FEMPOISSONSOLVER_WFC_H

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <string>

#include "LinearSolvers/PCG.h"
#include "LinearSolvers/Preconditioner.h"
#include "LinearSolvers/PreconditionerValidation.h"
#include "LinearSolvers/MultigridFEMContainer_p.h"
#include "Poisson.h"
#include "EvalFunctor.h"
#include "FEM/LagrangeSpace_wFEMContainer.h"

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

        using QuadratureType = GaussLegendreQuadrature<Tlhs, QuadNumNodes, ElementType>;

        using LagrangeType =
            LagrangeSpace_wfc<Tlhs, Dim, Order, ElementType, QuadratureType, FieldLHS, FieldRHS>;

        FEMPoissonSolver_wFEMContainer()
            : Base()
            , refElement_m()
            , quadrature_m(refElement_m)
            , lagrangeSpace_m(*(new MeshType(NDIndex<Dim>(Vector<unsigned, Dim>(0)),
                                             Vector<Tlhs, Dim>(0), Vector<Tlhs, Dim>(0))),
                              refElement_m, quadrature_m) {
            setDefaultParameters();
        }

        FEMPoissonSolver_wFEMContainer(lhs_type& lhs, rhs_type& rhs)
            : Base(lhs, rhs)
            , refElement_m()
            , quadrature_m(refElement_m)
            , lagrangeSpace_m(rhs.get_mesh(), refElement_m, quadrature_m, rhs.getLayout()) {
            static_assert(std::is_floating_point<Tlhs>::value, "Not a floating point type");
            setDefaultParameters();
        }

        void setRhs(rhs_type& rhs) override {
            Base::setRhs(rhs);
            lagrangeSpace_m.initialize(rhs.get_mesh(), rhs.getLayout());
        }

        LagrangeType& getSpace() { return lagrangeSpace_m; }

        void solve() override {
            this->rhs_mp->fillHalo();
            lagrangeSpace_m.evaluateLoadVector(*(this->rhs_mp));

            const Vector<size_t, Dim> zeroNdIndex = Vector<size_t, Dim>(0);

            const auto firstElementVertexPoints =
                lagrangeSpace_m.getElementMeshVertexPoints(zeroNdIndex);

            const Vector<Tlhs, Dim> DPhiInvT =
                refElement_m.getInverseTransposeTransformationJacobian(firstElementVertexPoints);

            const Tlhs absDetDPhi = Kokkos::abs(
                refElement_m.getDeterminantOfTransformationJacobian(firstElementVertexPoints));

            EvalFunctor<Tlhs, Dim, LagrangeType::numElementDOFs> poissonEquationEval(
                DPhiInvT, absDetDPhi);

            const auto bcTypes = (this->rhs_mp)->getFieldBCTypes();
            const FieldBC bcType = bcTypes[0];

            const auto algoOperator = [poissonEquationEval, bcTypes, this](rhs_type field) -> lhs_type {
                field.setFieldBC(bcTypes);
                field.fillHalo();
                return lagrangeSpace_m.evaluateAx(field, poissonEquationEval);
            };

            const auto algoOperatorL = [poissonEquationEval, bcTypes, this](rhs_type field) -> lhs_type {
                field.setFieldBC(bcTypes);
                field.fillHalo();
                return lagrangeSpace_m.evaluateAx_lower(field, poissonEquationEval);
            };

            const auto algoOperatorU = [poissonEquationEval, bcTypes, this](rhs_type field) -> lhs_type {
                field.setFieldBC(bcTypes);
                field.fillHalo();
                return lagrangeSpace_m.evaluateAx_upper(field, poissonEquationEval);
            };

            const auto algoOperatorUL = [poissonEquationEval, bcTypes, this](rhs_type field) -> lhs_type {
                field.setFieldBC(bcTypes);
                field.fillHalo();
                return lagrangeSpace_m.evaluateAx_upperlower(field, poissonEquationEval);
            };

            const auto algoOperatorInvD = [poissonEquationEval, bcTypes, this](rhs_type field) -> lhs_type {
                field.setFieldBC(bcTypes);
                field.fillHalo();
                return lagrangeSpace_m.evaluateAx_inversediag(field, poissonEquationEval);
            };

            const auto algoOperatorD = [poissonEquationEval, bcTypes, this](rhs_type field) -> lhs_type {
                field.setFieldBC(bcTypes);
                field.fillHalo();
                return lagrangeSpace_m.evaluateAx_diag(field, poissonEquationEval);
            };

            if (bcType == CONSTANT_FACE) {
                *(this->rhs_mp) = *(this->rhs_mp) -
                    lagrangeSpace_m.evaluateAx_lift(*(this->rhs_mp), poissonEquationEval);
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
                unsigned mg_min_cells = pcg_preconditioner_defaults::mg_min_cells;

                preconditioner_validation::sanitizeParams(
                    preconditioner_type, warn, level, degree, richardson_iterations, inner, outer,
                    omega, &communication, mg_pre, mg_post, mg_omega, mg_min_cells);

                double alpha = 0.0;
                double beta  = 0.0;
                if (preconditioner_type == "newton" || preconditioner_type == "chebyshev") {
                    lhs_type x0(*(this->lhs_mp));
                    x0 = Tlhs(1);
                    x0.setFieldBC(bcTypes);
                    // Cap power iterations: FEM operator is expensive vs FD laplace.
                    beta = powermethod(algoOperator, x0, /*max_iter=*/200, /*tol=*/1e-3);
                    x0   = Tlhs(1);
                    x0.setFieldBC(bcTypes);
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

                if (preconditioner_type == "multigrid" && Order > 1) {
                    // p-multigrid: build Order→…→P1 chain; P1 bottom = geometric h-MG.
                    auto& mesh   = this->rhs_mp->get_mesh();
                    auto& layout = this->rhs_mp->getLayout();

                    using P1Traits = FiniteElementSpaceTraits<LagrangeSpaceTag, Dim, 1>;
                    using P1Cont   = typename DOFHandler<Tlhs, P1Traits>::FEMContainer_t;
                    using P1Space =
                        LagrangeSpace_wfc<Tlhs, Dim, 1, ElementType, QuadratureType, P1Cont, P1Cont>;
                    EvalFunctor<Tlhs, Dim, P1Space::numElementDOFs> eval1(DPhiInvT, absDetDPhi);

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

                    pcg_algo_m.setOperator(algoOperator);
                    pcg_algo_m(*(this->lhs_mp), *(this->rhs_mp), this->params_m);
                } else {
                    pcg_algo_m.setPreconditioner(algoOperator, algoOperatorL, algoOperatorU,
                                                 algoOperatorUL, algoOperatorInvD, algoOperatorD,
                                                 alpha, beta, preconditioner_type, level, degree,
                                                 richardson_iterations, inner, outer, omega, mg_pre,
                                                 mg_post, mg_omega, mg_min_cells);

                    pcg_algo_m.setOperator(algoOperator);
                    pcg_algo_m(*(this->lhs_mp), *(this->rhs_mp), this->params_m);
                }
                iteration_count_m = pcg_algo_m.getIterationCount();
                residue_m         = pcg_algo_m.getResidue();
            } else {
                cg_algo_m.setOperator(algoOperator);
                cg_algo_m(*(this->lhs_mp), *(this->rhs_mp), this->params_m);
                iteration_count_m = cg_algo_m.getIterationCount();
                residue_m         = cg_algo_m.getResidue();
            }

            (this->lhs_mp)->fillHalo();
            IpplTimings::stopTimer(solveTimer);
        }

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
            this->params_m.add("preconditioner_type", "ssor");
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
        }

        ElementType refElement_m;
        QuadratureType quadrature_m;
        LagrangeType lagrangeSpace_m;
    };

}  // namespace ippl

#endif
