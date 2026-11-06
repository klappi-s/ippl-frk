#ifndef IPPL_LANDAU_DAMPING_MANAGER_H
#define IPPL_LANDAU_DAMPING_MANAGER_H

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

#include "AlpineManager.h"
#include "FieldContainer.hpp"
#include "FieldSolver.hpp"
#include "LoadBalancer.hpp"
#include "Manager/BaseManager.h"
#include "ParticleContainer.hpp"
#include "Random/Distribution.h"
#include "Random/InverseTransformSampling.h"
#include "Random/NormalDistribution.h"
#include "Random/Randn.h"

using view_type = typename ippl::detail::ViewType<ippl::Vector<double, Dim>, 1>::view_type;

// define functions used in sampling particles
struct CustomDistributionFunctions {
    struct CDF {
        KOKKOS_INLINE_FUNCTION double operator()(double x, unsigned int d,
                                                 const double* params_p) const {
            return x
                   + (params_p[d * 2 + 0] / params_p[d * 2 + 1])
                         * Kokkos::sin(params_p[d * 2 + 1] * x);
        }
    };

    struct PDF {
        KOKKOS_INLINE_FUNCTION double operator()(double x, unsigned int d,
                                                 double const* params_p) const {
            return 1.0 + params_p[d * 2 + 0] * Kokkos::cos(params_p[d * 2 + 1] * x);
        }
    };

    struct Estimate {
        KOKKOS_INLINE_FUNCTION double operator()(double u, unsigned int d,
                                                 double const* params_p) const {
            return u + params_p[d] * 0.;
        }
    };
};

template <typename T, unsigned Dim>
class LandauDampingManager : public AlpineManager<T, Dim> {
    // Opt-in measurements around the normal application/PoissonCG path.
    bool benchmark_m = false;
    std::string benchmarkOutput_m;
    double benchmarkTolerance_m = 1e-10, benchmarkDt_m = .05, benchmarkSetupSeconds_m = 0;
    int benchmarkMaxIterations_m = 10000;
    unsigned benchmarkSolve_m = 0;
    Kokkos::Timer benchmarkStepTimer_m;
    Field_t<Dim> benchmarkResidual_m;
    std::ostringstream benchmarkSolves_m, benchmarkSteps_m, benchmarkPhysics_m;

    void solveField() {
        if (!benchmark_m) {
            this->fsolver_m->runSolver();
            return;
        }
        Kokkos::fence();
        Kokkos::Timer timer;
        this->fsolver_m->runSolver();
        Kokkos::fence();
        const double seconds = timer.seconds();
        timer.reset();
        auto& rho = this->fcontainer_m->getRho();
        auto& phi = this->fcontainer_m->getPhi();
        auto& solver = std::get<CGSolver_t<T, Dim>>(this->fsolver_m->getSolver());
        // A=-laplace. Allocate the verification field once during initialization.
        benchmarkResidual_m = rho + ippl::laplace(phi);
        const double rhsNorm = ippl::norm(rho);
        const double relative = ippl::norm(benchmarkResidual_m) / (rhsNorm > 0 ? rhsNorm : 1);
        Kokkos::fence();
        const double verification = timer.seconds();
        const bool success = std::isfinite(relative)
                             && relative <= std::max(20 * benchmarkTolerance_m, 5e-11)
                             && solver.getIterationCount() < benchmarkMaxIterations_m;
        const std::string method = this->solver_m == "CG" ? "plain" : this->preconditioner_params_m.at(0);
        benchmarkSolves_m << (benchmarkSolve_m == 0 ? "initial" : "step") << ',' << method
            << ',' << benchmarkSolve_m << ',' << ippl::Comm->rank() << ','
            << solver.getIterationCount() << ',' << solver.getResidue() << ',' << relative
            << ',' << rhsNorm << ',' << seconds << ',' << verification << ','
            << (benchmarkSolve_m == 0 ? benchmarkSetupSeconds_m : 0) << ','
            << solver.getPreconditionerSetupCount() << ','
            << solver.getPreconditionerHierarchyLevels() << ','
            << solver.getPreconditionerCoarsestLocalExtent() << ',' << success << '\n';
        ++benchmarkSolve_m;
        if (!success) {
            flushBenchmark();
            throw std::runtime_error("Alpine fresh residual check failed at solve "
                                     + std::to_string(benchmarkSolve_m - 1));
        }
    }

public:
    using ParticleContainer_t = ParticleContainer<T, Dim>;
    using FieldContainer_t    = FieldContainer<T, Dim>;
    using FieldSolver_t       = FieldSolver<T, Dim>;
    using LoadBalancer_t      = LoadBalancer<T, Dim>;

