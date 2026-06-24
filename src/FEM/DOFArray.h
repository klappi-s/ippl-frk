// struct wrapped around Kokkos Array to allow operators on Kokkos::Array

#ifndef IPPL_DOFARRAY_H
#define IPPL_DOFARRAY_H

namespace ippl {

    // Helper struct wrapped around Kokkos Array to allow operators on Kokkos::Array
    template <typename T, std::size_t N>
    struct DOFArray {
        Kokkos::Array<T, N> data;

        KOKKOS_INLINE_FUNCTION
        DOFArray() = default;

        KOKKOS_INLINE_FUNCTION
        DOFArray(T value) {
            for (std::size_t i = 0; i < N; ++i) {
                data[i] = value;
            }
        }

        KOKKOS_INLINE_FUNCTION
        T& operator[](int i) { return data[i]; }          // read/write access
        const T& operator[](int i) const { return data[i]; } // read-only access

        KOKKOS_INLINE_FUNCTION
        DOFArray& operator+=(const DOFArray& other) {
            for (std::size_t i = 0; i < N; ++i) {
                data[i] += other.data[i];
            }
            return *this;
        }

        KOKKOS_INLINE_FUNCTION
        DOFArray& operator+=(T value) {
            for (std::size_t i = 0; i < N; ++i) {
                data[i] += value;
            }
            return *this;
        }

        KOKKOS_INLINE_FUNCTION
        DOFArray operator+(const DOFArray& other) const {
            DOFArray result;
            for (std::size_t i = 0; i < N; ++i) {
                result.data[i] = data[i] + other.data[i];
            }
            return result;
        }

        KOKKOS_INLINE_FUNCTION
        DOFArray operator+(T value) const {
            DOFArray result;
            for (std::size_t i = 0; i < N; ++i) {
                result.data[i] = data[i] + value;
            }
            return result;
        }

        KOKKOS_INLINE_FUNCTION
        DOFArray operator-(const DOFArray& other) const {
            DOFArray result;
            for (std::size_t i = 0; i < N; ++i) {
                result.data[i] = data[i] - other.data[i];
            }
            return result;
        }

        KOKKOS_INLINE_FUNCTION
        DOFArray operator-(T value) const {
            DOFArray result;
            for (std::size_t i = 0; i < N; ++i) {
                result.data[i] = data[i] - value;
            }
            return result;
        }

        KOKKOS_INLINE_FUNCTION
        DOFArray& operator-=(const DOFArray& other) {
            for (std::size_t i = 0; i < N; ++i) {
                data[i] -= other.data[i];
            }
            return *this;
        }
        KOKKOS_INLINE_FUNCTION
        DOFArray operator=(T value) {
            for (std::size_t i = 0; i < N; ++i) {
                data[i] = value;
            }
            return *this;
        }
        
        // Element-wise multiplication
        KOKKOS_INLINE_FUNCTION
        DOFArray operator*(const DOFArray& other) const {
            DOFArray result;
            for (std::size_t i = 0; i < N; ++i) {
                result.data[i] = data[i] * other.data[i];
            }
            return result;
        }

        // Scalar multiplication
        KOKKOS_INLINE_FUNCTION
        DOFArray operator*(T scalar) const {
            DOFArray result;
            for (std::size_t i = 0; i < N; ++i) {
                result.data[i] = data[i] * scalar;
            }
            return result;
        }

        // Friend function for scalar * DOFArray
        KOKKOS_INLINE_FUNCTION
        friend DOFArray operator*(T scalar, const DOFArray& arr) {
            return arr * scalar;
        }

        // Scalar division
        KOKKOS_INLINE_FUNCTION
        DOFArray operator/(T scalar) const {
            DOFArray result;
            for (std::size_t i = 0; i < N; ++i) {
                result.data[i] = data[i] / scalar;
            }
            return result;
        }
    };

    // Stream output operator
    template <typename T, std::size_t N>
    inline std::ostream& operator<<(std::ostream& out, const DOFArray<T, N>& arr) {
        out << "[";
        for (std::size_t i = 0; i < N; ++i) {
            out << arr.data[i];
            if (i != N - 1) out << ", ";
        }
        out << "]";
        return out;
    }

} // namespace ippl

// Specialize Kokkos::reduction_identity for DOFArray to enable Kokkos reductions
// This follows the same pattern as ippl::Vector in BareField.hpp
namespace Kokkos {
    template <typename T, std::size_t N>
    struct reduction_identity<ippl::DOFArray<T, N>> {
        KOKKOS_FORCEINLINE_FUNCTION static ippl::DOFArray<T, N> sum() {
            return ippl::DOFArray<T, N>(reduction_identity<T>::sum());
        }
        KOKKOS_FORCEINLINE_FUNCTION static ippl::DOFArray<T, N> prod() {
            return ippl::DOFArray<T, N>(reduction_identity<T>::prod());
        }
        KOKKOS_FORCEINLINE_FUNCTION static ippl::DOFArray<T, N> min() {
            return ippl::DOFArray<T, N>(reduction_identity<T>::min());
        }
        KOKKOS_FORCEINLINE_FUNCTION static ippl::DOFArray<T, N> max() {
            return ippl::DOFArray<T, N>(reduction_identity<T>::max());
        }
    };
}

#endif // IPPL_DOFARRAY_H