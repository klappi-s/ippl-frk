//
// Multigrid preconditioner adapter for FEMContainer (Track B).
//
// P1 Lagrange only: vertex DOFs align with a uniform Cartesian grid. The
// geometric V-cycle runs on scalar vertex Fields; the fine-level operator and
// Jacobi smoother use the FEM matrix-free ops.
//

#ifndef IPPL_MULTIGRID_FEMCONTAINER_H
#define IPPL_MULTIGRID_FEMCONTAINER_H

#include "FEM/Entity.h"
#include "LinearSolvers/Multigrid.h"
#include "LinearSolvers/Preconditioner.h"

namespace ippl {

    namespace detail {
        template <typename FEMContainerT>
        struct fem_multigrid_scalar_field {
            using type =
                Field<typename FEMContainerT::value_type, FEMContainerT::dim, typename FEMContainerT::Mesh_t,
                      Cell>;
        };

        template <typename FEMContainerT, unsigned Dim>
        struct fem_multigrid_is_p1_only {
            static constexpr bool value = (FEMContainerT::NEntitys == 1);
        };

        template <typename FEMContainerT>
        void fem_copy_vertices_to_scalar(const FEMContainerT& src,
                                         typename fem_multigrid_scalar_field<FEMContainerT>::type& dst) {
            constexpr unsigned Dim = FEMContainerT::dim;
            const auto srcView       = src.template getView<Vertex<Dim>>();
            auto dstView             = dst.getView();
            const int nghost         = src.getNghost();

            using index_array_type = typename RangePolicy<Dim>::index_array_type;
            ippl::parallel_for(
                "fem_mg: vertices to scalar", dst.getFieldRangePolicy(nghost),
                KOKKOS_LAMBDA(const index_array_type& args) {
                    apply(dstView, args) = apply(srcView, args)[0];
                });
            ippl::fence();
        }

        template <typename FEMContainerT>
        void fem_copy_scalar_to_vertices(
            const typename fem_multigrid_scalar_field<FEMContainerT>::type& src, FEMContainerT& dst) {
            constexpr unsigned Dim = FEMContainerT::dim;
            auto dstView             = dst.template getView<Vertex<Dim>>();
            const auto srcView       = src.getView();
            const int nghost         = dst.getNghost();

            using index_array_type = typename RangePolicy<Dim>::index_array_type;
            ippl::parallel_for(
                "fem_mg: scalar to vertices", dst.template getFieldRangePolicy<Vertex<Dim>>(nghost),
                KOKKOS_LAMBDA(const index_array_type& args) {
                    apply(dstView, args)[0] = apply(srcView, args);
                });
            ippl::fence();
        }

        template <typename FEMContainerT, typename OperatorF, typename InverseDiagF>
        class fem_multigrid_inner
            : public multigrid_preconditioner<
                  typename fem_multigrid_scalar_field<FEMContainerT>::type,
                  std::function<typename fem_multigrid_scalar_field<FEMContainerT>::type(
                      typename fem_multigrid_scalar_field<FEMContainerT>::type&)>> {
            constexpr static unsigned Dim = FEMContainerT::dim;
            using ScalarField             = typename fem_multigrid_scalar_field<FEMContainerT>::type;

            OperatorF fem_op_;
            InverseDiagF inv_diag_;
            FEMContainerT* fem_u_ = nullptr;
            FEMContainerT* fem_res_ = nullptr;
            FEMContainerT* fem_corr_ = nullptr;
            ScalarField* fem_ax_scalar_ = nullptr;
            ScalarField fd_ax_scalar_;
            ScalarField corr_scalar_;

        public:
            fem_multigrid_inner(OperatorF&& fem_op, InverseDiagF&& inv_diag, unsigned pre_smooth_iters,
                                unsigned post_smooth_iters, double omega_jacobi,
                                unsigned min_cells_per_rank_per_dim, bool communication)
                : multigrid_preconditioner<ScalarField, std::function<ScalarField(ScalarField&)>>(
                      [](ScalarField& u) {
                          ScalarField out(u.get_mesh(), u.getLayout(), u.getNghost());
                          out = laplace(u);
                          return out;
                      },
                      pre_smooth_iters, post_smooth_iters, omega_jacobi, min_cells_per_rank_per_dim,
                      communication)
                , fem_op_(std::forward<OperatorF>(fem_op))
                , inv_diag_(std::forward<InverseDiagF>(inv_diag)) {}

            void bind_scratch(FEMContainerT* fem_u, FEMContainerT* fem_res, FEMContainerT* fem_corr,
                              ScalarField* fem_ax_scalar) {
                fem_u_         = fem_u;
                fem_res_       = fem_res;
                fem_corr_      = fem_corr;
                fem_ax_scalar_ = fem_ax_scalar;

                auto& mesh       = fem_u_->get_mesh();
                auto& layout     = fem_u_->getLayout();
                const int nghost = fem_u_->getNghost();
                auto bcs         = fem_u_->getFieldBC();
                fd_ax_scalar_    = ScalarField(mesh, layout, nghost);
                corr_scalar_     = ScalarField(mesh, layout, nghost);
                fd_ax_scalar_.setFieldBC(bcs);
                corr_scalar_.setFieldBC(bcs);
                fem_ax_scalar_->setFieldBC(bcs);
            }

            ScalarField apply_operator(ScalarField& u) override {
                if (this->active_level_ == 0) {
                    fem_copy_scalar_to_vertices(u, *fem_u_);
                    fem_u_->fillHalo();
                    *fem_res_ = fem_op_(*fem_u_);
                    fem_copy_vertices_to_scalar(*fem_res_, *fem_ax_scalar_);
                    return *fem_ax_scalar_;
                }
                fd_ax_scalar_ = laplace(u);
                return fd_ax_scalar_;
            }

            void perform_jacobi_smooth(const size_t level, const unsigned iters) override {
                if (level == 0) {
                    IpplTimings::TimerRef jacobi = IpplTimings::getTimer("smooth_jacobi");
                    IpplTimings::startTimer(jacobi);

                    auto& lev     = this->L_[level];
                    auto& u       = lev.u;
                    const auto& f = lev.f;

                    for (unsigned it = 0; it < iters; ++it) {
                        if (this->communication_)
                            u.fillHalo();

                        ScalarField res = this->residual(u, f);
                        fem_copy_scalar_to_vertices(res, *fem_res_);
                        *fem_corr_ = inv_diag_(*fem_res_);
                        fem_copy_vertices_to_scalar(*fem_corr_, corr_scalar_);
                        u = u + this->omega_ * corr_scalar_;
                    }

                    IpplTimings::stopTimer(jacobi);
                    return;
                }
                multigrid_preconditioner<ScalarField, std::function<ScalarField(ScalarField&)>>::
                    perform_jacobi_smooth(level, iters);
            }
        };
    }  // namespace detail

