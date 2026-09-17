#include "Oracle/FEMOracleTestData.h"
#if FEM_ORACLE_DIM == 1
#include <FEMOracleBindings_1D.h>
using Data = fem_oracle::runtime::Data1;
#elif FEM_ORACLE_DIM == 2
#include <FEMOracleBindings_2D.h>
using Data = fem_oracle::runtime::Data2;
#elif FEM_ORACLE_DIM == 3
#include <FEMOracleBindings_3D.h>
using Data = fem_oracle::runtime::Data3;
#else
#error "FEM_ORACLE_DIM must be 1, 2 or 3"
#endif

namespace {
    const Data& data() {
        static const Data instance(std::filesystem::path(FEM_ORACLE_DATA_DIR)
                                   / ("fem-" + std::to_string(FEM_ORACLE_DIM) + "d.bin"));
        return instance;
    }
}  // namespace

std::span<const fem_oracle::OperatorCase> fem_oracle::operatorCases() {
    return data().operatorCases;
}

std::span<const fem_oracle::ReferenceCase> fem_oracle::referenceCases() {
    return data().referenceCases;
}

std::span<const fem_oracle::Case> fem_oracle::legacyCases() {
    return data().legacyCases;
}
