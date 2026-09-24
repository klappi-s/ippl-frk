#include "Ippl.h"

#include "Utility/IpplTimings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "gtest/gtest.h"

namespace {
    std::string outputPath() {
        long long stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        MPI_Bcast(&stamp, 1, MPI_LONG_LONG_INT, 0, ippl::Comm->getCommunicator());
        return (std::filesystem::temp_directory_path()
                / ("ippl-timers-" + std::to_string(stamp) + ".csv"))
            .string();
    }

    void checkExport(const std::string& delimiter, bool header) {
        Timing timing;
        const int rank = ippl::Comm->rank();
        // Empty rank one and unequal payloads exercise zero counts and chunk tails.
        // Long names cross multiple 1 MiB transfer boundaries without large allocations.
        const std::string suffix(8192, 'x');
        const std::string name = "rank" + std::to_string(rank) + suffix;
        timing.getTimer(name.c_str());
        const int count = rank == 1 ? 0 : 300 + rank;
        timing.infoTimer(name.c_str())->measurements.assign(count, rank + 0.25);
        const auto path = outputPath();
        timing.dumpToCSV(path, delimiter, header);
        if (rank == 0) {
            std::ostringstream expected;
            if (header)
                expected << "timer_name" << delimiter << "rank" << delimiter << "measurement_id"
                         << delimiter << "duration_seconds\n";
            for (int source = 0; source < ippl::Comm->size(); ++source) {
                for (int index = 0; index < (source == 1 ? 0 : 300 + source); ++index)
                    expected << "rank" << source << suffix << delimiter << source << delimiter
                             << index << delimiter << source + 0.25 << '\n';
            }
            std::ifstream file(path);
            const std::string actual((std::istreambuf_iterator<char>(file)), {});
            EXPECT_EQ(actual, expected.str());
            std::filesystem::remove(path);
        }
    }
}  // namespace

TEST(IpplTimings, ChunkedRankOrderedCSV) {
    checkExport(",", true);
}
TEST(IpplTimings, CustomDelimiterWithoutHeader) {
    checkExport(";", false);
}

TEST(IpplTimings, EmptyMeasurements) {
    Timing timing;
    const auto path = outputPath();
    timing.dumpToCSV(path);
    if (ippl::Comm->rank() == 0) {
        std::ifstream file(path);
        const std::string actual((std::istreambuf_iterator<char>(file)), {});
        EXPECT_EQ(actual, "timer_name,rank,measurement_id,duration_seconds\n");
        std::filesystem::remove(path);
    }
}

TEST(IpplTimings, OpenFailureReportedOnEveryRank) {
    Timing timing;
    EXPECT_THROW(timing.dumpToCSV(outputPath() + "/missing.csv"), std::runtime_error);
}

TEST(IpplTimings, WriteFailureReportedOnEveryRank) {
    if (!std::filesystem::exists("/dev/full"))
        GTEST_SKIP() << "Requires the Linux full device";
    Timing timing;
    timing.getTimer("write_failure");
    timing.infoTimer("write_failure")->measurements.assign(10, 0.25);
    EXPECT_THROW(timing.dumpToCSV("/dev/full"), std::runtime_error);
}

TEST(IpplTimings, SummaryKeepsAllStatisticsWithoutSamples) {
    Timing timing;
    timing.setSampleLimit(0);
    const auto ref = timing.getTimer("summary");
    std::vector<double> observed;
    for (unsigned i = 0; i < 2000; ++i) {
        timing.startTimer(ref);
        timing.stopTimer(ref);
        observed.push_back(timing.getLastMeasurement(ref));
    }
    const auto* info = timing.infoTimer("summary");
    EXPECT_EQ(timing.getCallCount(ref), observed.size());
    EXPECT_TRUE(timing.getMeasurements(ref).empty());
    EXPECT_EQ(info->measurements.capacity(), 0);
    double sum = 0;
    for (double value : observed)
        sum += value;
    EXPECT_DOUBLE_EQ(info->wallTime, sum);
    EXPECT_DOUBLE_EQ(info->minMeasurement, *std::min_element(observed.begin(), observed.end()));
    EXPECT_DOUBLE_EQ(info->maxMeasurement, *std::max_element(observed.begin(), observed.end()));
    EXPECT_NEAR(info->meanMeasurement, sum / observed.size(), 1e-15);
    double variance = 0;
    for (double value : observed)
        variance += std::pow(value - sum / observed.size(), 2);
    EXPECT_NEAR(info->squaredDeviations, variance, 1e-15);
    timing.clearTimer(ref);  // Stopping/resetting an interval preserves accumulated statistics.
    EXPECT_EQ(timing.getCallCount(ref), observed.size());
    timing.resetAllTimers();
    EXPECT_EQ(timing.getCallCount(ref), 0);
    EXPECT_EQ(timing.getLastMeasurement(ref), 0);
    EXPECT_EQ(info->wallTime, 0);
    EXPECT_EQ(info->minMeasurement, 0);
}

TEST(IpplTimings, RawCaptureIsBoundedAndPolicyCoversExistingTimers) {
    Timing timing;
    const auto ref = timing.getTimer("bounded");
    timing.setSampleLimit(3);
    for (unsigned i = 0; i < 100; ++i) {
        timing.startTimer(ref);
        timing.stopTimer(ref);
    }
    EXPECT_EQ(timing.getCallCount(ref), 100);
    EXPECT_EQ(timing.getMeasurementCount(ref), 3);
    timing.setSampleLimit(0);
    EXPECT_EQ(timing.getCallCount(ref), 100);
    EXPECT_EQ(timing.infoTimer("bounded")->measurements.capacity(), 0);
    timing.startTimer(ref);
    timing.stopTimer(ref);
    EXPECT_EQ(timing.getCallCount(ref), 101);
    EXPECT_TRUE(timing.getMeasurements(ref).empty());
}

TEST(IpplTimings, SummaryExportIncludesEmptyTimersAndUnstoredCalls) {
    Timing timing;
    timing.setSampleLimit(0);
    timing.getTimer("unused");
    const auto ref = timing.getTimer("active");
    timing.startTimer(ref);
    timing.stopTimer(ref);
    const auto path = outputPath();
    timing.dumpSummaryToCSV(path);
    if (ippl::Comm->rank() == 0) {
        std::ifstream file(path);
        std::string line;
        std::getline(file, line);
        EXPECT_EQ(line,
                  "timer_name,rank,count,total_seconds,min_seconds,max_seconds,mean_seconds,stddev_"
                  "seconds,stored_samples");
        for (int rank = 0; rank < ippl::Comm->size(); ++rank) {
            std::getline(file, line);
            EXPECT_EQ(line, "unused," + std::to_string(rank) + ",0,0,0,0,0,0,0");
            std::getline(file, line);
            EXPECT_EQ(line.find("active," + std::to_string(rank) + ",1,"), 0);
            EXPECT_TRUE(line.ends_with(",0,0"));
        }
        EXPECT_FALSE(std::getline(file, line));
        std::filesystem::remove(path);
    }
    EXPECT_THROW(timing.dumpSummaryToCSV(path + "/missing.csv"), std::runtime_error);
}

int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
