#ifndef IPPL_CONDUIT_VECTOR_H
#define IPPL_CONDUIT_VECTOR_H

#include <Kokkos_Core.hpp>

#include <catalyst_conduit.hpp>
#include <stdexcept>
#include <type_traits>

namespace ippl::detail {
    // Conduit's wrapper lacks long long overloads when its int64 type is long.
    template <typename T>
    auto conduitCompatiblePtr(T* ptr) {
        if constexpr (std::is_same_v<T, long long>) {
            static_assert(sizeof(T) == sizeof(conduit_int64));
            static_assert(alignof(T) == alignof(conduit_int64));
            return reinterpret_cast<conduit_int64*>(ptr);
        } else if constexpr (std::is_same_v<T, unsigned long long>) {
            static_assert(sizeof(T) == sizeof(conduit_uint64));
            static_assert(alignof(T) == alignof(conduit_uint64));
            return reinterpret_cast<conduit_uint64*>(ptr);
        } else {
            return ptr;
        }
    }

    /** Bind x/y/z to host vector storage without owning or rearranging it.
     * The caller keeps the view alive in ViewRegistry. Multi-dimensional fields must
     * already be packed in the desired spatial order; rank-one particle views may be strided.
     * An rvalue Node handle (e.g. field["values"]) avoids Conduit's deep-copy constructor.
     */
    template <typename View>
    void setConduitVectorValues(conduit_cpp::Node&& values, const View& view, size_t count) {
        static_assert(Kokkos::SpaceAccessibility<Kokkos::DefaultHostExecutionSpace,
                                                 typename View::memory_space>::accessible);
        using Vector                  = typename View::value_type;
        using Scalar                  = typename Vector::value_type;
        constexpr const char* Names[] = {"x", "y", "z"};
        if (count > view.size())
            throw std::invalid_argument("Conduit vector count exceeds view size");
        for (unsigned d = 0; d < Vector::dim && d < 3; ++d) {
            Scalar* data  = nullptr;
            size_t stride = sizeof(Scalar);
            if (count != 0) {
                if constexpr (requires { view.component(d); }) {
                    auto component = view.component(d);
                    data           = component.data();
                    if constexpr (View::rank == 1) {
                        stride *= component.stride(0);
                    } else if (!component.span_is_contiguous()) {
                        throw std::invalid_argument("Conduit field components must be packed");
                    }
                } else {
                    data   = &view.data()[0][d];
                    stride = sizeof(Vector);
                    if constexpr (View::rank == 1) {
                        stride *= view.stride(0);
                    } else if (!view.span_is_contiguous()) {
                        throw std::invalid_argument("Conduit vector field must be packed");
                    }
                }
            }
            values[Names[d]].set_external(conduitCompatiblePtr(data), count, 0, stride);
        }
    }
}  // namespace ippl::detail

#endif
