#ifndef IPPL_POISSON_CONVERGENCE_SOLUTIONS_HPP
#define IPPL_POISSON_CONVERGENCE_SOLUTIONS_HPP
#include <Kokkos_Core.hpp>

namespace ippl { namespace poisson_convergence_wfc {

template <typename T>
KOKKOS_INLINE_FUNCTION T pi_v() { return Kokkos::numbers::pi_v<T>; }

// ---------------------------------------------------------------------------
// LowOrderPolynomial
// ---------------------------------------------------------------------------
template <typename T>
KOKKOS_INLINE_FUNCTION T exact_LowOrderPolynomial_1D(T x) {
    return x*(x - T(1));
}
template <typename T>
KOKKOS_INLINE_FUNCTION T source_LowOrderPolynomial_1D(T /*x*/) {
    return T(-2);
}

template <typename T>
KOKKOS_INLINE_FUNCTION T exact_LowOrderPolynomial_2D(T x, T y) {
    return x*(x - T(1)) * y*(y - T(1));
}
template <typename T>
KOKKOS_INLINE_FUNCTION T source_LowOrderPolynomial_2D(T x, T y) {
    return T(-2)*y*(y - T(1)) - T(2)*x*(x - T(1));
}

template <typename T>
KOKKOS_INLINE_FUNCTION T exact_LowOrderPolynomial_3D(T x, T y, T z) {
    return x*(x - T(1)) * y*(y - T(1)) * z*(z - T(1));
}
template <typename T>
KOKKOS_INLINE_FUNCTION T source_LowOrderPolynomial_3D(T x, T y, T z) {
    T uxx = T(2) * y*(y - T(1)) * z*(z - T(1));
    T uyy = x*(x - T(1)) * T(2) * z*(z - T(1));
    T uzz = x*(x - T(1)) * y*(y - T(1)) * T(2);
    return -(uxx + uyy + uzz);
}

// ---------------------------------------------------------------------------
// HighOrderPolynomial
// ---------------------------------------------------------------------------
template <typename T>
KOKKOS_INLINE_FUNCTION T exact_HighOrderPolynomial_1D(T x) {
    return x*Kokkos::pow(x - T(1), T(3));
}
template <typename T>
KOKKOS_INLINE_FUNCTION T source_HighOrderPolynomial_1D(T x) {
    // f = -u'' = -6*(2x^2 - 3x + 1)
    return T(-6)*(T(2)*x*x - T(3)*x + T(1));
}

template <typename T>
KOKKOS_INLINE_FUNCTION T exact_HighOrderPolynomial_2D(T x, T y) {
    return x*Kokkos::pow(x - T(1), T(3)) * Kokkos::pow(y, T(3))*(y - T(1));
}
template <typename T>
KOKKOS_INLINE_FUNCTION T source_HighOrderPolynomial_2D(T x, T y) {
    // uxx = 6(2x^2 - 3x + 1) * y^3(y-1)
    T uxx = T(6)*(T(2)*x*x - T(3)*x + T(1)) * Kokkos::pow(y, T(3))*(y - T(1));
    // uyy = x(x-1)^3 * (12y^2 - 6y)
    T uyy = x*Kokkos::pow(x - T(1), T(3)) * T(6)*y*(T(2)*y - T(1));
    return -(uxx + uyy);
}

template <typename T>
KOKKOS_INLINE_FUNCTION T exact_HighOrderPolynomial_3D(T x, T y, T z) {
    return x*Kokkos::pow(x - T(1), T(3)) * Kokkos::pow(y, T(3))*(y - T(1)) * Kokkos::pow(z, T(4))*(z - T(1));
}
template <typename T>
KOKKOS_INLINE_FUNCTION T source_HighOrderPolynomial_3D(T x, T y, T z) {
    T uxx = T(6)*(T(2)*x*x - T(3)*x + T(1)) * Kokkos::pow(y, T(3))*(y - T(1)) * Kokkos::pow(z, T(4))*(z - T(1));
    T uyy = x*Kokkos::pow(x - T(1), T(3)) * T(6)*y*(T(2)*y - T(1)) * Kokkos::pow(z, T(4))*(z - T(1));
    // uzz = x(x-1)^3 y^3(y-1) * (20z^3 - 12z^2) = ... * 4z^2(5z - 3)
    T uzz = x*Kokkos::pow(x - T(1), T(3)) * Kokkos::pow(y, T(3))*(y - T(1)) * T(4)*z*z*(T(5)*z - T(3));
    return -(uxx + uyy + uzz);
}

// ---------------------------------------------------------------------------
// ShiftedExponential
// g(w) = (e^w - 1)(e^{w-1} - 1)
// g''(w) = 4e^{2w-1} - e^w - e^{w-1}
// ---------------------------------------------------------------------------
template <typename T>
KOKKOS_INLINE_FUNCTION T g_exp(T w) {
    return (Kokkos::exp(w) - T(1)) * (Kokkos::exp(w - T(1)) - T(1));
}
template <typename T>
KOKKOS_INLINE_FUNCTION T g_exp_dd(T w) {
    return T(4)*Kokkos::exp(T(2)*w - T(1)) - Kokkos::exp(w) - Kokkos::exp(w - T(1));
}

template <typename T>
KOKKOS_INLINE_FUNCTION T exact_ShiftedExponential_1D(T x) {
    return g_exp(x);
}
template <typename T>
KOKKOS_INLINE_FUNCTION T source_ShiftedExponential_1D(T x) {
    return -g_exp_dd(x);
}

template <typename T>
KOKKOS_INLINE_FUNCTION T exact_ShiftedExponential_2D(T x, T y) {
    return g_exp(x) * g_exp(y);
}
template <typename T>
KOKKOS_INLINE_FUNCTION T source_ShiftedExponential_2D(T x, T y) {
    return -g_exp_dd(x)*g_exp(y) - g_exp(x)*g_exp_dd(y);
}

template <typename T>
KOKKOS_INLINE_FUNCTION T exact_ShiftedExponential_3D(T x, T y, T z) {
    return g_exp(x) * g_exp(y) * g_exp(z);
}
template <typename T>
KOKKOS_INLINE_FUNCTION T source_ShiftedExponential_3D(T x, T y, T z) {
    T uxx = g_exp_dd(x) * g_exp(y) * g_exp(z);
    T uyy = g_exp(x) * g_exp_dd(y) * g_exp(z);
    T uzz = g_exp(x) * g_exp(y) * g_exp_dd(z);
    return -(uxx + uyy + uzz);
}

// ---------------------------------------------------------------------------
// Sines
// ---------------------------------------------------------------------------
template <typename T>
KOKKOS_INLINE_FUNCTION T exact_Sines_1D(T x) {
    return Kokkos::sin(pi_v<T>() * x);
}
template <typename T>
KOKKOS_INLINE_FUNCTION T source_Sines_1D(T x) {
    return pi_v<T>()*pi_v<T>() * Kokkos::sin(pi_v<T>() * x);
}

template <typename T>
KOKKOS_INLINE_FUNCTION T exact_Sines_2D(T x, T y) {
    return Kokkos::sin(pi_v<T>() * x) * Kokkos::sin(T(2)*pi_v<T>() * y);
}
template <typename T>
KOKKOS_INLINE_FUNCTION T source_Sines_2D(T x, T y) {
    return T(5) * pi_v<T>()*pi_v<T>() * exact_Sines_2D(x, y);
}

template <typename T>
KOKKOS_INLINE_FUNCTION T exact_Sines_3D(T x, T y, T z) {
    return Kokkos::sin(pi_v<T>() * x) * Kokkos::sin(T(2)*pi_v<T>() * y) * Kokkos::sin(T(3)*pi_v<T>() * z);
}
template <typename T>
KOKKOS_INLINE_FUNCTION T source_Sines_3D(T x, T y, T z) {
    return T(14) * pi_v<T>()*pi_v<T>() * exact_Sines_3D(x, y, z);
}

} } // namespaces
#endif
