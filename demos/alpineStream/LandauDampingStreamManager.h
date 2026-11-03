#ifndef IPPL_LANDAU_DAMPING_STREAM_MANAGER_H
#define IPPL_LANDAU_DAMPING_STREAM_MANAGER_H

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <type_traits>

#include "Manager/datatypes.h"
#include "Stream/OpenPMD/OpenPMDWriter.h"
#include "Utility/IpplTimings.h"
#include "../alpine/LandauDampingManager.h"

namespace alpine_stream {
    enum class OutputKind { Particles, Fields };

    template <class View>
    inline constexpr const char* VECTOR_LAYOUT_NAME =
        requires(View view) { view.scalarView(); } ? "SoA" : "AoS";

    struct OutputOptions {
        bool enabled = true;
        OutputKind kind = OutputKind::Particles;
        double lengthUnitSI = 1.0;
        ippl::OpenPMDOptions writer{.directory = "openpmd"};
    };

    // The existing ID allocator and migration paths keep IDs attached to particles.
    using LandauWithIds =
        LandauDampingManager<double, 3, Kokkos::DefaultExecutionSpace::memory_space>;

    class LandauDampingStreamManager : public LandauWithIds {
    public:
        static constexpr unsigned DIM = 3;
        static constexpr const char* PARTICLE_LAYOUT =
            VECTOR_LAYOUT_NAME<typename ParticleContainer_t::particle_position_type::view_type>;
        static constexpr const char* FIELD_LAYOUT =
            VECTOR_LAYOUT_NAME<typename VField_t<double, DIM>::view_type>;
        static constexpr std::uint64_t BYTES_PER_PARTICLE = 6 * sizeof(double) + sizeof(std::int64_t);
        static_assert(sizeof(double) == 8);
        static_assert(std::is_same_v<ParticleContainer_t::index_type, std::int64_t>);

        LandauDampingStreamManager(size_type particles, int steps, Vector_t<int, DIM>& grid,
                                   double loadBalanceThreshold, std::string& solver,
                                   std::string& stepMethod, std::vector<std::string> preconditioner,
                                   OutputOptions output = {})
            : LandauWithIds(particles, steps, grid, loadBalanceThreshold, solver, stepMethod,
                            std::move(preconditioner))
            , options_m(std::move(output)) {
            if (!std::isfinite(options_m.lengthUnitSI) || options_m.lengthUnitSI <= 0
                || !std::isfinite(options_m.writer.timeUnitSI) || options_m.writer.timeUnitSI <= 0
                || particles > std::numeric_limits<std::uint64_t>::max() / BYTES_PER_PARTICLE)
                throw IpplException("LandauDampingStream", "Invalid units or particle count");
        }

        void pre_run() override {
            LandauWithIds::pre_run();
            if (!options_m.enabled)
                return;

            Kokkos::fence("Landau output initialization");
            ippl::Comm->barrier();
            IpplTimings::startTimer(openTimer_m);
            if (options_m.writer.backend == ippl::OpenPMDBackend::HDF5
                && options_m.writer.backendConfig == "{}")
                options_m.writer.backendConfig = R"({"hdf5":{"independent_stores":false}})";
            writer_m = std::make_unique<ippl::OpenPMDWriter>(options_m.writer,
                                                            ippl::Comm->getCommunicator());
            if (options_m.kind == OutputKind::Particles) {
                auto& species = writer_m->addParticles("electrons", *this->pcontainer_m,
                                                   {.lengthUnitSI = options_m.lengthUnitSI});
            // Kick-drift-kick leaves both R and P at the completed step's time.
                species.bind(this->pcontainer_m->P,
                         {{options_m.lengthUnitSI / options_m.writer.timeUnitSI,
                           {1, 0, -1, 0, 0, 0, 0}}, {"x", "y", "z"}});
                writer_m->setParticleAttributeSelection(
                "electrons", ippl::ParticleAttributeSelection::minimal({"velocity"})
                                 .withParticleIds().withRankIds(false));
            } else {
                // Landau's E and rho are normalized quantities, not SI-valued records.
                const ippl::FieldMetadata electricMetadata{
                    {{1.0, {}}, {"x", "y", "z"}}, options_m.lengthUnitSI,
                    {"x", "y", "z"}, {0.5, 0.5, 0.5}};
                auto densityMetadata = electricMetadata;
                densityMetadata.record.components.clear();
                writer_m->addField("electric_field", this->fcontainer_m->getE(), electricMetadata);
                writer_m->addField("charge_density", this->fcontainer_m->getRho(), densityMetadata);
            }
            writer_m->open();
            IpplTimings::stopTimer(openTimer_m);
        }

        void pre_step() override {
            IpplTimings::startTimer(stepTimer_m);
            LandauWithIds::pre_step();
        }

        void advance() override {
            Kokkos::fence("Landau advance start");
            IpplTimings::startTimer(advanceTimer_m);
            LandauWithIds::advance();
            Kokkos::fence("Landau advance end");
            IpplTimings::stopTimer(advanceTimer_m);
        }

        void post_step() override {
            LandauWithIds::post_step();
            if (writer_m) {
                IpplTimings::startTimer(syncTimer_m);
                Kokkos::fence("Landau output start");
                ippl::Comm->barrier();
                IpplTimings::stopTimer(syncTimer_m);
                IpplTimings::startTimer(writeTimer_m);
                // write() includes packing, host staging, iteration close and backend flush.
                writer_m->write({static_cast<std::uint64_t>(this->it_m), this->time_m, this->dt_m});
                IpplTimings::stopTimer(writeTimer_m);
            }
            Kokkos::fence("Landau timestep end");
            IpplTimings::stopTimer(stepTimer_m);
        }

