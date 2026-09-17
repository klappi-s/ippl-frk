#pragma once
#include <FEMOracleRegistry.h>
namespace fem_oracle {
    // One immutable runtime dataset per dimension and MPI process.
    std::span<const OperatorCase> operatorCases();
    std::span<const ReferenceCase> referenceCases();
    std::span<const Case> legacyCases();
}  // namespace fem_oracle
