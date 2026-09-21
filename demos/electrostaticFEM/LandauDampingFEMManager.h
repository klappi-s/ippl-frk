#pragma once
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

#include "FEM/FEMContainerInterpolate.h"
#include "FEMBuildInfo.h"
#include "FEMFields.h"
#include "FEMLoadBalancer.h"
#include "FEMParticles.h"
#include "Manager/BaseManager.h"
#include "Random/Distribution.h"
#include "Random/InverseTransformSampling.h"
#include "Random/Randn.h"

namespace electrostatic_fem {
    struct LandauDistribution {
        struct CDF {
            KOKKOS_INLINE_FUNCTION double operator()(double x, unsigned d, const double* p) const {
                return x + p[2 * d] / p[2 * d + 1] * Kokkos::sin(p[2 * d + 1] * x);
            }
        };
        struct PDF {
            KOKKOS_INLINE_FUNCTION double operator()(double x, unsigned d, const double* p) const {
                return 1 + p[2 * d] * Kokkos::cos(p[2 * d + 1] * x);
            }
        };
        struct Estimate {
            KOKKOS_INLINE_FUNCTION double operator()(double u, unsigned, const double*) const {
                return u;
            }
        };
    };

    template <unsigned Dim, unsigned Order, unsigned QuadPoints>
    class LandauDampingFEMManager : public ippl::BaseManager {
        using Fields = FEMFields<Dim, Order, QuadPoints>;
        using Field  = typename Fields::Field;
        using Point  = typename Fields::Point;
        using Policy = Kokkos::RangePolicy<Kokkos::DefaultExecutionSpace>;
        ExperimentConfig config_m;
        std::unique_ptr<Fields> fields_m;
        std::unique_ptr<FEMParticles<Dim>> particles_m;
        FEMLoadBalancer<Dim> balancer_m;
        unsigned step_m = 0;
        std::string method_m;
        std::map<std::string, double> times_m;
        std::ostringstream solves_m, steps_m, physics_m;
        std::vector<Field> loads_m;
        Kokkos::View<Point*> quadraturePositions_m, quadratureField_m;
        Kokkos::View<double*> quadratureWeights_m;
        size_t totalParticles_m = 0;
        double initialEnergy_m  = 0;
        bool ownsOutput_m       = false;

