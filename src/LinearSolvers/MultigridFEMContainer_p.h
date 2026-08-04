//
// p-multigrid preconditioner for higher-order Lagrange FEMContainer (Track B).
// Same mesh; coarsen polynomial degree; P1 bottom uses fem_multigrid_preconditioner.
//

#ifndef IPPL_MULTIGRID_FEMCONTAINER_P_H
#define IPPL_MULTIGRID_FEMCONTAINER_P_H

#include <functional>
#include <memory>

#include "LinearSolvers/FEMOrderTransfer.h"
#include "LinearSolvers/MultigridFEMContainer.h"
#include "LinearSolvers/Preconditioner.h"

namespace ippl {

    /**
     * @brief One p-multigrid level: smooth on Fine, restrict → coarse precon → prolong, post-smooth.
     *
     * CoarsePrecon is typically fem_pmultigrid_preconditioner (next lower order) or
     * fem_multigrid_preconditioner (P1 h-MG).
     */
    template <typename FineContainer, typename CoarseContainer, typename CoarsePrecon,
              unsigned OrderFine, unsigned OrderCoarse>
    class fem_pmultigrid_preconditioner : public preconditioner<FineContainer> {
        static_assert(OrderFine > OrderCoarse, "OrderFine must exceed OrderCoarse");

    public:
        using T                       = typename FineContainer::value_type;
        static constexpr unsigned Dim = FineContainer::dim;
        using Transfer                = FEMOrderTransfer<T, Dim, OrderFine, OrderCoarse>;
        using HandlerFine             = typename Transfer::HandlerFine;
        using HandlerCoarse           = typename Transfer::HandlerCoarse;
        using OperatorF               = std::function<FineContainer(FineContainer)>;
        using InverseDiagF            = std::function<FineContainer(FineContainer)>;

        fem_pmultigrid_preconditioner(OperatorF fine_op, InverseDiagF fine_inv_diag,
                                      HandlerFine handler_fine, HandlerCoarse handler_coarse,
                                      Kokkos::View<size_t*> element_indices,
                                      std::unique_ptr<CoarsePrecon> coarse_precon,
                                      unsigned pre_smooth = 2, unsigned post_smooth = 2,
                                      double omega = 0.8)
            : preconditioner<FineContainer>("Multigrid")
            , fine_op_(std::move(fine_op))
            , fine_inv_diag_(std::move(fine_inv_diag))
            , handler_fine_(std::move(handler_fine))
            , handler_coarse_(std::move(handler_coarse))
            , element_indices_(std::move(element_indices))
            , coarse_precon_(std::move(coarse_precon))
            , transfer_()
            , nu1_(pre_smooth)
            , nu2_(post_smooth)
            , omega_(omega) {}

        void init_fields(FineContainer& b) override {
            auto& mesh       = b.get_mesh();
            auto& layout     = b.getLayout();
            const int nghost = b.getNghost();
            const auto bc    = b.getFieldBCTypes();

            u_fine_    = std::make_unique<FineContainer>(mesh, layout, nghost);
            f_fine_    = std::make_unique<FineContainer>(mesh, layout, nghost);
            r_fine_    = std::make_unique<FineContainer>(mesh, layout, nghost);
            corr_fine_ = std::make_unique<FineContainer>(mesh, layout, nghost);
            u_fine_->setFieldBC(bc);
            f_fine_->setFieldBC(bc);
            r_fine_->setFieldBC(bc);
            corr_fine_->setFieldBC(bc);

            f_coarse_ = std::make_unique<CoarseContainer>(mesh, layout, nghost);
            u_coarse_ = std::make_unique<CoarseContainer>(mesh, layout, nghost);
            f_coarse_->setFieldBC(bc);
            u_coarse_->setFieldBC(bc);

            coarse_precon_->init_fields(*f_coarse_);
            initialized_ = true;
        }

        void operator()(FineContainer& b, FineContainer& result) override {
            if (!initialized_) {
                init_fields(b);
            }
            *f_fine_ = b;
            *u_fine_ = T(0);
            vcycle();
            result.deepCopyFrom(*u_fine_);
        }

    private:
        void smooth(unsigned iters) {
            for (unsigned it = 0; it < iters; ++it) {
                u_fine_->fillHalo();
                *r_fine_    = *f_fine_ - fine_op_(*u_fine_);
                *corr_fine_ = fine_inv_diag_(*r_fine_);
                *u_fine_    = *u_fine_ + (omega_ * (*corr_fine_));
                u_fine_->setFieldBC(u_fine_->getFieldBCTypes());
            }
        }

        void vcycle() {
            smooth(nu1_);

            u_fine_->fillHalo();
            *r_fine_ = *f_fine_ - fine_op_(*u_fine_);
            transfer_.restrict(*r_fine_, *f_coarse_, handler_fine_, handler_coarse_,
                               element_indices_);
            f_coarse_->setFieldBC(f_coarse_->getFieldBCTypes());

            *u_coarse_ = T(0);
            (*coarse_precon_)(*f_coarse_, *u_coarse_);

            FineContainer prolong_corr(u_fine_->get_mesh(), u_fine_->getLayout(),
                                       u_fine_->getNghost());
            prolong_corr.setFieldBC(u_fine_->getFieldBCTypes());
            transfer_.prolong(*u_coarse_, prolong_corr, handler_fine_, handler_coarse_,
                              element_indices_);
            *u_fine_ = *u_fine_ + prolong_corr;
            u_fine_->setFieldBC(u_fine_->getFieldBCTypes());

            smooth(nu2_);
        }

        OperatorF fine_op_;
        InverseDiagF fine_inv_diag_;
        HandlerFine handler_fine_;
        HandlerCoarse handler_coarse_;
        Kokkos::View<size_t*> element_indices_;
        std::unique_ptr<CoarsePrecon> coarse_precon_;
        Transfer transfer_;
        unsigned nu1_, nu2_;
        double omega_;
        bool initialized_ = false;

        std::unique_ptr<FineContainer> u_fine_, f_fine_, r_fine_, corr_fine_;
        std::unique_ptr<CoarseContainer> f_coarse_, u_coarse_;
    };

}  // namespace ippl

#endif  // IPPL_MULTIGRID_FEMCONTAINER_P_H