    LandauDampingManager(size_type totalP_, int nt_, Vector_t<int, Dim>& nr_, double lbt_,
                         std::string& solver_, std::string& stepMethod_)
        : AlpineManager<T, Dim>(totalP_, nt_, nr_, lbt_, solver_, stepMethod_) {}

    LandauDampingManager(size_type totalP_, int nt_, Vector_t<int, Dim>& nr_, double lbt_,
                         std::string& solver_, std::string& stepMethod_,
                         std::vector<std::string> preconditioner_params_)
        : AlpineManager<T, Dim>(totalP_, nt_, nr_, lbt_, solver_, stepMethod_,
                                preconditioner_params_) {}

    ~LandauDampingManager() {}

    void enableBenchmark(const std::string& output, double tolerance, int maxIterations, double dt) {
        if ((this->solver_m != "CG" && this->solver_m != "PCG") || this->lbt_m != 1.0
            || !(tolerance > 0) || !std::isfinite(tolerance) || maxIterations < 1
            || !(dt > 0) || !std::isfinite(dt))
            throw std::runtime_error("Benchmark requires CG/PCG, lbthres=1 and positive solver/dt controls.");
        benchmarkOutput_m = std::filesystem::absolute(output).string();
        int error = 0;
        if (ippl::Comm->rank() == 0) {
            try {
                if (std::filesystem::exists(benchmarkOutput_m + "/configuration.txt"))
                    error = 1;
                else
                    std::filesystem::create_directories(benchmarkOutput_m);
            } catch (...) { error = 1; }
        }
        MPI_Bcast(&error, 1, MPI_INT, 0, ippl::Comm->getCommunicator());
        if (error)
            throw std::runtime_error("Benchmark output exists or cannot be created: " + output);
        ippl::Comm->barrier();
        std::filesystem::current_path(benchmarkOutput_m); // Isolate existing Alpine diagnostic files.
        benchmark_m = true;
        benchmarkTolerance_m = tolerance;
        benchmarkMaxIterations_m = maxIterations;
        benchmarkDt_m = dt;
        IpplTimings::setSampleLimit(0);
        benchmarkSolves_m << std::setprecision(17)
            << "phase,method,solve,rank,iterations,recursive_residual,fresh_relative_residual,"
               "rhs_norm,solve_seconds,verification_seconds,solver_setup_seconds,"
               "preconditioner_setups,preconditioner_levels,preconditioner_coarsest_local_extent,"
               "success\n";
        benchmarkSteps_m << std::setprecision(17) << "step,rank,stage,seconds\n";
        benchmarkPhysics_m << std::setprecision(17)
            << "step,time,Ex_field_energy,Ex_max_norm,mean_potential,mean_charge_density\n";
    }

    void flushBenchmark() {
        if (!benchmark_m) return;
        const std::string rank = std::to_string(ippl::Comm->rank());
        std::ofstream(benchmarkOutput_m + "/solves-rank" + rank + ".csv") << benchmarkSolves_m.str();
        std::ofstream(benchmarkOutput_m + "/steps-rank" + rank + ".csv") << benchmarkSteps_m.str();
        if (ippl::Comm->rank() == 0)
            std::ofstream(benchmarkOutput_m + "/physics.csv") << benchmarkPhysics_m.str();
    }

    void pre_step() override {
        if (benchmark_m) {
            Kokkos::fence();
            benchmarkStepTimer_m.reset();
        }
        AlpineManager<T, Dim>::pre_step();
    }

    void post_step() override {
        AlpineManager<T, Dim>::post_step();
        if (benchmark_m) {
            Kokkos::fence();
            benchmarkSteps_m << this->it_m << ',' << ippl::Comm->rank()
                             << ",step_total," << benchmarkStepTimer_m.seconds() << '\n';
        }
    }