        template <typename F>
        double timed(const std::string& name, F&& operation) {
            const auto timer = IpplTimings::getTimer(("electrostaticFEM." + name).c_str());
            Kokkos::fence();
            IpplTimings::startTimer(timer);
            try {
                operation();
            } catch (...) {
                Kokkos::fence();
                IpplTimings::stopTimer(timer);
                throw;
            }
            Kokkos::fence();
            IpplTimings::stopTimer(timer);
            const double seconds = IpplTimings::getMeasurements(timer).back();
            times_m[name] += seconds;
            return seconds;
        }
        std::string path(const std::string& name) const { return config_m.output + "/" + name; }
        void selectMethod(const std::string& method) {
            // Reject typos before they can fall through to an identity preconditioner.
            if (method != "plain")
                ippl::preconditioner_validation::throwIfUnknownType(method, "LandauDampingFEM");
            if (method == "multigrid")
                throw std::runtime_error("Multigrid is outside this experiment.");
            const auto parameters = config_m.parameters(method);
            if ((method == "richardson" || method == "richardson_alt")
                && parameters.template get<int>("richardson_iterations") % 2 == 0)
                throw std::runtime_error(
                    "Even undamped Richardson sweeps can be indefinite for periodic higher-order "
                    "FEM; select an odd richardson_iterations value.");
            method_m = method;
            fields_m->solver.mergeParameters(parameters);
            fields_m->solver.invalidatePreparedData();
            fields_m->potential = 0;
        }
        void initializeParticles() {
            auto& f     = *fields_m;
            particles_m = std::make_unique<FEMParticles<Dim>>(f.mesh, f.layout);
            auto& p     = *particles_m;
            double parameters[2 * Dim];
            Point lower(0), upper, localLower, localUpper;
            const auto owned = f.solver.getSpace().getDOFHandler().getLocalElementDomain();
            for (unsigned d = 0; d < Dim; ++d) {
                upper[d]      = config_m.lengths[d];
                localLower[d] = owned[d].first() * f.mesh.getMeshSpacing()[d];
                localUpper[d] = (owned[d].last() + 1) * f.mesh.getMeshSpacing()[d];
                parameters[2 * d] =
                    (d == 0 || config_m.initialCondition == "alpine_product") ? config_m.alpha : 0;
                parameters[2 * d + 1] =
                    2 * Kokkos::numbers::pi_v<double> * config_m.mode / config_m.lengths[d];
            }
            using Distribution =
                ippl::random::Distribution<double, Dim, 2 * Dim, LandauDistribution>;
            Distribution distribution(parameters);
            ippl::detail::size_type requested = config_m.particles;
            ippl::random::InverseTransformSampling<double, Dim, Kokkos::DefaultExecutionSpace,
                                                   Distribution>
                sample(distribution, upper, lower, localUpper, localLower, requested);
            p.create(sample.getLocalSamplesNum());
            Kokkos::Random_XorShift64_Pool<> pool(config_m.seed + 100 * ippl::Comm->rank());
            sample.generate(p.R.getView(), pool);
            double mean[Dim], sigma[Dim];
            for (unsigned d = 0; d < Dim; ++d) {
                mean[d]  = 0;
                sigma[d] = config_m.thermalSpeed;
            }
            Kokkos::parallel_for(p.getLocalNum(), ippl::random::randn<double, Dim>(
                                                      p.velocity.getView(), pool, mean, sigma));
            size_t local = p.getLocalNum();
            ippl::Comm->allreduce(local, totalParticles_m, 1, std::plus<size_t>());
            if (totalParticles_m != config_m.particles)
                throw std::runtime_error("Sampling changed the requested particle count.");
            p.charge   = -f.volume / totalParticles_m;
            p.electric = Point(0);
            p.update();
        }
        void initializeQuadrature() {
            auto& f = *fields_m;
            typename Fields::Solver::ElementType element;
            typename Fields::Solver::QuadratureType quadrature(element);
            const auto points   = quadrature.getIntegrationNodesForRefElement();
            const auto weights  = quadrature.getWeightsForRefElement();
            const auto handler  = f.solver.getSpace().getDOFHandler();
            const auto elements = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(),
                                                                      handler.getElementIndices());
            const size_t count  = elements.extent(0) * points.dim;
            quadraturePositions_m = Kokkos::View<Point*>("FEM diagnostic points", count);
            quadratureField_m     = Kokkos::View<Point*>("FEM diagnostic gradients", count);
            quadratureWeights_m   = Kokkos::View<double*>("FEM diagnostic weights", count);
            auto positions        = Kokkos::create_mirror_view(quadraturePositions_m);
            auto physicalWeights  = Kokkos::create_mirror_view(quadratureWeights_m);
            const auto h          = f.mesh.getMeshSpacing();
            double det            = 1;
            for (unsigned d = 0; d < Dim; ++d)
                det *= h[d];
            for (size_t e = 0; e < elements.extent(0); ++e) {
                auto cell = handler.getElementNDIndex(elements(e));
                for (unsigned q = 0; q < points.dim; ++q) {
                    for (unsigned d = 0; d < Dim; ++d)
                        positions(e * points.dim + q)[d] = (cell[d] + points[q][d]) * h[d];
                    physicalWeights(e * points.dim + q) = det * weights[q];
                }
            }
            Kokkos::deep_copy(quadraturePositions_m, positions);
            Kokkos::deep_copy(quadratureWeights_m, physicalWeights);
        }
        void deposit() {
            auto& f = *fields_m;
            auto& p = *particles_m;
            timed("deposition", [&] {
                const auto stats = ippl::depositFEMLoad(f.load, f.solver.getSpace(), p.R.getView(),
                                                        p.charge.getView(), p.getLocalNum());
                times_m["deposition_local"] += stats.localSeconds;
                times_m["deposition_halo"] += stats.haloSeconds;
                times_m["deposition_ownership"] += stats.ownershipSeconds;
            });
            timed("background", [&] {
                f.load += f.weights;
            });  // rho_ion=1 in normalized units
        }
        void solve(const std::string& phase, unsigned id) {
            auto& f = *fields_m;
            if (config_m.initialGuess == "zero")
                f.potential = 0;
            const double seconds = timed("solve", [&] {
                f.solver.solveLoadVector(f.load);
            });
            const auto stats     = f.solver.getSolveStatistics();
            double relative = 0, rhsNorm = 0;
            const double verify = timed("residual_check", [&] {
                rhsNorm       = ippl::norm(f.load);
                auto residual = f.load - f.solver.applyOperator(f.potential);
                relative      = ippl::norm(residual) / (rhsNorm > 0 ? rhsNorm : 1);
            });
            const bool success  = std::isfinite(relative)
                                  && relative <= std::max(20 * config_m.tolerance, 5e-11)
                                  && f.solver.getIterationCount() < config_m.maxIterations;
            solves_m << phase << ',' << method_m << ',' << id << ',' << ippl::Comm->rank() << ','
                     << f.solver.getIterationCount() << ',' << f.solver.getResidue() << ','
                     << relative << ',' << rhsNorm << ',' << stats.operatorSetupSeconds << ','
                     << stats.preconditionerSetupSeconds << ','
                     << stats.preconditionerApplicationSeconds << ',' << stats.krylovSeconds << ','
                     << seconds << ',' << verify << ',' << stats.operatorSetups << ','
                     << stats.preconditionerSetups << ',' << stats.operatorCalls << ','
                     << stats.preconditionerOperatorCalls << ',' << stats.preconditionerApplications
                     << ',' << stats.spectralLower << ',' << stats.spectralUpper << ',' << success
                     << ',' << stats.triangularCalls << ',' << stats.diagonalCalls << ','
                     << stats.operatorSeconds << ',' << stats.triangularSeconds << ','
                     << stats.diagonalSeconds << ',' << stats.diagonalSetupSeconds << ','
                     << stats.spectralSetupSeconds << '\n';
            if (!success)
                throw std::runtime_error("Fresh residual check failed for " + method_m
                                         + " at solve " + std::to_string(id));
        }
        void gather() {
            auto& f = *fields_m;
            auto& p = *particles_m;
            timed("gather", [&] {
                const auto stats =
                    ippl::gatherFEMElectricField(f.potential, f.solver.getSpace(), p.R.getView(),
                                                 p.electric.getView(), p.getLocalNum());
                times_m["gather_local"] += stats.localSeconds;
                times_m["potential_halo"] += stats.haloSeconds;
                times_m["gather_ownership"] += stats.ownershipSeconds;
            });
        }
        void diagnostics() {
            auto& f = *fields_m;
            auto& p = *particles_m;
            timed("diagnostics", [&] {
                ippl::gatherFEMElectricField(f.potential, f.solver.getSpace(),
                                             quadraturePositions_m, quadratureField_m,
                                             quadraturePositions_m.extent(0));
                const auto field = quadratureField_m, positions = quadraturePositions_m;
                const auto weights = quadratureWeights_m;
                double local = 0, energy = 0, real = 0, imaginary = 0, kinetic = 0;
                Kokkos::parallel_reduce(
                    "FEM field energy", Policy(0, weights.extent(0)),
                    KOKKOS_LAMBDA(size_t i, double& sum) {
                        for (unsigned d = 0; d < Dim; ++d)
                            sum += 0.5 * weights(i) * field(i)[d] * field(i)[d];
                    },
                    local);
                ippl::Comm->allreduce(local, energy, 1, std::plus<double>());
                const double wave =
                    2 * Kokkos::numbers::pi_v<double> * config_m.mode / config_m.lengths[0];
                Kokkos::parallel_reduce(
                    "FEM electric Fourier real", Policy(0, weights.extent(0)),
                    KOKKOS_LAMBDA(size_t i, double& sum) {
                        sum += weights(i) * field(i)[0] * Kokkos::cos(wave * positions(i)[0]);
                    },
                    local);
                ippl::Comm->allreduce(local, real, 1, std::plus<double>());
                Kokkos::parallel_reduce(
                    "FEM electric Fourier imaginary", Policy(0, weights.extent(0)),
                    KOKKOS_LAMBDA(size_t i, double& sum) {
                        sum += weights(i) * field(i)[0] * Kokkos::sin(wave * positions(i)[0]);
                    },
                    local);
                ippl::Comm->allreduce(local, imaginary, 1, std::plus<double>());
                const auto velocity = p.velocity.getView();
                const auto charge   = p.charge.getView();
                Kokkos::parallel_reduce(
                    "FEM kinetic energy", Policy(0, p.getLocalNum()),
                    KOKKOS_LAMBDA(size_t i, double& sum) {
                        for (unsigned d = 0; d < Dim; ++d)
                            sum -= 0.5 * charge(i) * velocity(i)[d] * velocity(i)[d];
                    },
                    local);
                ippl::Comm->allreduce(local, kinetic, 1, std::plus<double>());
                Point momentum(0);
                for (unsigned d = 0; d < Dim; ++d) {
                    Kokkos::parallel_reduce(
                        "FEM particle momentum", Policy(0, p.getLocalNum()),
                        KOKKOS_LAMBDA(size_t i, double& sum) { sum -= charge(i) * velocity(i)[d]; },
                        local);
                    ippl::Comm->allreduce(local, momentum[d], 1, std::plus<double>());
                }
                if (!step_m)
                    initialEnergy_m = energy + kinetic;
                const double netCharge  = innerProduct(f.one, f.load);
                const double weakEnergy = 0.5 * innerProduct(f.potential, f.load);
                if (!std::isfinite(energy + kinetic) || std::abs(netCharge) > 1e-11 * f.volume)
                    throw std::runtime_error("Nonfinite energy or lost charge.");
                if (std::abs(energy - weakEnergy)
                    > std::max(100 * config_m.tolerance, 1e-10) * std::max(energy, 1e-12))
                    throw std::runtime_error("Quadrature and weak field energies disagree.");
                const double mean = f.solver.getAvg(true);
                if (ippl::Comm->rank() == 0) {
                    physics_m << step_m << ',' << step_m * config_m.dt << ',' << netCharge << ','
                              << mean << ','
                              << 2 * std::sqrt(real * real + imaginary * imaginary) / f.volume
                              << ',' << energy << ',' << weakEnergy << ',' << kinetic << ','
                              << (energy + kinetic - initialEnergy_m) / initialEnergy_m;
                    for (unsigned d = 0; d < Dim; ++d)
                        physics_m << ',' << momentum[d];
                    physics_m << '\n';
                }
            });
        }
        void writeManifest() {
            if (ippl::Comm->rank())
                return;
            std::ofstream out(path("configuration.txt"));
            out << BuildConfiguration << std::setprecision(17)
                << "source=" << IPPL_FEM_SOURCE_REVISION << '\n'
                << "backend=" << Kokkos::DefaultExecutionSpace::name()
                << "\nranks=" << ippl::Comm->size()
                << "\nthreads=" << Kokkos::DefaultExecutionSpace().concurrency()
                << "\ndimension=" << Dim << "\norder=" << Order
                << "\nquadrature_points=" << QuadPoints
                << "\ninterpolation_nodes=" << config_m.family
                << "\nstiffness=" << config_m.stiffness << "\nreuse=" << config_m.reuse
                << "\ninitial_guess=" << config_m.initialGuess
                << "\ninitial_condition=" << config_m.initialCondition << "\nseed=" << config_m.seed
                << "\nparticles=" << totalParticles_m << "\nsteps=" << config_m.steps
                << "\ndt=" << config_m.dt << "\nalpha=" << config_m.alpha
                << "\nmode=" << config_m.mode << "\nthermal_speed=" << config_m.thermalSpeed
                << "\ntolerance=" << config_m.tolerance
                << "\nmax_iterations=" << config_m.maxIterations << "\npic_method=" << method_m
                << "\nsolver=" << config_m.solver << "\npreconditioner=" << config_m.preconditioner
                << "\ntimer_level=" << config_m.timerLevel << "\nreplay=" << config_m.replay
                << "\nload_balancing=disabled\nlayout_generation=0\n";
            size_t dofs = 1;
            for (unsigned d = 0; d < Dim; ++d) {
                dofs *= Order * config_m.cells[d];
                out << "cells_" << d << '=' << config_m.cells[d] << "\nlength_" << d << '='
                    << config_m.lengths[d] << '\n';
            }
            out << "dofs=" << dofs << "\ndiagnostics_every=" << config_m.diagnosticsEvery << '\n';
            for (const auto& [name, value] : config_m.options)
                out << "option." << name << '=' << value << '\n';
            for (const auto& [name, fallback] :
                 std::map<std::string, std::string>{{"newton_level", "2"},
                                                    {"chebyshev_degree", "8"},
                                                    {"richardson_iterations", "4"},
                                                    {"gauss_seidel_inner_iterations", "2"},
                                                    {"gauss_seidel_outer_iterations", "2"},
                                                    {"ssor_omega", "1"}})
                out << name << '=' << config_m.get(name, fallback) << '\n';
        }