        void closeOutput() {
            if (!writer_m)
                return;
            IpplTimings::startTimer(closeTimer_m);
            writer_m->close();
            writer_m.reset();
            IpplTimings::stopTimer(closeTimer_m);
        }

        double getDt() const { return this->dt_m; }

        std::uint64_t outputBytes() const {
            if (!options_m.enabled)
                return 0;
            if (options_m.kind == OutputKind::Particles)
                return this->totalP_m * BYTES_PER_PARTICLE;
            std::uint64_t cells = 1;
            for (unsigned d = 0; d < DIM; ++d) {
                const auto extent = this->fcontainer_m->getE().getDomain()[d].length();
                if (cells > std::numeric_limits<std::uint64_t>::max() / extent / (4 * sizeof(double)))
                    throw IpplException("LandauDampingStream", "Field payload overflows uint64_t");
                cells *= extent;
            }
            return cells * 4 * sizeof(double);
        }

        // Collect after the run so reductions and CSV writes are outside timestep timers.
        void writeSummary(const std::string& path = "io_summary.csv") const {
            const int rank = ippl::Comm->rank();
            const int ranks = ippl::Comm->size();
            std::ofstream output;
            int failed = 0;
            if (rank == 0) {
                output.open(path);
                failed = !output;
            }
            MPI_Bcast(&failed, 1, MPI_INT, 0, ippl::Comm->getCommunicator());
            if (failed)
                throw IpplException("LandauDampingStream", "Cannot open timing summary " + path);
            if (rank == 0) {
                output << "step,time,dt,ranks,nx,ny,nz,particles,io_enabled,io_backend,payload_bytes,"
                          "advance_max_s,io_sync_max_s,io_write_min_s,io_write_mean_s,"
                          "io_write_max_s,step_max_s,payload_GiB_per_s,"
                          "particle_layout,field_layout,output_kind\n";
                output << std::setprecision(17);
            }
            const auto& advance = IpplTimings::getMeasurements(advanceTimer_m);
            const auto& sync = IpplTimings::getMeasurements(syncTimer_m);
            const auto& write = IpplTimings::getMeasurements(writeTimer_m);
            const auto& step = IpplTimings::getMeasurements(stepTimer_m);
            const auto bytes = outputBytes();
            double time = 0;
            for (std::size_t i = 0; i < step.size(); ++i) {
                time += this->dt_m;
                std::array<double, 4> local{advance.at(i), options_m.enabled ? sync.at(i) : 0,
                                            options_m.enabled ? write.at(i) : 0, step.at(i)};
                std::array<double, 4> minimum{}, maximum{}, sum{};
                const auto comm = ippl::Comm->getCommunicator();
                MPI_Reduce(local.data(), minimum.data(), 4, MPI_DOUBLE, MPI_MIN, 0, comm);
                MPI_Reduce(local.data(), maximum.data(), 4, MPI_DOUBLE, MPI_MAX, 0, comm);
                MPI_Reduce(local.data(), sum.data(), 4, MPI_DOUBLE, MPI_SUM, 0, comm);
                if (rank == 0) {
                    const double throughput = maximum[2] > 0 ? bytes / 1073741824.0 / maximum[2] : 0;
                    output << i + 1 << ',' << time << ',' << this->dt_m << ','
                           << ranks << ',' << this->nr_m[0] << ',' << this->nr_m[1] << ','
                           << this->nr_m[2] << ',' << this->totalP_m << ',' << options_m.enabled
                           << ',' << (options_m.writer.backend == ippl::OpenPMDBackend::BP5 ? "bp5" : "hdf5")
                           << ',' << bytes << ',' << maximum[0] << ',' << maximum[1] << ','
                           << minimum[2] << ',' << sum[2] / ranks << ',' << maximum[2] << ','
                           << maximum[3] << ',' << throughput << ',' << PARTICLE_LAYOUT << ','
                           << FIELD_LAYOUT << ','
                           << (options_m.kind == OutputKind::Particles ? "particles" : "fields")
                           << '\n';
                }
            }
            if (rank == 0) {
                output.close();
                failed = !output;
            }
            MPI_Bcast(&failed, 1, MPI_INT, 0, ippl::Comm->getCommunicator());
            if (failed)
                throw IpplException("LandauDampingStream", "Cannot write timing summary " + path);
        }

    private:
        OutputOptions options_m;
        std::unique_ptr<ippl::OpenPMDWriter> writer_m;
        IpplTimings::TimerRef openTimer_m = IpplTimings::getTimer("ioOpen");
        IpplTimings::TimerRef syncTimer_m = IpplTimings::getTimer("ioSync");
        IpplTimings::TimerRef writeTimer_m = IpplTimings::getTimer("ioWrite");
        IpplTimings::TimerRef closeTimer_m = IpplTimings::getTimer("ioClose");
        IpplTimings::TimerRef advanceTimer_m = IpplTimings::getTimer("advance");
        IpplTimings::TimerRef stepTimer_m = IpplTimings::getTimer("timestep");
    };
}  // namespace alpine_stream

#endif
