#ifndef IPPL_FEM_VALIDATION_DATAEXPORT_HPP
#define IPPL_FEM_VALIDATION_DATAEXPORT_HPP

#include <fstream>
#include <iomanip>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace ippl::femdata {

namespace detail {

inline void write_header_line(std::ofstream& f, const std::string& line) {
    f << line << '\n';
}

inline std::string local_order_label(unsigned nloc, unsigned dim) {
    const unsigned root = static_cast<unsigned>(std::round(std::pow(static_cast<double>(nloc), 1.0 / dim)));
    if (root < 2 || root * root * (dim == 3 ? root : 1) != nloc && dim == 3) {
        if (dim == 2 && root * root == nloc) {
            const unsigned p = root - 1;
            if (p == 1) {
                return "BL(0), BR(1), TR(2), TL(3)  [CCW]";
            }
        }
    }
    if (dim == 1) {
        const unsigned p = nloc - 1;
        if (p >= 1 && nloc == p + 1) {
            return p == 1 ? "left(0), right(1)" : "left(0), right(1), edge_interior L→R";
        }
    }
    if (dim == 2) {
        const unsigned p = root - 1;
        if (root * root == nloc) {
            return "BL(0), BR(1), TR(2), TL(3)  [CCW]; IPPL local order";
        }
    }
    return std::to_string(nloc) + " local DOFs";
}

}  // namespace detail

inline void write_vector(const std::string& path, const std::vector<double>& v,
                         const std::string& description) {
    std::ofstream f(path);
    if (!f) {
        throw std::runtime_error("FEMDataExport: cannot open " + path);
    }
    detail::write_header_line(f, "# " + description);
    detail::write_header_line(f, "# format: index value");
    detail::write_header_line(f, "# index_base: 1");
    f << "# length: " << v.size() << '\n';
    for (std::size_t i = 0; i < v.size(); ++i) {
        f << (i + 1) << ' ' << std::setprecision(17) << v[i] << '\n';
    }
}

inline void write_constrained_dofs(const std::string& path,
                                   const std::vector<std::pair<std::size_t, double>>& dofs_values,
                                   const std::string& description) {
    auto items = dofs_values;
    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::ofstream f(path);
    if (!f) {
        throw std::runtime_error("FEMDataExport: cannot open " + path);
    }
    detail::write_header_line(f, "# " + description);
    detail::write_header_line(f, "# format: dof value");
    detail::write_header_line(f, "# index_base: 1");
    detail::write_header_line(f, "# dof_order: IPPL lexicographic (x-innermost, z-outermost)");
    detail::write_header_line(
        f, "# note: BC-application convention (elimination/lifting/penalty) is left to the consumer");
    f << "# n_constrained: " << items.size() << '\n';
    for (const auto& [dof, val] : items) {
        f << dof << ' ' << std::setprecision(17) << val << '\n';
    }
}

inline void write_dense_matrix(const std::string& path, const std::vector<std::vector<double>>& A,
                               const std::string& description) {
    std::ofstream f(path);
    if (!f) {
        throw std::runtime_error("FEMDataExport: cannot open " + path);
    }
    const std::size_t nr = A.size();
    const std::size_t nc = nr ? A[0].size() : 0;
    detail::write_header_line(f, "# " + description);
    detail::write_header_line(f, "# format: row col value");
    detail::write_header_line(f, "# index_base: 1");
    f << "# nrows: " << nr << '\n';
    f << "# ncols: " << nc << '\n';
    for (std::size_t i = 0; i < nr; ++i) {
        for (std::size_t j = 0; j < nc; ++j) {
            f << (i + 1) << ' ' << (j + 1) << ' ' << std::setprecision(17) << A[i][j] << '\n';
        }
    }
}

inline void write_local_blocks(const std::string& path, const std::vector<LocalCellBlock>& cells,
                               unsigned spatial_dim, unsigned order, bool include_matrix,
                               bool include_vector, const std::string& description) {
    if (cells.empty()) {
        throw std::runtime_error("FEMDataExport: no local cell blocks");
    }
    const std::size_t nloc = cells[0].matrix.empty() ? cells[0].vector.size() : cells[0].matrix.size();

    std::ofstream f(path);
    if (!f) {
        throw std::runtime_error("FEMDataExport: cannot open " + path);
    }
    detail::write_header_line(f, "# " + description);
    detail::write_header_line(f, "# index_base: 1");
    f << "# n_cells: " << cells.size() << '\n';
    f << "# n_local_dofs: " << nloc << '\n';
    detail::write_header_line(f, "# cell_order: IPPL lexicographic (x-innermost, z-outermost)");
    f << "# local_node_order (Ferrite/IPPL convention): "
      << detail::local_order_label(static_cast<unsigned>(nloc), spatial_dim) << '\n';
    detail::write_header_line(
        f, "#   (FEniCSx DOLFINx internal order differs; data is permuted to Ferrite/IPPL)");
    detail::write_header_line(f, "# GLOBAL_DOFS: 1-based IPPL lexicographic DOF indices");
    detail::write_header_line(f, "# block format:");
    detail::write_header_line(f, "#   BEGIN_CELL <ippl_cell_id>");
    detail::write_header_line(f, "#   GLOBAL_DOFS d1 d2 ...  (IPPL 1-based, Ferrite/IPPL local order)");
    if (include_matrix) {
        detail::write_header_line(f, "#   MATRIX local_row local_col value ...");
    }
    if (include_vector) {
        detail::write_header_line(f, "#   VECTOR local_index value ...");
    }
    detail::write_header_line(f, "#   END_CELL");

    for (std::size_t k = 0; k < cells.size(); ++k) {
        const auto& cell = cells[k];
        f << "BEGIN_CELL " << (k + 1) << '\n';
        f << "GLOBAL_DOFS";
        for (auto g : cell.global_dofs_1based) {
            f << ' ' << g;
        }
        f << '\n';
        if (include_matrix) {
            for (std::size_t i = 0; i < cell.matrix.size(); ++i) {
                for (std::size_t j = 0; j < cell.matrix[i].size(); ++j) {
                    f << "MATRIX " << (i + 1) << ' ' << (j + 1) << ' ' << std::setprecision(17)
                      << cell.matrix[i][j] << '\n';
                }
            }
        }
        if (include_vector) {
            for (std::size_t i = 0; i < cell.vector.size(); ++i) {
                f << "VECTOR " << (i + 1) << ' ' << std::setprecision(17) << cell.vector[i] << '\n';
            }
        }
        f << "END_CELL\n";
    }
}

inline void write_local_stiffness_matrices(const std::string& path,
                                           const std::vector<LocalCellBlock>& cells,
                                           unsigned spatial_dim, unsigned order,
                                           const std::string& description) {
    write_local_blocks(path, cells, spatial_dim, order, true, false, description);
}

inline void write_local_load_vectors(const std::string& path, const std::vector<LocalCellBlock>& cells,
                                     unsigned spatial_dim, unsigned order,
                                     const std::string& description) {
    write_local_blocks(path, cells, spatial_dim, order, false, true, description);
}

}  // namespace ippl::femdata

#endif
