// SCHEMA.md text writers for FEM reference-data export (serial, rank-0).
#ifndef IPPL_FEM_VALIDATION_DATAEXPORT_H
#define IPPL_FEM_VALIDATION_DATAEXPORT_H

#include <cstddef>
#include <string>
#include <vector>

namespace ippl::femdata {

/// Per-cell local stiffness/load block for SCHEMA export.
struct LocalCellBlock {
    std::vector<std::size_t> global_dofs_1based;
    std::vector<std::vector<double>> matrix;
    std::vector<double> vector;
};

void write_vector(const std::string& path, const std::vector<double>& v, const std::string& description);

void write_constrained_dofs(const std::string& path,
                            const std::vector<std::pair<std::size_t, double>>& dofs_values,
                            const std::string& description);

void write_dense_matrix(const std::string& path, const std::vector<std::vector<double>>& A,
                        const std::string& description);

void write_local_stiffness_matrices(const std::string& path, const std::vector<LocalCellBlock>& cells,
                                    unsigned spatial_dim, unsigned order,
                                    const std::string& description);

void write_local_load_vectors(const std::string& path, const std::vector<LocalCellBlock>& cells,
                              unsigned spatial_dim, unsigned order, const std::string& description);

}  // namespace ippl::femdata

#include "FEM/Validation/FEMDataExport.hpp"

#endif
