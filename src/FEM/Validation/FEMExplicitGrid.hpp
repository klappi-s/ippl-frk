#ifndef IPPL_FEM_VALIDATION_EXPLICITGRID_HPP
#define IPPL_FEM_VALIDATION_EXPLICITGRID_HPP

#include <cmath>
#include <stdexcept>

namespace ippl::femdata {
namespace detail {

template <unsigned Dim>
inline void decode_ippl_dof(std::size_t g, const std::array<unsigned, Dim>& nx,
                            std::array<unsigned, Dim>& ix) {
    if constexpr (Dim == 1) {
        ix[0] = static_cast<unsigned>(g);
    } else if constexpr (Dim == 2) {
        ix[0] = static_cast<unsigned>(g % nx[0]);
        ix[1] = static_cast<unsigned>(g / nx[0]);
    } else {
        ix[0] = static_cast<unsigned>(g % nx[0]);
        ix[1] = static_cast<unsigned>((g / nx[0]) % nx[1]);
        ix[2] = static_cast<unsigned>(g / (nx[0] * nx[1]));
    }
}

template <unsigned Dim>
inline ippl::Vector<double, Dim> dof_coord(std::size_t g, const std::array<unsigned, Dim>& nel,
                                           unsigned order, const std::array<double, Dim>& origin,
                                           const std::array<double, Dim>& corner) {
    std::array<unsigned, Dim> nx{};
    std::array<unsigned, Dim> ix{};
    for (unsigned d = 0; d < Dim; ++d) {
        nx[d] = nel[d] * order + 1;
    }
    decode_ippl_dof<Dim>(g, nx, ix);
    ippl::Vector<double, Dim> x{};
    for (unsigned d = 0; d < Dim; ++d) {
        const double denom = static_cast<double>(nx[d] - 1);
        x[d] = origin[d] + static_cast<double>(ix[d]) * (corner[d] - origin[d]) / denom;
    }
    return x;
}

template <unsigned Dim>
inline bool is_boundary_dof(std::size_t g, unsigned order, const std::array<unsigned, Dim>& nel) {
    std::array<unsigned, Dim> nx{};
    std::array<unsigned, Dim> ix{};
    for (unsigned d = 0; d < Dim; ++d) {
        nx[d] = nel[d] * order + 1;
    }
    decode_ippl_dof<Dim>(g, nx, ix);
    for (unsigned d = 0; d < Dim; ++d) {
        if (ix[d] == 0 || ix[d] == nx[d] - 1) {
            return true;
        }
    }
    return false;
}

inline std::vector<double> solve_dirichlet(std::vector<std::vector<double>> A, std::vector<double> b,
                                           const std::vector<std::size_t>& constrained,
                                           double bc_val) {
    const std::size_t n = A.size();
    for (std::size_t c : constrained) {
        for (std::size_t i = 0; i < n; ++i) {
            if (i != c) {
                b[i] -= A[i][c] * bc_val;
            }
            A[i][c] = 0.0;
            A[c][i] = 0.0;
        }
        A[c][c] = 1.0;
        b[c]  = bc_val;
    }
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t piv = k;
        double best     = std::abs(A[k][k]);
        for (std::size_t i = k + 1; i < n; ++i) {
            if (std::abs(A[i][k]) > best) {
                best = std::abs(A[i][k]);
                piv  = i;
            }
        }
        if (best < 1e-30) {
            throw std::runtime_error("FEMExplicitGrid: singular matrix");
        }
        if (piv != k) {
            std::swap(A[k], A[piv]);
            std::swap(b[k], b[piv]);
        }
        const double diag = A[k][k];
        for (std::size_t j = k; j < n; ++j) {
            A[k][j] /= diag;
        }
        b[k] /= diag;
        for (std::size_t i = 0; i < n; ++i) {
            if (i == k) {
                continue;
            }
            const double f = A[i][k];
            if (f == 0.0) {
                continue;
            }
            for (std::size_t j = k; j < n; ++j) {
                A[i][j] -= f * A[k][j];
            }
            b[i] -= f * b[k];
        }
    }
    return b;
}

}  // namespace detail
}  // namespace ippl::femdata

#endif