    public:
        explicit LandauDampingFEMManager(ExperimentConfig c)
            : config_m(std::move(c)) {
            solves_m
                << std::setprecision(17)
                << "phase,method,solve,rank,iterations,recursive_residual,fresh_relative_residual,"
                   "rhs_norm,operator_setup_seconds,preconditioner_setup_seconds,preconditioner_"
                   "apply_seconds,krylov_seconds,solve_seconds,verification_seconds,operator_"
                   "setups,preconditioner_setups,operator_calls,preconditioner_operator_calls,"
                   "preconditioner_applications,spectral_lower,spectral_upper,success,triangular_"
                   "calls,diagonal_calls,operator_seconds,triangular_seconds,diagonal_seconds,"
                   "diagonal_setup_seconds,spectral_setup_seconds\n";
            steps_m << std::setprecision(17)
                    << "step,rank,stage,seconds,local_particles,imbalance\n";
            physics_m << std::setprecision(17)
                      << "step,time,net_charge,mean_potential,Ex_mode_amplitude,field_energy,weak_"
                         "field_energy,kinetic_energy,relative_energy_drift";
            for (unsigned d = 0; d < Dim; ++d)
                physics_m << ",momentum_" << d;
            physics_m << '\n';
        }
        void pre_run() override {
            int outputError = 0;
            if (ippl::Comm->rank() == 0) {
                try {
                    if (std::filesystem::exists(path("configuration.txt")))
                        outputError = 1;
                    else
                        std::filesystem::create_directories(config_m.output);
                } catch (...) {
                    outputError = 1;
                }
            }
            MPI_Bcast(&outputError, 1, MPI_INT, 0, MPI_COMM_WORLD);
            if (outputError)
                throw std::runtime_error("Output exists or cannot be created: " + config_m.output);
            ownsOutput_m = true;
            ippl::Comm->barrier();
            timed("initialization", [&] {
                fields_m = std::make_unique<Fields>(config_m);
                initializeParticles();
                initializeQuadrature();
            });
            selectMethod(config_m.replay.empty() && config_m.solver != "plain"
                             ? config_m.preconditioner
                             : "plain");
            writeManifest();
            deposit();
            if (!config_m.replay.empty())
                loads_m.push_back(fields_m->load.deepCopy());
            solve("initial", 0);
            gather();
            diagnostics();
            writeStep();
        }
        void advance() override {
            ++step_m;
            times_m.clear();
            timed("step_total", [&] {
                auto& p = *particles_m;
                timed("kick", [&] {
                    p.velocity = p.velocity - 0.5 * config_m.dt * p.electric;
                });
                timed("drift", [&] {
                    p.R = p.R + config_m.dt * p.velocity;
                });
                timed("particle_update", [&] {
                    p.update();
                });
                // balancer_m.repartition(); // Disabled until FEM redistribution is validated.
                deposit();
                if (!config_m.replay.empty())
                    timed("capture_load", [&] {
                        loads_m.push_back(fields_m->load.deepCopy());
                    });
                solve("step", step_m);
                gather();
                timed("kick", [&] {
                    p.velocity = p.velocity - 0.5 * config_m.dt * p.electric;
                });
                if (step_m % config_m.diagnosticsEvery == 0 || step_m == config_m.steps)
                    diagnostics();
            });
            writeStep();
        }
        void writeStep() {
            double imbalance = 0;
            timed("imbalance", [&] {
                imbalance = balancer_m.imbalance(particles_m->getLocalNum(), totalParticles_m);
            });
            for (const auto& [stage, seconds] : times_m)
                steps_m << step_m << ',' << ippl::Comm->rank() << ',' << stage << ',' << seconds
                        << ',' << particles_m->getLocalNum() << ',' << imbalance << '\n';
        }
        void flush() {
            if (!ownsOutput_m)
                return;
            const auto rank = std::to_string(ippl::Comm->rank());
            std::ofstream(path("solves-rank" + rank + ".csv")) << solves_m.str();
            std::ofstream(path("steps-rank" + rank + ".csv")) << steps_m.str();
            if (ippl::Comm->rank() == 0)
                std::ofstream(path("physics.csv")) << physics_m.str();
        }
        void finish() {
            if (!config_m.replay.empty()) {
                std::string list = config_m.replay;
                for (auto& ch : list)
                    if (ch == ',')
                        ch = ' ';
                std::istringstream methods(list);
                std::string method;
                while (methods >> method) {
                    selectMethod(method);
                    for (size_t i = 0; i < loads_m.size(); ++i) {
                        fields_m->load = loads_m[i];
                        solve("replay", i);
                    }
                }
            }
            timed("output", [&] {
                flush();
            });
            IpplTimings::dumpToCSV(path("timers.csv"));
            if (ippl::Comm->rank() == 0)
                std::cout << "LandauDampingFEM completed " << config_m.steps << " steps ("
                          << config_m.steps + 1 << " PIC solves). Results: " << config_m.output
                          << '\n';
        }
    };
}  // namespace electrostatic_fem
