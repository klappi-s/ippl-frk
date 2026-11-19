// Restart files for the FEL manager at the end of a complete FDTD/Boris step.
#ifndef IPPL_FEL_CHECKPOINT_H
#define IPPL_FEL_CHECKPOINT_H

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <openPMD/openPMD.hpp>

#include "Stream/IO/IOReader.h"
#include "Stream/IO/IOWriter.h"
#include "Stream/IO/detail/Common.h"
#include "units.h"

struct FELCheckpoint {
    using Path    = std::filesystem::path;
    using Counts  = std::vector<std::uint64_t>;
    using Session = ippl::detail::io::Session;

#ifdef IPPL_FEL_CHECKPOINT_BP5
    static constexpr auto BACKEND    = ippl::OpenPMDBackend::BP5;
    static constexpr auto STATE_FILE = "fields.bp5";
#else
    static constexpr auto BACKEND    = ippl::OpenPMDBackend::HDF5;
    static constexpr auto STATE_FILE = "fields.h5";
#endif

    static void check(bool valid, const std::string& message) {
        if (!valid)
            throw std::runtime_error("FEL checkpoint: " + message);
    }

    class InvalidRestart : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // All ranks leave the launch normally for a rejected checkpoint, so that a
    // launcher can deliver the explanation before tearing down MPI.
    static void requireRestart(bool valid, const std::string& message) {
        int accepted = valid;
        MPI_Allreduce(MPI_IN_PLACE, &accepted, 1, MPI_INT, MPI_MIN, ippl::Comm->getCommunicator());
        if (!accepted)
            throw InvalidRestart(message);
    }

    // The shared IPPL I/O layer handles typed particle records, SI metadata and MPI offsets.
    template <class Particles, class Bind>
    static void attributes(Particles& p, Bind bind) {
        const std::vector<std::string> xyz{"x", "y", "z"};
        const ippl::Units length{unit_length_in_meters, {1, 0, 0, 0, 0, 0, 0}};
        bind(p.R_nm1, ippl::RecordMetadata{length, xyz});
        bind(p.R_np1, ippl::RecordMetadata{length, xyz});
        bind(p.gamma_beta, ippl::RecordMetadata{{1., {}}, xyz});
        bind(p.Q, ippl::RecordMetadata{{unit_charges_in_coulomb, {0, 0, 1, 1, 0, 0, 0}}, {}});
        bind(p.mass, ippl::RecordMetadata{{unit_mass_in_kg, {0, 1, 0, 0, 0, 0, 0}}, {}});
        bind(p.E_gather,
             ippl::RecordMetadata{
                 {unit_electric_fieldstrength_in_voltpermeters, {1, 1, -3, -1, 0, 0, 0}}, xyz});
        bind(p.B_gather, ippl::RecordMetadata{
                             {unit_magnetic_fluxdensity_in_tesla, {0, 1, -2, -1, 0, 0, 0}}, xyz});
    }

    template <class Manager>
    static void writeParticles(Manager& m, const Path& path, const Session& session) {
        ippl::IOOptions options;
        options.directory       = path;
        options.openPMD.backend = BACKEND;
        options.timeUnitSI      = unit_time_in_seconds;
        ippl::IOWriter writer(options, session.comm_m);
        auto& p         = *m.pcontainer_m;
        auto& electrons = writer.addParticles("electrons", p, {unit_length_in_meters});
        attributes(p, [&](auto& attr, auto metadata) {
            electrons.bind(attr, metadata);
        });
        writer.setParticleAttributeSelection(
            "electrons", ippl::ParticleAttributeSelection::all().withRankIds(false));
        writer.open();
        writer.write({static_cast<std::uint64_t>(m.it_m), m.time_m, m.dt_m});
        writer.close();
    }

    template <class Manager>
    static void readParticles(Manager& m, const Path& path, const Counts& counts,
                              const Session& session, bool sameLayout) {
        ippl::IOReader reader(path, session.comm_m);
        reader.open();
        const auto info  = reader.inspect(m.it_m);
        const auto total = std::accumulate(counts.begin(), counts.end(), std::uint64_t{0});
        check(info.particles.size() == 1 && info.particles.front().name == "electrons"
                  && info.particles.front().count == total && info.step.time == m.time_m
                  && info.step.dt == m.dt_m,
              "particle and solver snapshots disagree");
        // Preserve order when the decomposition matches. Otherwise distribute the file
        // read evenly, then use the particle layout to migrate all attributes by position.
        const std::uint64_t rank = session.rank_m;
        const auto perRank       = total / session.size_m;
        const auto remainder     = total % session.size_m;
        const auto offset =
            sameLayout ? std::accumulate(counts.begin(), counts.begin() + rank, std::uint64_t{0})
                       : rank * perRank + std::min(rank, remainder);
        const auto count = sameLayout ? counts[rank] : perRank + (rank < remainder);
        auto& p          = *m.pcontainer_m;
        p.create(count);
        auto read = [&](auto& attr, const std::string& name, std::vector<std::string> components) {
            auto view = attr.getView();
            auto host = Kokkos::create_mirror(view);
            if (components.empty())
                components.emplace_back();
            for (std::size_t c = 0; c < components.size(); ++c) {
                auto values = reader.readParticleComponent<double>(m.it_m, "electrons", name,
                                                                   components[c], offset, count);
                for (std::size_t i = 0; i < count; ++i) {
                    if constexpr (std::is_arithmetic_v<typename decltype(view)::value_type>)
                        host(i) = values[i];
                    else
                        host(i)[c] = values[i];
                }
            }
            Kokkos::deep_copy(view, host);
        };
        read(p.R, "position", {"x", "y", "z"});
        attributes(p, [&](auto& attr, auto metadata) {
            read(attr, attr.get_name(), metadata.components);
        });
        reader.close();
        if (!sameLayout)
            p.update();
    }

