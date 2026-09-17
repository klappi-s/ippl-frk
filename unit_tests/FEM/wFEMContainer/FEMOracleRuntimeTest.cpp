#include "Ippl.h"

#include <chrono>
#include <fstream>

#include "Oracle/FEMOracleRuntime.h"
#include "gtest/gtest.h"

namespace {
    class FEMOracleRuntimeTest : public ::testing::Test {
    protected:
        std::filesystem::path directory_m;
        // Format-1 fixture: dimension 3, two f64 values (1.25, -0.0),
        // two u32 indices (0, UINT32_MAX). Explicit bytes are host-endian independent.
        std::vector<unsigned char> bytes_m{
            'I', 'P', 'P', 'L', 'F', 'E', 'M', 0, 1, 0,   0, 0, 3, 0, 0,   0,   2,   0,  0,
            0,   0,   0,   0,   0,   2,   0,   0, 0, 0,   0, 0, 0, 0, 0,   0,   0,   0,  0,
            244, 63,  0,   0,   0,   0,   0,   0, 0, 128, 0, 0, 0, 0, 255, 255, 255, 255};

        void SetUp() override {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            directory_m      = std::filesystem::temp_directory_path()
                               / ("ippl-oracle-reader-" + std::to_string(stamp));
            ASSERT_TRUE(std::filesystem::create_directory(directory_m));
        }
        void TearDown() override { std::filesystem::remove_all(directory_m); }
        std::filesystem::path write() {
            auto path = directory_m / "data.bin";
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(bytes_m.data()), bytes_m.size());
            return path;
        }
    };

    TEST_F(FEMOracleRuntimeTest, ExactBitsIndicesAndBounds) {
        fem_oracle::runtime::BinaryArrays data(write(), 3);
        const auto values = data.reals(0, 2);
        EXPECT_EQ(values[0], 1.25);
        EXPECT_EQ(std::bit_cast<std::uint64_t>(values[1]), UINT64_C(0x8000000000000000));
        EXPECT_EQ(data.indices(0, 2)[1], UINT32_MAX);
        EXPECT_TRUE(data.reals(2, 0).empty());
        EXPECT_TRUE(data.indices(2, 0).empty());
        EXPECT_THROW(data.reals(1, 2), std::runtime_error);
        EXPECT_THROW(data.indices(3, 0), std::runtime_error);
        EXPECT_THROW(data.reals(0, SIZE_MAX), std::runtime_error);
    }

    TEST_F(FEMOracleRuntimeTest, MissingTruncatedAndTrailingDataAreRejected) {
        EXPECT_THROW(fem_oracle::runtime::BinaryArrays(directory_m / "missing", 3),
                     std::runtime_error);
        bytes_m.pop_back();
        EXPECT_THROW(fem_oracle::runtime::BinaryArrays(write(), 3), std::runtime_error);
        bytes_m.push_back(255);
        bytes_m.push_back(0);
        EXPECT_THROW(fem_oracle::runtime::BinaryArrays(write(), 3), std::runtime_error);
        bytes_m.resize(10);
        EXPECT_THROW(fem_oracle::runtime::BinaryArrays(write(), 3), std::runtime_error);
    }

    TEST_F(FEMOracleRuntimeTest, WrongMagicVersionDimensionAndCountsAreRejected) {
        EXPECT_THROW(fem_oracle::runtime::BinaryArrays(write(), 2), std::runtime_error);
        for (const auto offset : {0u, 8u, 12u, 16u, 24u}) {
            const auto original = bytes_m[offset];
            bytes_m[offset]     = 255;
            EXPECT_THROW(fem_oracle::runtime::BinaryArrays(write(), 3), std::runtime_error)
                << offset;
            bytes_m[offset] = original;
        }
    }
}  // namespace

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
