#include "Oracle/FEMOracleTestData.h"
#include "Oracle/FEMOracleTestSupport.h"
#include "TestUtils.h"

#if FEM_ORACLE_DIM == 1
namespace oracle = fem_oracle::d1;
#elif FEM_ORACLE_DIM == 2
namespace oracle = fem_oracle::d2;
#elif FEM_ORACLE_DIM == 3
namespace oracle = fem_oracle::d3;
#endif

namespace support = fem_oracle::test;
// CTest partitions only the expensive column loop; every column runs exactly once.
unsigned columnShard = 0, columnShards = 1;

TEST(FEMOperatorRegistry, EveryReferenceCaseHasOneOperatorAndCompiledType) {
    for (const auto& ref : fem_oracle::referenceCases()) {
        EXPECT_EQ(std::count_if(
                      fem_oracle::operatorCases().begin(), fem_oracle::operatorCases().end(),
                      [&](const auto& c) {
                          return std::string_view(c.geometry.name) == ref.name
                                 && c.geometry.reference.nodes.data() == ref.reference.nodes.data();
                      }),
                  1)
            << ref.name;
    }
    for (const auto& c : fem_oracle::operatorCases()) {
        const auto count = []<typename... Tags>(std::tuple<Tags...>, const auto& ref) {
            return (0 + ... + int(support::matches<Tags>(ref)));
        }(oracle::CaseTypes{}, c.geometry);
        EXPECT_EQ(count, 1) << c.geometry.name;
    }
}

int main(int argc, char** argv) {
    ippl::initialize(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    support::configureStiffnessMode();
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg.starts_with("--fem-column-shard="))
            columnShard = std::stoul(std::string(arg.substr(19)));
        if (arg.starts_with("--fem-column-shards="))
            columnShards = std::stoul(std::string(arg.substr(20)));
    }
    if (columnShards == 0 || columnShard >= columnShards) {
        std::cerr << "Invalid column shard.\n";
        ippl::finalize();
        return 1;
    }
    int result = RUN_ALL_TESTS();
    ippl::finalize();
    return result;
}