    void pre_run() override {
        Inform m("Pre Run");

        const double pi = Kokkos::numbers::pi_v<T>;

        if (this->solver_m == "OPEN") {
            throw IpplException("LandauDamping",
                                "Open boundaries solver incompatible with this simulation!");
        }

        for (unsigned i = 0; i < Dim; i++) {
            this->domain_m[i] = ippl::Index(this->nr_m[i]);
        }

        this->decomp_m.fill(true);
        this->kw_m    = 0.5;
        this->alpha_m = 0.05;
        this->rmin_m  = 0.0;
        this->rmax_m  = 2 * pi / this->kw_m;

        bool isFEM = ((this->getSolver() == "FEM") || (this->getSolver() == "FEM_PRECON"));

        Vector<int, Dim> nElements = this->nr_m - 1;
        if (isFEM) {
            this->hr_m = this->rmax_m / nElements;
        } else {
            this->hr_m = this->rmax_m / this->nr_m;
        }

        // Q = -\int\int f dx dv
        this->Q_m =
            std::reduce(this->rmax_m.begin(), this->rmax_m.end(), -1., std::multiplies<double>());
        this->origin_m = this->rmin_m;
        this->dt_m   = std::min(.05, 0.5 * *std::min_element(this->hr_m.begin(), this->hr_m.end()));
        if (benchmark_m) this->dt_m = benchmarkDt_m;
        this->it_m   = 0;
        this->time_m = 0.0;

        m << "Discretization:" << endl
          << "nt " << this->nt_m << " Np= " << this->totalP_m << " grid = " << this->nr_m << endl;

        this->isAllPeriodic_m = true;

        this->setFieldContainer(std::make_shared<FieldContainer_t>(
            this->hr_m, this->rmin_m, this->rmax_m, this->decomp_m, this->domain_m, this->origin_m,
            this->isAllPeriodic_m));

        this->setParticleContainer(std::make_shared<ParticleContainer_t>(
            this->fcontainer_m->getMesh(), this->fcontainer_m->getFL(), isFEM));

        this->fcontainer_m->initializeFields(this->solver_m);

        if ((this->getSolver() == "PCG") || (this->getSolver() == "FEM_PRECON")) {
            this->setFieldSolver(std::make_shared<FieldSolver_t>(
                this->solver_m, &this->fcontainer_m->getRho(), &this->fcontainer_m->getE(),
                &this->fcontainer_m->getPhi(), this->preconditioner_params_m));
        } else {
            this->setFieldSolver(std::make_shared<FieldSolver_t>(
                this->solver_m, &this->fcontainer_m->getRho(), &this->fcontainer_m->getE(),
                &this->fcontainer_m->getPhi()));
        }

        Kokkos::fence();
        Kokkos::Timer setupTimer;
        this->fsolver_m->initSolver();
        Kokkos::fence();
        if (benchmark_m) {
            benchmarkSetupSeconds_m = setupTimer.seconds();
            auto& solver = std::get<CGSolver_t<T, Dim>>(this->fsolver_m->getSolver());
            ippl::ParameterList parameters;
            parameters.add("tolerance", benchmarkTolerance_m);
            parameters.add("max_iterations", benchmarkMaxIterations_m);
            solver.mergeParameters(parameters);
            std::static_pointer_cast<FieldSolver_t>(this->fsolver_m)->setLogSolves(false);
            this->fcontainer_m->getPhi() = 0;
            benchmarkResidual_m.initialize(this->fcontainer_m->getMesh(), this->fcontainer_m->getFL());
        }

        this->setLoadBalancer(std::make_shared<LoadBalancer_t>(
            this->lbt_m, this->fcontainer_m, this->pcontainer_m, this->fsolver_m));

        initializeParticles();

        if (!benchmark_m) {
            static IpplTimings::TimerRef DummySolveTimer = IpplTimings::getTimer("solveWarmup");
            IpplTimings::startTimer(DummySolveTimer);
            this->fcontainer_m->getRho() = 0.0;
            this->fsolver_m->runSolver();
            IpplTimings::stopTimer(DummySolveTimer);
        } else if (ippl::Comm->rank() == 0) {
            std::ofstream out(benchmarkOutput_m + "/configuration.txt");
            out << std::setprecision(17)
                << "application=alpine\nbackend=" << Kokkos::DefaultExecutionSpace::name()
                << "\ndimension=" << Dim << "\nranks=" << ippl::Comm->size()
                << "\nparticles=" << this->totalP_m << "\nsteps=" << this->nt_m
                << "\ndt=" << this->dt_m << "\nalpha=" << this->alpha_m
                << "\nseed=42\ninitial_condition=alpine_product\ninitial_guess=previous"
                << "\nload_balancing=disabled\ntimer_recording=summary\ntimer_sample_limit=0"
                << "\npreconditioner_lifecycle_metrics=1"
                << "\ntolerance=" << benchmarkTolerance_m
                << "\nmax_iterations=" << benchmarkMaxIterations_m
                << "\npic_method=" << (this->solver_m == "CG" ? "plain" : this->preconditioner_params_m.at(0))
                << "\nsolver=" << this->solver_m << "\nsolver_setup_seconds=" << benchmarkSetupSeconds_m
                << "\nresidual_policy=" << (this->solver_m == "CG" ? "l2_relative_rhs" : "preconditioned_relative_initial")
                << "\nwarmup_solves=0\n";
            for (unsigned d = 0; d < Dim; ++d)
                out << "cells_" << d << '=' << this->nr_m[d] << "\nlength_" << d << '=' << this->rmax_m[d] << '\n';
            for (size_t i = 0; i < this->preconditioner_params_m.size(); ++i)
                out << "preconditioner_arg_" << i << '=' << this->preconditioner_params_m[i] << '\n';
        }

        this->par2grid();

        static IpplTimings::TimerRef SolveTimer = IpplTimings::getTimer("solve");
        IpplTimings::startTimer(SolveTimer);

        solveField();

        IpplTimings::stopTimer(SolveTimer);

        this->grid2par();

        this->dump();

        m << "Done";
    }

