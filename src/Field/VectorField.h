#ifndef IPPL_VECTOR_FIELD_H
#define IPPL_VECTOR_FIELD_H

#include "Ippl.h"

#include "Field/Field.h"

namespace ippl {
    // Dim is the mesh dimension; Components can differ (e.g. a 3-vector on a 2D mesh).
    template <typename T, unsigned Dim, typename Storage = AoS,
              typename Mesh      = UniformCartesian<T, Dim>,
              typename Centering = typename Mesh::DefaultCentering, unsigned Components = Dim,
              typename... Properties>
        requires(detail::is_vector_layout_v<Storage> && !is_vector_v<T> && Components > 0)
    using VectorField = Field<Vector<T, Components>, Dim, Mesh, Centering, Storage, Properties...>;
}  // namespace ippl
#endif