    template <typename FEMContainerT, typename OperatorF, typename InverseDiagF>
    struct fem_multigrid_preconditioner : public preconditioner<FEMContainerT> {
        constexpr static unsigned Dim = FEMContainerT::dim;
        using ScalarField             = typename detail::fem_multigrid_scalar_field<FEMContainerT>::type;

    public:
        fem_multigrid_preconditioner(OperatorF&& fem_op, InverseDiagF&& inverse_diagonal,
                                     unsigned pre_smooth_iters = 2, unsigned post_smooth_iters = 2,
                                     double omega_jacobi = 0.8,
                                     unsigned min_cells_per_rank_per_dim = 4, bool communication = true)
            : preconditioner<FEMContainerT>("Multigrid")
            , inner_(std::make_unique<detail::fem_multigrid_inner<FEMContainerT, OperatorF, InverseDiagF>>(
                  std::forward<OperatorF>(fem_op), std::forward<InverseDiagF>(inverse_diagonal),
                  pre_smooth_iters, post_smooth_iters, omega_jacobi, min_cells_per_rank_per_dim,
                  communication)) {}

        void operator()(FEMContainerT& b, FEMContainerT& result) override {
            detail::fem_copy_vertices_to_scalar(b, b_scalar_);
            (*inner_)(b_scalar_, result_scalar_);
            detail::fem_copy_scalar_to_vertices(result_scalar_, result);
        }

        void init_fields(FEMContainerT& b) override {
            if (!detail::fem_multigrid_is_p1_only<FEMContainerT, Dim>::value) {
                throw IpplException(
                    "fem_multigrid_preconditioner::init_fields",
                    "Multigrid preconditioner supports P1 Lagrange (vertex-only) elements only.");
            }

            auto& mesh       = b.get_mesh();
            auto& layout     = b.getLayout();
            const int nghost = b.getNghost();
            auto bcs         = b.getFieldBC();

            b_scalar_     = ScalarField(mesh, layout, nghost);
            result_scalar_ = ScalarField(mesh, layout, nghost);
            b_scalar_.setFieldBC(bcs);
            result_scalar_.setFieldBC(bcs);

            fem_u_scratch_    = std::make_unique<FEMContainerT>(mesh, layout, nghost);
            fem_res_scratch_  = std::make_unique<FEMContainerT>(mesh, layout, nghost);
            fem_corr_scratch_ = std::make_unique<FEMContainerT>(mesh, layout, nghost);
            fem_ax_scalar_    = ScalarField(mesh, layout, nghost);
            fem_ax_scalar_.setFieldBC(bcs);

            fem_u_scratch_->setFieldBC(b.getFieldBCTypes());
            fem_res_scratch_->setFieldBC(b.getFieldBCTypes());
            fem_corr_scratch_->setFieldBC(b.getFieldBCTypes());

            inner_->init_fields(b_scalar_);
            inner_->bind_scratch(fem_u_scratch_.get(), fem_res_scratch_.get(), fem_corr_scratch_.get(),
                                 &fem_ax_scalar_);
        }

    private:
        std::unique_ptr<detail::fem_multigrid_inner<FEMContainerT, OperatorF, InverseDiagF>> inner_;
        ScalarField b_scalar_;
        ScalarField result_scalar_;
        std::unique_ptr<FEMContainerT> fem_u_scratch_;
        std::unique_ptr<FEMContainerT> fem_res_scratch_;
        std::unique_ptr<FEMContainerT> fem_corr_scratch_;
        ScalarField fem_ax_scalar_;
    };

}  // namespace ippl

#endif  // IPPL_MULTIGRID_FEMCONTAINER_H
