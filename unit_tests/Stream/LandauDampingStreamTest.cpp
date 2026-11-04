//
// Unit test: Landau damping shared I/O for independent vector layouts
//
// Checks output against snapshots taken before each write, stable particle IDs during
// MPI migration/repartitioning, selected records, metadata, timers and disabled output.
// Field-only output also checks owned-cell values, geometry and omission of particles.
// HDF5 is the default; IPPL_TEST_BP5 selects BP5 and IPPL_TEST_H5HUT selects H5hut.
// The suite uses the selected Kokkos execution space and one/two MPI ranks.
//

#include <cstdlib>
#include <algorithm>
#include <array>
#include <filesystem>
#include <gtest/gtest.h>

#include "LandauDampingStreamManager.h"
#include "Stream/IO/IOReader.h"

const char* TestName = "LandauDampingStreamTest";

namespace {
    using Manager = alpine_stream::LandauDampingStreamManager;
    using Particles = Manager::ParticleContainer_t;
    static_assert(!ParticleContainer<double, 3>::EnableIDs);
    static_assert(Particles::EnableIDs);

    struct Snapshot {
        std::vector<std::array<double, 3>> positions, velocities;
        std::vector<std::int64_t> ids;
    };

    Snapshot snapshot(Particles& particles) {
        const auto r = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), particles.R.getView());
        const auto p = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), particles.P.getView());
        const auto ids = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), particles.ID.getView());
        Snapshot result;
        for (std::size_t i = 0; i < particles.getLocalNum(); ++i) {
            result.positions.push_back({r(i)[0], r(i)[1], r(i)[2]});
            result.velocities.push_back({p(i)[0], p(i)[1], p(i)[2]});
            result.ids.push_back(ids(i));
        }
        return result;
    }

    std::vector<std::int64_t> globalIds(const Snapshot& data) {
        const int count = static_cast<int>(data.ids.size());
        const auto comm = ippl::Comm->getCommunicator();
        std::vector<int> counts(ippl::Comm->size()), offsets(counts.size(), 0);
        MPI_Allgather(&count, 1, MPI_INT, counts.data(), 1, MPI_INT, comm);
        for (std::size_t i = 1; i < offsets.size(); ++i)
            offsets[i] = offsets[i - 1] + counts[i - 1];
        std::vector<std::int64_t> ids(offsets.back() + counts.back());
        MPI_Allgatherv(data.ids.data(), count, MPI_INT64_T, ids.data(), counts.data(), offsets.data(),
                      MPI_INT64_T, comm);
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    struct FieldSnapshot {
        ippl::Chunk region;
        std::array<std::vector<double>, 3> electric;
        std::vector<double> density;
    };

    FieldSnapshot snapshotFields(Manager& manager) {
        auto& fields = *manager.getFieldContainer();
        auto& e = fields.getE();
        const auto electric = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), e.getView());
        const auto density = Kokkos::create_mirror_view_and_copy(
            Kokkos::HostSpace(), fields.getRho().getView());
        FieldSnapshot result;
        for (unsigned d = 0; d < 3; ++d) {
            result.region.offset.push_back(e.getOwned()[d].first() - e.getDomain()[d].first());
            result.region.extent.push_back(e.getOwned()[d].length());
        }
        const auto ghost = e.getNghost();
        for (std::uint64_t i = 0; i < result.region.extent[0]; ++i)
            for (std::uint64_t j = 0; j < result.region.extent[1]; ++j)
                for (std::uint64_t k = 0; k < result.region.extent[2]; ++k) {
                    for (unsigned d = 0; d < 3; ++d)
                        result.electric[d].push_back(electric(i + ghost, j + ghost, k + ghost)[d]);
                    result.density.push_back(density(i + ghost, j + ghost, k + ghost));
                }
        return result;
    }

    // Check exact field-only readback of owned electric-field and density values over two
    // steps, including geometry/units, payload size and source nonmutation.
    TEST(LandauDampingStream, FieldOnlyOutputPreservesOwnedValues) {
        IpplTimings::resetAllTimers();
        if (ippl::Comm->rank() == 0) {
            std::filesystem::remove_all("fields");
            std::filesystem::remove_all("data");
        }
        ippl::Comm->barrier();
        Vector_t<int, 3> grid(8);
        std::string solver = "FFT", method = "LeapFrog";
        alpine_stream::OutputOptions options;
        if (std::getenv("IPPL_TEST_BP5")) {
            options.writer.openPMD.backend = ippl::OpenPMDBackend::BP5;
            options.writer.openPMD.backendConfig = "{}";
        }
        if (std::getenv("IPPL_TEST_H5HUT"))
            options.writer.backend = ippl::IOBackend::H5hut;
        options.kind = alpine_stream::OutputKind::Fields;
        options.writer.directory = "fields";
        options.writer.stagingBytes = std::getenv("IPPL_TEST_H5HUT") ? 16384 : 128;
        options.lengthUnitSI = 3.0;
        Manager manager(4096, 2, grid, 0.0, solver, method, {}, options);
        manager.pre_run();
        EXPECT_EQ(manager.outputBytes(), 8u * 8u * 8u * 4u * sizeof(double));
        std::vector<FieldSnapshot> expected;
        for (int step = 0; step < 2; ++step) {
            manager.pre_step();
            manager.advance();
            expected.push_back(snapshotFields(manager));
            manager.post_step();
            const auto after = snapshotFields(manager);
            EXPECT_EQ(after.electric, expected.back().electric);
            EXPECT_EQ(after.density, expected.back().density);
        }
        manager.closeOutput();
        manager.writeSummary("field-summary.csv");
        EXPECT_EQ(IpplTimings::getMeasurements("ioWrite").size(), 2u);
        ippl::IOReader reader("fields", ippl::Comm->getCommunicator());
        reader.open();
        for (std::uint64_t step = 1; step <= expected.size(); ++step) {
            const auto info = reader.inspect(step);
            EXPECT_TRUE(info.particles.empty());
            ASSERT_EQ(info.fields.size(), 2u);
            for (const auto& field : info.fields) {
                EXPECT_EQ(field.extent, (std::vector<std::uint64_t>{8, 8, 8}));
                EXPECT_EQ(field.metadata.record.units.dimension, (std::array<double, 7>{}));
                EXPECT_DOUBLE_EQ(field.metadata.gridUnitSI, 3.0);
                EXPECT_EQ(field.metadata.samplePosition, (std::vector<double>{0.5, 0.5, 0.5}));
            }
            const auto& values = expected[step - 1];
            const std::array<std::string, 3> axes{"x", "y", "z"};
            for (unsigned d = 0; d < 3; ++d)
                EXPECT_EQ(reader.readFieldComponent<double>(
                    step, "electric_field", axes[d], values.region), values.electric[d]);
            EXPECT_EQ(reader.readFieldComponent<double>(
                step, "charge_density", "", values.region), values.density);
        }
        reader.close();
    }

    // Check exact per-step particle output and source nonmutation, unique IDs across migration
    // and repartitioning, metadata, I/O timers and the output-disabled path.
    TEST(LandauDampingStream, OutputPreservesParticleStateAndIdsAcrossMigration) {
        IpplTimings::resetAllTimers();
        if (ippl::Comm->rank() == 0) {
            std::filesystem::remove_all("particles");
            std::filesystem::remove_all("disabled-output");
            std::filesystem::remove_all("data");
        }
        ippl::Comm->barrier();
        Vector_t<int, 3> grid(8);
        std::string solver = "FFT", method = "LeapFrog";
        alpine_stream::OutputOptions options;
        if (std::getenv("IPPL_TEST_BP5")) {
            options.writer.openPMD.backend = ippl::OpenPMDBackend::BP5;
            options.writer.openPMD.backendConfig = "{}";
        }
        if (std::getenv("IPPL_TEST_H5HUT"))
            options.writer.backend = ippl::IOBackend::H5hut;
        options.writer.directory = "particles";
        options.writer.stagingBytes = 3072;  // Multiple chunks per component.
        options.lengthUnitSI = 3.0;
        options.writer.timeUnitSI = 5.0;
        // Exercise initial partitioning and rebalance whenever particle counts differ.
        Manager manager(4096, 2, grid, 0.0, solver, method, {}, options);
        manager.pre_run();
        const auto initialIds = globalIds(snapshot(*manager.getParticleContainer()));
        EXPECT_EQ(initialIds.size(), 4096u);
        EXPECT_EQ(std::adjacent_find(initialIds.begin(), initialIds.end()), initialIds.end());
        std::vector<Snapshot> expected;
        for (int step = 0; step < 2; ++step) {
            manager.pre_step();
            manager.advance();
            expected.push_back(snapshot(*manager.getParticleContainer()));
            manager.post_step();
            const auto after = snapshot(*manager.getParticleContainer());
            EXPECT_EQ(after.positions, expected.back().positions);
            EXPECT_EQ(after.velocities, expected.back().velocities);
            EXPECT_EQ(after.ids, expected.back().ids);
            EXPECT_EQ(globalIds(after), initialIds);
        }
        manager.closeOutput();
        manager.writeSummary();
        EXPECT_EQ(IpplTimings::getMeasurements("ioWrite").size(), 2u);
        EXPECT_EQ(IpplTimings::getMeasurements("ioOpen").size(), 1u);
        EXPECT_EQ(IpplTimings::getMeasurements("ioClose").size(), 1u);
        std::uint64_t migrated = 0;
        for (auto id : expected.back().ids)
            migrated += id % ippl::Comm->size() != ippl::Comm->rank();
        MPI_Allreduce(MPI_IN_PLACE, &migrated, 1, MPI_UINT64_T, MPI_SUM,
                      ippl::Comm->getCommunicator());
        if (ippl::Comm->size() > 1) {
            EXPECT_GT(migrated, 0u);
        }

        ippl::IOReader reader("particles", ippl::Comm->getCommunicator());
        reader.open();
        EXPECT_EQ(reader.info().iterations, (std::vector<std::uint64_t>{1, 2}));
        EXPECT_DOUBLE_EQ(reader.info().timeUnitSI, 5.0);
        const std::array<std::string, 3> axes{"x", "y", "z"};
        for (std::uint64_t step = 1; step <= expected.size(); ++step) {
            const auto& values = expected[step - 1];
            const auto info = reader.inspect(step);
            EXPECT_DOUBLE_EQ(info.step.time, step * manager.getDt());
            const auto& species = info.particles.at(0);
            EXPECT_EQ(species.count, 4096u);
            EXPECT_FALSE(species.records.contains("charge"));
            EXPECT_FALSE(species.records.contains("electric_field"));
            EXPECT_FALSE(species.records.contains("ippl_rank"));
            EXPECT_DOUBLE_EQ(species.records.at("velocity").metadata.units.unitSI, 3.0 / 5.0);
            EXPECT_DOUBLE_EQ(species.records.at("velocity").metadata.timeOffset.constant, 0.0);
            std::uint64_t count = values.ids.size(), offset = 0;
            MPI_Exscan(&count, &offset, 1, MPI_UINT64_T, MPI_SUM, ippl::Comm->getCommunicator());
            if (ippl::Comm->rank() == 0)
                offset = 0;
            EXPECT_EQ(reader.readParticleComponent<std::int64_t>(step, "electrons", "ippl_id", "",
                                                                 offset, count), values.ids);
            for (unsigned d = 0; d < axes.size(); ++d) {
                const auto r = reader.readParticleComponent<double>(step, "electrons", "position",
                                                                    axes[d], offset, count);
                const auto p = reader.readParticleComponent<double>(step, "electrons", "velocity",
                                                                    axes[d], offset, count);
                EXPECT_EQ(r.size(), count);
                EXPECT_EQ(p.size(), count);
                for (std::size_t i = 0; i < std::min(r.size(), p.size()); ++i) {
                    EXPECT_DOUBLE_EQ(r[i], values.positions[i][d]);
                    EXPECT_DOUBLE_EQ(p[i], values.velocities[i][d]);
                }
            }
        }
        reader.close();

        IpplTimings::resetAllTimers();
        options.enabled = false;
        options.writer.directory = "disabled-output";
        Manager baseline(4096, 1, grid, 1.0, solver, method, {}, options);
        baseline.pre_run();
        baseline.run(1);
        baseline.closeOutput();
        baseline.writeSummary("disabled-summary.csv");
        EXPECT_EQ(IpplTimings::getMeasurements("ioWrite").size(), 0u);
        EXPECT_EQ(IpplTimings::getMeasurements("timestep").size(), 1u);
        EXPECT_FALSE(std::filesystem::exists("disabled-output"));
        EXPECT_EQ(globalIds(snapshot(*baseline.getParticleContainer())).size(), 4096u);
    }
}  // namespace

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
