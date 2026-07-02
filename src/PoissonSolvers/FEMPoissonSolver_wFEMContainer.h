// Class FEMPoissonSolver_wFEMContainer
//   Track B Poisson solver: LagrangeSpace_wfc + FEMContainer + matrix-free CG.
//   Supports plain (unpreconditioned) and Jacobi-preconditioned CG.
//   Select via params["preconditioned"] = true|false (default: true).

#ifndef IPPL_FEMPOISSONSOLVER_WFC_H
#define IPPL_FEMPOISSONSOLVER_WFC_H

#include "LinearSolvers/PCG.h"
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

        // Plain (unpreconditioned) CG, specialised for FEMContainer
        using CGSolverAlgorithm_t =
            CG<lhs_type, lhs_type, lhs_type, lhs_type, lhs_type, FieldLHS, FieldRHS>;

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

            if (bcType == CONSTANT_FACE) {
                *(this->rhs_mp) = *(this->rhs_mp) -
                    lagrangeSpace_m.evaluateAx_lift(*(this->rhs_mp), poissonEquationEval);
            }

            const bool usePrecon = this->params_m.template get<bool>("preconditioned");

            static IpplTimings::TimerRef solveTimer = IpplTimings::getTimer("pcg");
            IpplTimings::startTimer(solveTimer);

            if (usePrecon) {
                // ---- Jacobi-preconditioned CG -----------------------------------
                // Compute diagonal of A; boundary DOFs are pre-set to 1 in evaluateAx_diag
                lhs_type diag = lagrangeSpace_m.evaluateAx_diag(*(this->lhs_mp), poissonEquationEval);

                lhs_type& x = *(this->lhs_mp);
                rhs_type& b = *(this->rhs_mp);

                const int    maxIter = this->params_m.template get<int>("max_iterations");
                const Tlhs   tol    = this->params_m.template get<Tlhs>("tolerance");

                // r = b - A x
                lhs_type r = b - algoOperator(x);
                r.setFieldBC(bcTypes);

                // s = M^{-1} r  (element-wise: s_i = r_i / diag_i)
                lhs_type s = applyJacobi(r, diag, bcTypes);

                // d = s,  delta = (r, s)
                lhs_type d = s.deepCopy();
                d.setFieldBC(bcTypes);

                Tlhs delta    = innerProduct(r, s);
                const Tlhs delta0 = delta;
                residue_m         = Kokkos::sqrt(Kokkos::abs(delta));
                const Tlhs abstol = tol * norm(b);

                lhs_type q(x.get_mesh(), x.getLayout());
                iteration_count_m = 0;

                while (iteration_count_m < maxIter && residue_m > abstol) {
                    q = algoOperator(d);

                    Tlhs alpha = delta / innerProduct(d, q);
                    x         = x + alpha * d;
                    r         = r - alpha * q;
                    r.setFieldBC(bcTypes);

                    s = applyJacobi(r, diag, bcTypes);

                    Tlhs delta_new = innerProduct(r, s);
                    Tlhs beta      = delta_new / delta;
                    delta          = delta_new;

                    residue_m = Kokkos::sqrt(Kokkos::abs(delta));
                    d         = s + beta * d;
                    d.setFieldBC(bcTypes);

                    ++iteration_count_m;
                }
                (void)delta0;
                // -----------------------------------------------------------------
            } else {
                // ---- Plain (unpreconditioned) CG --------------------------------
                cg_algo_m.setOperator(algoOperator);
                cg_algo_m(*(this->lhs_mp), *(this->rhs_mp), this->params_m);
                iteration_count_m = cg_algo_m.getIterationCount();
                residue_m         = cg_algo_m.getResidue();
                // -----------------------------------------------------------------
            }

            (this->lhs_mp)->fillHalo();
            IpplTimings::stopTimer(solveTimer);
        }

        int  getIterationCount() const { return iteration_count_m; }
        Tlhs getResidue()        const { return residue_m; }

        template <typename F>
        Tlhs getL2Error(const F& analytic) {
            return this->lagrangeSpace_m.computeErrorL2(*(this->lhs_mp), analytic);
        }

        Tlhs getAvg(bool Vol = false) {
            Tlhs avg = this->lagrangeSpace_m.computeAvg(*(this->lhs_mp));
            if (Vol) {
                lhs_type unit((this->lhs_mp)->get_mesh(), (this->lhs_mp)->getLayout());
                unit = 1.0;
                Tlhs vol = this->lagrangeSpace_m.computeAvg(unit);
                return avg / vol;
            }
            return avg;
        }

    protected:
        // Plain CG (used when preconditioned == false)
        CGSolverAlgorithm_t cg_algo_m;

        // Stats (populated by both paths)
        int  iteration_count_m = 0;
        Tlhs residue_m         = 0;

        virtual void setDefaultParameters() override {
            this->params_m.add("max_iterations", 1000);
            this->params_m.add("tolerance", (Tlhs)1e-13);
            // Default: use Jacobi preconditioner (mirrors alpine FEM_PRECON behaviour)
            this->params_m.add("preconditioned", true);
        }

        // Apply Jacobi preconditioner: s = r / diag  (element-wise DOFArray division per entity view).
        // Boundary DOFs in diag are 1 (set by evaluateAx_diag), so s[boundary] = r[boundary].
        lhs_type applyJacobi(const lhs_type& r, const lhs_type& diag,
                             const std::array<FieldBC, 2 * Dim>& bcTypes) const {
            lhs_type s = r.deepCopy();

            using DOFHandler_t = typename LagrangeType::DOFHandler_t;
            constexpr size_t numTypes = DOFHandler_t::numEntityTypes;

            [&]<size_t... Is>(std::index_sequence<Is...>) {
                ([&] {
                    using EntityType = std::tuple_element_t<Is, typename DOFHandler_t::EntityTypes>;
                    auto s_view    = s.template getView<EntityType>();
                    auto diag_view = diag.template getView<EntityType>();

                    // Use the entity-type-specific field range policy (owned cells only, nghost=0)
                    auto policy = s.template getFieldRangePolicy<EntityType>(0);

                    Kokkos::parallel_for(
                        "Jacobi::applyJacobi",
                        policy,
                        KOKKOS_LAMBDA(const auto&... idx) {
                            auto& sv       = s_view(idx...);
                            const auto& dv = diag_view(idx...);
                            // DOFArray element-wise division (DOFArray::operator/(DOFArray))
                            sv = sv / dv;
                        });
                }(), ...);
            }(std::make_index_sequence<numTypes>{});

            Kokkos::fence();
            s.setFieldBC(bcTypes);
            return s;
        }

        ElementType  refElement_m;
        QuadratureType quadrature_m;
        LagrangeType lagrangeSpace_m;
    };

}  // namespace ippl

#endif