    void initializeParticles() {
        Inform m("Initialize Particles");

        auto* mesh = &this->fcontainer_m->getMesh();
        auto* FL   = &this->fcontainer_m->getFL();
        using DistR_t =
            ippl::random::Distribution<double, Dim, 2 * Dim, CustomDistributionFunctions>;
        double parR[2 * Dim];
        for (unsigned int i = 0; i < Dim; i++) {
            parR[i * 2]     = this->alpha_m;
            parR[i * 2 + 1] = this->kw_m[i];
        }
        DistR_t distR(parR);

        bool isFEM = ((this->getSolver() == "FEM") || (this->getSolver() == "FEM_PRECON"));

        Vector_t<double, Dim> kw                         = this->kw_m;
        Vector_t<double, Dim> hr                         = this->hr_m;
        Vector_t<double, Dim> origin                     = this->origin_m;
        static IpplTimings::TimerRef domainDecomposition = IpplTimings::getTimer("loadBalance");
        if ((this->lbt_m != 1.0) && (ippl::Comm->size() > 1)) {
            m << "Starting first repartition" << endl;
            IpplTimings::startTimer(domainDecomposition);
            this->isFirstRepartition_m     = true;
            const ippl::NDIndex<Dim>& lDom = FL->getLocalNDIndex();
            const int nghost               = this->fcontainer_m->getRho().getNghost();
            auto rhoview                   = this->fcontainer_m->getRho().getView();

            using index_array_type = typename ippl::RangePolicy<Dim>::index_array_type;
            ippl::parallel_for(
                "Assign initial rho based on PDF",
                this->fcontainer_m->getRho().getFieldRangePolicy(),
                KOKKOS_LAMBDA(const index_array_type& args) {
                    // local to global index conversion
                    Vector_t<double, Dim> xvec =
                        (args + lDom.first() - nghost + 0.5 * (!isFEM)) * hr + origin;

                    // ippl::apply accesses the view at the given indices and obtains a
                    // reference; see src/Expression/IpplOperations.h
                    ippl::apply(rhoview, args) = distR.getFullPdf(xvec);
                });
            Kokkos::fence();

            this->loadbalancer_m->initializeORB(FL, mesh);
            this->loadbalancer_m->repartition(FL, mesh, this->isFirstRepartition_m);
            IpplTimings::stopTimer(domainDecomposition);
        }

        static IpplTimings::TimerRef particleCreation = IpplTimings::getTimer("particlesCreation");
        IpplTimings::startTimer(particleCreation);

        // Sample particle positions:
        ippl::detail::RegionLayout<double, Dim, Mesh_t<Dim>> rlayout;
        rlayout = ippl::detail::RegionLayout<double, Dim, Mesh_t<Dim>>(*FL, *mesh, isFEM);

        // unsigned int
        size_type totalP = this->totalP_m;
        int seed         = 42;
        Kokkos::Random_XorShift64_Pool<> rand_pool64((size_type)(seed + 100 * ippl::Comm->rank()));

        using samplingR_t =
            ippl::random::InverseTransformSampling<double, Dim, Kokkos::DefaultExecutionSpace,
                                                   DistR_t>;
        Vector_t<double, Dim> rmin = this->rmin_m;
        Vector_t<double, Dim> rmax = this->rmax_m;
        samplingR_t samplingR(distR, rmax, rmin, rlayout, totalP);
        size_type nlocal = samplingR.getLocalSamplesNum();

        this->pcontainer_m->create(nlocal);

        view_type R = this->pcontainer_m->R.getView();
        samplingR.generate(R, rand_pool64);

        view_type P = this->pcontainer_m->P.getView();

        double mu[Dim];
        double sd[Dim];
        for (unsigned int i = 0; i < Dim; i++) {
            mu[i] = 0.0;
            sd[i] = 1.0;
        }
        Kokkos::parallel_for(nlocal, ippl::random::randn<double, Dim>(P, rand_pool64, mu, sd));
        Kokkos::fence();
        ippl::Comm->barrier();

        IpplTimings::stopTimer(particleCreation);

        this->pcontainer_m->q = this->Q_m / totalP;

        // For FEM need an update due to node-centering, as periodic BCs mean
        // that a particle at R=0 is equivalent to R=1 so it could be on the
        // wrong rank and needs to be sent over.
        if (isFEM) {
            this->pcontainer_m->update();
        }
        m << "particles created and initial conditions assigned " << endl;
    }

