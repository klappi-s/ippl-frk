#ifndef IPPL_VECTOR_ATTRIBUTE_H
#define IPPL_VECTOR_ATTRIBUTE_H

#include "Ippl.h"

#include "Particle/ParticleAttrib.h"

namespace ippl {
    // Explicit storage takes precedence over IPPL_ENABLE_VECTOR_SOA.
    template <typename T, unsigned Components, typename Storage = SoA, typename... Properties>
        requires(detail::is_vector_layout_v<Storage> && !is_vector_v<T> && Components > 0)
    using VectorAttribute = ParticleAttrib<Vector<T, Components>, Storage, Properties...>;
}  // namespace ippl
#endif
