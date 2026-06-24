#ifndef IPPL_FEM_HELPER_STRUCTS
#define IPPL_FEM_HELPER_STRUCTS

namespace ippl {

    // Helper struct to check weather templated input is a std::tuple
    // Default case: not a tuple
    template <typename T>
    struct is_tuple : std::false_type {};

    // Specialization for std::tuple
    template <typename... Ts>
    struct is_tuple<std::tuple<Ts...>> : std::true_type {};

    // Convenience variable template
    template <typename T>
    inline constexpr bool is_tuple_v = is_tuple<T>::value;



    // Helper struct to verify that all types in tuple are unique (we do not want duplicate EntityTypes)
    // Check if a single type T is in the list of types Rest...
    template <typename T, typename... Rest>
    struct contains : std::disjunction<std::is_same<T, Rest>...> {};

    // Recursive duplicate check
    template <typename... Ts>
    struct has_duplicates;

    // Default case: no types, no duplicates
    template <>
    struct has_duplicates<> : std::false_type {};

    // Recursive case
    template <typename T, typename... Ts>
    struct has_duplicates<T, Ts...>
        : std::disjunction<contains<T, Ts...>, has_duplicates<Ts...>> {};

    // Convenience variable template
    template <typename... Ts>
    inline constexpr bool has_duplicates_v = has_duplicates<Ts...>::value;

    // Wrapper for tuples
    template <typename Tuple>
    struct tuple_has_duplicates;

    template <typename... Ts>
    struct tuple_has_duplicates<std::tuple<Ts...>> : has_duplicates<Ts...> {};

    // Convenience variable template
    template <typename Tuple>
    inline constexpr bool tuple_has_duplicates_v = tuple_has_duplicates<Tuple>::value;




    // Helper struct to map EntityTypes and DOFNums to corresponding tuple of Field types
    // FieldTraits to map EntityType and NDOF to corresponding Field type
    template <typename T, unsigned Dim, typename Mesh_t, typename EntityType, unsigned NDOF>
    struct FieldTraits {
        // TODO: Currently only supports Cell centering, if Field changes, use centering that matches EntityType
        using type = Field<DOFArray<T, NDOF>, Dim, Mesh_t, Cell>;
    };


    // FieldTupleBuilder to build a tuple of field types based on EntityTypes and DOFNums
    template <typename T, unsigned Dim, typename Mesh_t, typename EntityTypes, typename DOFNums>
    struct FieldTupleBuilder;

    // Recursive case to build the tuple of field types
    template <typename T, unsigned Dim, typename Mesh_t, typename... EntityType, unsigned... DOFNum>
    struct FieldTupleBuilder<T, Dim, Mesh_t, std::tuple<EntityType...>, std::tuple<std::integral_constant<unsigned, DOFNum>...>> {

        using type = std::tuple<typename FieldTraits<T, Dim, Mesh_t, EntityType, DOFNum>::type...>;
        using view_type = std::tuple<typename FieldTraits<T, Dim, Mesh_t, EntityType, DOFNum>::type::view_type...>;
    };


    // Helper to get index of EntityType in EntityTypes...
    // TagIndex to index to access Fields over Entity Type
    template <typename EntityTypes>
    struct TagIndex;

    template <typename... EntityType>
    struct TagIndex<std::tuple<EntityType...>> {
        
        template <typename TestEntity>
        constexpr static bool contains() {
            return std::disjunction_v<std::is_same<TestEntity, EntityType>...>;
        }
        
        template <typename TestEntity>
        constexpr static unsigned index() {
            static_assert(contains<TestEntity>(), "EntityType not found in this FEMContainer");
            
            constexpr unsigned index = []<size_t... Is>(std::index_sequence<Is...>) {
                unsigned result = static_cast<unsigned>(-1);
                ((std::is_same_v<TestEntity, EntityType> ? (result = static_cast<unsigned>(Is), void()) : void()), ...);
                return result;
            }(std::make_index_sequence<sizeof...(EntityType)>{});
            
            return index;
        }
    }; 

} // namespace ippl

#endif //IPPL_FEM_HELPER_STRUCTS