    // The caller keeps buffers alive until the collective flush, including on empty ranks.
    template <bool Writing>
    static void queueChunk(openPMD::MeshRecordComponent component, const Counts& shape,
                           const Counts& offset, const Counts& local, double* values) {
        if constexpr (Writing) {
            component.resetDataset({openPMD::Datatype::DOUBLE, shape});
            component.setPosition(std::vector<double>(shape.size(), 0.0));
            component.storeChunkRaw(values, offset, local);
        } else {
            check(component.getDatatype() == openPMD::Datatype::DOUBLE
                      && component.getExtent() == shape,
                  "incompatible mesh record");
            component.loadChunkRaw(values, offset, local);
        }
    }

    static void axes(openPMD::Mesh mesh, std::vector<std::string> labels) {
        mesh.setGridSpacing(std::vector<double>(labels.size(), 1.0));
        mesh.setGridGlobalOffset(std::vector<double>(labels.size(), 0.0));
        mesh.setAxisLabels(std::move(labels));
    }

    struct SlabChunk {
        std::uint64_t filePlane, localPlane, planes;
    };

    // A file slab holds [lower ghosts, owned z planes, upper ghosts]. Join the
    // intersections of old and new owned ranges, adding ghosts only at the two
    // ends of the NEW slab. A coincident cut keeps its saved ghost values exactly.
    static std::vector<SlabChunk> slabChunks(const std::vector<int>& starts, int nz, int first,
                                             int end, int ghosts) {
        std::vector<SlabChunk> chunks;
        auto it = std::upper_bound(starts.begin(), starts.end(), first) - 1;
        for (; it != starts.end() && *it < end; ++it) {
            const int stop   = std::next(it) == starts.end() ? nz : *std::next(it);
            const int lo     = std::max(first, *it);
            const int hi     = std::min(end, stop);
            const int begin  = lo == first ? lo - ghosts : lo;
            const int finish = hi == end ? hi + ghosts : hi;
            const auto slab  = static_cast<std::uint64_t>(it - starts.begin());
            chunks.push_back({slab * 2 * ghosts + static_cast<std::uint64_t>(begin + ghosts),
                              static_cast<std::uint64_t>(begin - first + ghosts),
                              static_cast<std::uint64_t>(finish - begin)});
        }
        return chunks;
    }

    template <bool Writing, class Field>
    static void field(openPMD::Series& series, openPMD::Mesh mesh, Field& field,
                      const std::vector<int>& sortedStarts) {
        const auto view = field.getView();
        const Counts local{view.extent(2), view.extent(1), view.extent(0)};
        const int nz     = field.getDomain()[2].length();
        const int ghosts = field.getNghost();
        const Counts shape{nz + 2 * std::uint64_t(ghosts) * sortedStarts.size(), local[1],
                           local[2]};
        const auto owned = field.getOwned()[2];
        auto chunks      = slabChunks(sortedStarts, nz, owned.first(), owned.last() + 1, ghosts);
        if constexpr (!Writing) {
            // Collective HDF5 transfer requires matching calls even when ranks
            // intersect different numbers of writer slabs. Padded chunks have zero planes.
            int rounds = static_cast<int>(chunks.size());
            MPI_Allreduce(MPI_IN_PLACE, &rounds, 1, MPI_INT, MPI_MAX,
                          ippl::Comm->getCommunicator());
            chunks.resize(rounds);
        }
        auto host = Kokkos::create_mirror_view(Kokkos::HostSpace(), view);
        if constexpr (Writing)
            Kokkos::deep_copy(host, view);
        std::vector<double> values(local[0] * local[1] * local[2]);
        if constexpr (Writing)
            axes(mesh, {"slab_z", "y", "x"});
        for (unsigned c = 0; c < Field::value_type::dim; ++c) {
            auto component      = mesh[std::to_string(c)];
            auto transferPlanes = [&] {
                for (const auto& chunk : chunks) {
                    queueChunk<Writing>(component, shape, {chunk.filePlane, 0, 0},
                                        {chunk.planes, local[1], local[2]},
                                        values.data() + chunk.localPlane * local[1] * local[2]);
                }
                series.flush();
            };
            if constexpr (!Writing)
                transferPlanes();
            std::size_t n = 0;
            for (std::size_t k = 0; k < local[0]; ++k)
                for (std::size_t j = 0; j < local[1]; ++j)
                    for (std::size_t i = 0; i < local[2]; ++i) {
                        if constexpr (Writing)
                            values[n++] = host(i, j, k)[c];
                        else
                            host(i, j, k)[c] = values[n++];
                    }
            if constexpr (Writing)
                transferPlanes();
        }
        if constexpr (!Writing)
            Kokkos::deep_copy(view, host);
    }