    void advance() override {
        if (this->stepMethod_m == "LeapFrog") {
            LeapFrogStep();
        } else {
            throw IpplException(TestName, "Step method is not set/recognized!");
        }
    }

    void LeapFrogStep() {
        // LeapFrog time stepping https://en.wikipedia.org/wiki/Leapfrog_integration
        // Here, we assume a constant charge-to-mass ratio of -1 for
        // all the particles hence eliminating the need to store mass as
        // an attribute
        static IpplTimings::TimerRef PTimer              = IpplTimings::getTimer("pushVelocity");
        static IpplTimings::TimerRef RTimer              = IpplTimings::getTimer("pushPosition");
        static IpplTimings::TimerRef updateTimer         = IpplTimings::getTimer("update");
        static IpplTimings::TimerRef domainDecomposition = IpplTimings::getTimer("loadBalance");
        static IpplTimings::TimerRef SolveTimer          = IpplTimings::getTimer("solve");

        double dt                               = this->dt_m;
        std::shared_ptr<ParticleContainer_t> pc = this->pcontainer_m;
        std::shared_ptr<FieldContainer_t> fc    = this->fcontainer_m;

        IpplTimings::startTimer(PTimer);
        pc->P = pc->P - 0.5 * dt * pc->E;
        IpplTimings::stopTimer(PTimer);

        // drift
        IpplTimings::startTimer(RTimer);
        pc->R = pc->R + dt * pc->P;
        IpplTimings::stopTimer(RTimer);

        // Since the particles have moved spatially update them to correct processors
        IpplTimings::startTimer(updateTimer);
        pc->update();
        IpplTimings::stopTimer(updateTimer);

        size_type totalP        = this->totalP_m;
        int it                  = this->it_m;
        bool isFirstRepartition = false;
        if (this->loadbalancer_m->balance(totalP, it + 1)) {
            IpplTimings::startTimer(domainDecomposition);
            auto* mesh = &fc->getRho().get_mesh();
            auto* FL   = &fc->getFL();
            this->loadbalancer_m->repartition(FL, mesh, isFirstRepartition);
            IpplTimings::stopTimer(domainDecomposition);
        }

        // scatter the charge onto the underlying grid
        this->par2grid();

        // Field solve
        IpplTimings::startTimer(SolveTimer);
        solveField();
        IpplTimings::stopTimer(SolveTimer);

        // gather E field
        this->grid2par();

        // kick
        IpplTimings::startTimer(PTimer);
        pc->P = pc->P - 0.5 * dt * pc->E;
        IpplTimings::stopTimer(PTimer);
    }

    void dump() override {
        static IpplTimings::TimerRef dumpDataTimer = IpplTimings::getTimer("dumpData");
        IpplTimings::startTimer(dumpDataTimer);

        if ((this->getSolver() == "FEM") || (this->getSolver() == "FEM_PRECON")) {
            // When using FEM, we only have E on particles
            // so we use the dump function which computes the
            // energy using the particles instead of the field.
            dumpLandau();
        } else {
            dumpLandau(this->fcontainer_m->getE().getView());
        }

        IpplTimings::stopTimer(dumpDataTimer);
    }

    template <typename View>
    void dumpLandau(const View& Eview) {
        const int nghostE = this->fcontainer_m->getE().getNghost();

        using index_array_type = typename ippl::RangePolicy<Dim>::index_array_type;
        double localEx2 = 0, localExNorm = 0;
        ippl::parallel_reduce(
            "Ex stats", ippl::getRangePolicy(Eview, nghostE),
            KOKKOS_LAMBDA(const index_array_type& args, double& E2, double& ENorm) {
                // ippl::apply<unsigned> accesses the view at the given indices and obtains a
                // reference; see src/Expression/IpplOperations.h
                double val = ippl::apply(Eview, args)[0];
                double e2  = Kokkos::pow(val, 2);
                E2 += e2;

                double norm = Kokkos::fabs(ippl::apply(Eview, args)[0]);
                if (norm > ENorm) {
                    ENorm = norm;
                }
            },
            Kokkos::Sum<double>(localEx2), Kokkos::Max<double>(localExNorm));

        double globaltemp = 0.0;
        ippl::Comm->reduce(localEx2, globaltemp, 1, std::plus<double>());

        double fieldEnergy =
            std::reduce(this->fcontainer_m->getHr().begin(), this->fcontainer_m->getHr().end(),
                        globaltemp, std::multiplies<double>());

        double ExAmp = 0.0;
        ippl::Comm->reduce(localExNorm, ExAmp, 1, std::greater<double>());

        if (benchmark_m) {
            const double meanPhi = this->fcontainer_m->getPhi().getVolumeAverage();
            const double meanRho = this->fcontainer_m->getRho().getVolumeAverage();
            if (ippl::Comm->rank() == 0)
                benchmarkPhysics_m << this->it_m << ',' << this->time_m << ',' << fieldEnergy
                                   << ',' << ExAmp << ',' << meanPhi << ',' << meanRho << '\n';
        }

        if (ippl::Comm->rank() == 0) {
            std::filesystem::create_directory("data");
            std::stringstream fname;
            fname << "data/FieldLandau_";
            fname << ippl::Comm->size();
            fname << "_manager";
            fname << ".csv";
            Inform csvout(NULL, fname.str().c_str(), Inform::APPEND);
            csvout.precision(16);
            csvout.setf(std::ios::scientific, std::ios::floatfield);
            if (std::fabs(this->time_m) < 1e-14) {
                csvout << "time, Ex_field_energy, Ex_max_norm" << endl;
            }
            csvout << this->time_m << " " << fieldEnergy << " " << ExAmp << endl;
        }
        ippl::Comm->barrier();
    }

    // Overloaded dumpLandau which computes the E-field energy using the particles
    // instead of using the E-field on the grid (as above). Since we have E for
    // each particle, we treat the particles as Monte-Carlo samples to compute
    // the energy integral.
    void dumpLandau() {
        auto Eview               = this->pcontainer_m->E.getView();
        size_type localParticles = this->pcontainer_m->getLocalNum();

        using exec_space  = typename Kokkos::View<const size_t*>::execution_space;
        using policy_type = Kokkos::RangePolicy<exec_space>;
        policy_type iteration_policy(0, localParticles);

        double localEx2 = 0;
        Kokkos::parallel_reduce(
            "Ex stats", iteration_policy,
            KOKKOS_LAMBDA(const size_t i, double& E2) {
                double val = Eview(i)[0];
                double e2  = Kokkos::pow(val, 2);
                E2 += e2;
            },
            Kokkos::Sum<double>(localEx2));

        double globaltemp = 0.0;
        ippl::Comm->reduce(localEx2, globaltemp, 1, std::plus<double>());

        // MC integration: divide by no. of particles N and multiply by volume
        ippl::Vector<T, Dim> domain_size = this->rmax_m - this->rmin_m;
        double fieldEnergy = std::reduce(domain_size.begin(), domain_size.end(), globaltemp,
                                         std::multiplies<double>());

        fieldEnergy = fieldEnergy / this->totalP_m;

        if (ippl::Comm->rank() == 0) {
            std::filesystem::create_directory("data");
            std::stringstream fname;
            fname << "data/FieldLandau_";
            fname << ippl::Comm->size();
            fname << "_manager";
            fname << ".csv";
            Inform csvout(NULL, fname.str().c_str(), Inform::APPEND);
            csvout.precision(16);
            csvout.setf(std::ios::scientific, std::ios::floatfield);
            if (std::fabs(this->time_m) < 1e-14) {
                csvout << "time, Ex_field_energy" << endl;
            }
            csvout << this->time_m << " " << fieldEnergy << endl;
        }
        ippl::Comm->barrier();
    }
};
#endif