    template <bool Writing, class Manager>
    static bool fields(Manager& m, openPMD::Series& series, openPMD::Iteration iteration,
                       const Session& session, std::size_t writerRanks) {
        auto& fc        = *m.fcontainer_m;
        auto& solver    = *m.solver_m;
        auto& layout    = fc.getFL();
        const int first = layout.getLocalNDIndex()[2].first();
        std::vector<int> starts;
        for (int rank = 0; rank < session.size_m; ++rank)
            starts.push_back(layout.getLocalNDIndex(rank)[2].first());
        bool sameLayout = true;
        if constexpr (Writing) {
            iteration.setAttribute("zSlabStarts", starts);
        } else {
            auto saved = iteration.getAttribute("zSlabStarts").get<std::vector<int>>();
            sameLayout = saved == starts;
            starts     = std::move(saved);
        }
        std::sort(starts.begin(), starts.end());
        requireRestart(starts.size() == writerRanks && !starts.empty() && starts.front() == 0
                           && starts.back() < m.nr_m[2]
                           && std::adjacent_find(starts.begin(), starts.end()) == starts.end(),
                       "invalid saved z-slab boundaries");
        auto visit = [&](const char* name, auto& data) {
            field<Writing>(series, iteration.meshes[name], data, starts);
        };
        visit("electric", fc.getE());
        visit("magnetic", fc.getB());
        visit("fourCurrent", fc.getJ());
        visit("potential", solver.A_n);
        visit("previousPotential", solver.A_nm1);
        // FDTDSolverBase::timeShift copies all of A_np1 into A_n before post_step().
        if constexpr (!Writing)
            Kokkos::deep_copy(solver.A_np1.getView(), solver.A_n.getView());

        // Only the owner of the diagnostic exit plane samples or reads this ring.
        const auto ring  = m.rp_fdt_m;
        const int k      = m.nr_m[2] - 3 - first;
        const bool owner = k >= 1 && k < int(fc.getE().getView().extent(2)) - 1;
        const Counts shape{ring.extent(0), ring.extent(1), ring.extent(2), ring.extent(3)};
        auto mesh = iteration.meshes["radiationWindow"];
        if constexpr (Writing)
            axes(mesh, {"slot", "x", "y", "component"});
        using Buffer = Kokkos::View<double****, Kokkos::LayoutRight, Kokkos::HostSpace>;
        Buffer packed;
        if (owner) {
            packed = Buffer("FEL checkpoint radiation", shape[0], shape[1], shape[2], shape[3]);
            if constexpr (Writing)
                Kokkos::deep_copy(packed,
                                  Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), ring));
        }
        double empty = 0;
        queueChunk<Writing>(mesh[openPMD::RecordComponent::SCALAR], shape, {0, 0, 0, 0},
                            owner ? shape : Counts(4, 0), owner ? packed.data() : &empty);
        series.flush();
        if constexpr (!Writing) {
            if (owner) {
                auto host = Kokkos::create_mirror_view(Kokkos::HostSpace(), ring);
                Kokkos::deep_copy(host, packed);
                Kokkos::deep_copy(ring, host);
            }
        }
        return sameLayout;
    }

    // These values affect evolution after the checkpoint. The output location,
    // requested end time and checkpoint interval may change on restart.
    template <class Manager>
    static std::vector<double> physics(const Manager& m) {
        const auto& cfg = m.m_config;
        return {cfg.extents[0],       cfg.extents[1],        cfg.extents[2],
                double(m.nr_m[0]),    double(m.nr_m[1]),     double(m.nr_m[2]),
                cfg.bunch_gamma,      cfg.undulator_K,       cfg.undulator_period,
                cfg.undulator_length, cfg.sigma_position[2], double(cfg.space_charge),
                double(m.nsubsteps_m)};
    }

    // Appending is safe if the diagnostic files still end at the saved step.
    // A fresh output directory starts a new CSV segment with headers.
    static Counts diagnosticSizes(const Path& output, int ranks) {
        Counts sizes;
        for (auto prefix : {"radiation_", "radiation_band_", "feldiag_"}) {
            auto file = output / (prefix + std::to_string(ranks) + ".csv");
            sizes.push_back(std::filesystem::exists(file) ? std::filesystem::file_size(file) : 0);
        }
        return sizes;
    }

    template <class Manager>
    static void save(Manager& m) {
        Session session;
        session.start(ippl::Comm->getCommunicator());
        Path final =
            Path(m.m_config.output_path) / "checkpoints" / ("step_" + std::to_string(m.it_m));
        auto temporaryPath = [&](unsigned attempt) {
            return Path(final.string() + "." + std::to_string(attempt) + ".partial");
        };
        unsigned attempt = 0;
        Counts sizes(3);
        collective(session, [&] {
            if (session.rank_m != 0)
                return;
            std::filesystem::create_directories(final.parent_path());
            check(!std::filesystem::exists(final), "checkpoint already exists: " + final.string());
            while (!std::filesystem::create_directory(temporaryPath(attempt)))
                ++attempt;
            sizes = diagnosticSizes(m.m_config.output_path, session.size_m);
        });
        MPI_Bcast(&attempt, 1, MPI_UNSIGNED, 0, session.comm_m);
        const Path temporary = temporaryPath(attempt);
        MPI_Bcast(sizes.data(), 3, MPI_UINT64_T, 0, session.comm_m);
        Counts counts(session.size_m);
        std::uint64_t local = m.pcontainer_m->getLocalNum();
        MPI_Allgather(&local, 1, MPI_UINT64_T, counts.data(), 1, MPI_UINT64_T, session.comm_m);
        writeParticles(m, temporary / "particles", session);
        openPMD::Series series((temporary / STATE_FILE).string(), openPMD::Access::CREATE,
                               session.comm_m);
        series.setAttribute("ipplFELCheckpointVersion", 1);
        auto iteration = series.iterations[m.it_m];
        iteration.setTime(m.time_m).setDt(m.dt_m).setTimeUnitSI(unit_time_in_seconds);
        iteration.setAttribute("particlesPerRank", counts);
        iteration.setAttribute("diagnosticBytes", sizes);
        iteration.setAttribute("physics", physics(m));
        fields<true>(m, series, iteration, session, counts.size());
        iteration.close();
        series.close();
        MPI_Barrier(session.comm_m);
        collective(session, [&] {
            if (session.rank_m != 0)
                return;
            std::ofstream file(temporary / "config.json");
            file << m.m_config.jobFileContents;
            file.close();
            check(bool(file), "failed to save job configuration");
            std::filesystem::rename(temporary, final);
            std::cout << "FEL checkpoint saved to " << final << '\n';
        });
    }

    template <class Manager>
    static void restore(Manager& m, const Path& path) {
        Session session;
        session.start(ippl::Comm->getCommunicator());
        openPMD::Series series((path / STATE_FILE).string(), openPMD::Access::READ_ONLY,
                               session.comm_m);
        requireRestart(series.getAttribute("ipplFELCheckpointVersion").get<int>() == 1
                           && series.iterations.size() == 1,
                       "unknown checkpoint format");
        auto entry = series.iterations.begin();
        requireRestart(entry->first <= static_cast<std::uint64_t>(m.nt_m),
                       "end time precedes checkpoint");
        m.it_m         = static_cast<int>(entry->first);
        auto iteration = entry->second;
        iteration.open();
        m.time_m = iteration.time<double>();
        requireRestart(
            std::isfinite(m.time_m) && iteration.dt<double>() == m.dt_m
                && iteration.getAttribute("physics").get<std::vector<double>>() == physics(m),
            "incompatible mesh or FEL physics in restart job");
        auto counts           = iteration.getAttribute("particlesPerRank").get<Counts>();
        const auto savedSizes = iteration.getAttribute("diagnosticBytes").get<Counts>();
        bool canAppend        = true;
        collective(session, [&] {
            if (session.rank_m != 0)
                return;
            auto sizes                 = diagnosticSizes(m.m_config.output_path, session.size_m);
            m.writeDiagnosticHeaders_m = sizes == Counts(3, 0);
            canAppend = m.writeDiagnosticHeaders_m
                        || (counts.size() == std::size_t(session.size_m) && sizes == savedSizes);
        });
        requireRestart(canAppend,
                       "CSV files do not end at the checkpoint; select a fresh output.path");
        const bool sameLayout = fields<false>(m, series, iteration, session, counts.size());
        iteration.close();
        series.close();
        readParticles(m, path / "particles", counts, session, sameLayout);
        if (!session.rank_m)
            std::cout << "Restored FEL step " << m.it_m << " from " << path << '\n';
    }
};
#endif
