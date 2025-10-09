#pragma once


#include "RegistryHelper.h"

// class RegistryBase{

//     public:

//     virtual ~RegistryBase() = default;

// };

// // A dynamic registry with compile-time name-only API via nested mappings
// class RegistryDynamic : public RegistryBase {



// A dynamic registry with compile-time name-only API via nested mappings
class RegistryDynamic {


private:
    std::unordered_map<std::string, void*> m_storage;  // string→void* storage


    // Runtime string API
    template<typename T>
    void set_named(const std::string& name, T& object) {
        m_storage[name] = const_cast<void*>(static_cast<const void*>(&object));
    }

    template<typename T>
    T* get_named(const std::string& name) const {
        auto it = m_storage.find(name);
        return (it != m_storage.end()) ? static_cast<T*>(it->second) : nullptr;
    }

    bool contains_named(const std::string& name) const {
        return m_storage.find(name) != m_storage.end();
    }

    bool unset_named(const std::string& name) {
        return m_storage.erase(name) > 0;
    }
};
// public:
//     // Quick check whether the registry has any bindings
//     bool empty() const noexcept { return m_storage.empty(); }

//     // Nested mapping: default unknown names to void
//     template<fixed_string Name>
//     struct NameToType { using type = void; };

//     // Add/bind with compile-time name (SFINAE constrained by NameToType)
//     template<fixed_string Name, typename U>
//     auto Set(U& object) -> std::enable_if_t<
//         std::is_same_v<typename NameToType<Name>::type, std::remove_const_t<U>>, void>
//     {
//         std::string key{ Name.sv() };
//         m_storage[key] = const_cast<void*>(static_cast<const void*>(&object));
//     }

//     // Get with compile-time name (SFINAE ensures known name)
//     template<fixed_string Name>
//     auto Get() -> std::enable_if_t<
//         !std::is_same_v<typename NameToType<Name>::type, void>,
//         typename NameToType<Name>::type&
//     >
//     {
//         using T = typename NameToType<Name>::type;
//         std::string key{ Name.sv() };
//         auto it = m_storage.find(key);
//         if (it == m_storage.end() || it->second == nullptr) {
//             throw std::runtime_error("Null or missing entry for ID: " + key);
//         }
//         return *static_cast<T*>(it->second);
//     }

    // template<fixed_string Name>
    // auto Get() const -> std::enable_if_t<
    //     !std::is_same_v<typename NameToType<Name>::type, void>,
    //     const typename NameToType<Name>::type&
    // >
    // {
    //     using T = typename NameToType<Name>::type;
    //     std::string key{ Name.sv() };
    //     auto it = m_storage.find(key);
    //     if (it == m_storage.end() || it->second == nullptr) {
    //         throw std::runtime_error("Null or missing entry for ID: " + key);
    //     }
    //     return *static_cast<const T*>(it->second);
    // }

    // // Contains with compile-time name only (SFINAE ensures known name)
    // template<fixed_string Name>
    // auto Contains() const -> std::enable_if_t<
    //     !std::is_same_v<typename NameToType<Name>::type, void>, bool>
    // {
    //     std::string key{ Name.sv() };
    //     return m_storage.find(key) != m_storage.end();
    // }

    // // Optional: Unset/remove binding by compile-time name
    // template<fixed_string Name>
    // auto Unset() -> std::enable_if_t<
    //     !std::is_same_v<typename NameToType<Name>::type, void>, bool>
    // {
    //     std::string key{ Name.sv() };
    //     return m_storage.erase(key) > 0;
    // }


    
    // /* Tag Based API Overload */
    // template<fixed_string Name, typename U>
    // auto Set(id_tag<Name>, U& object) -> std::enable_if_t<        std::is_same_v<typename NameToType<Name>::type, std::remove_const_t<U>>, void>
    // {        this->template Set<Name>(object);}

    // template <fixed_string Name>
    // auto& Get(id_tag<Name>) { return this->template Get<Name>(); }
    
    // template <fixed_string Name>
    // const auto& Get(id_tag<Name>) const { return this->template Get<Name>(); }
    
    // template<fixed_string Name>
    // auto Contains(id_tag<Name>) const -> std::enable_if_t< !std::is_same_v<typename NameToType<Name>::type, void>, bool>
    // {        return this->template Contains<Name>();}

    // template<fixed_string Name>
    // auto Unset(id_tag<Name>) -> std::enable_if_t<!std::is_same_v<typename NameToType<Name>::type, void>, bool>
    // {        return this->template Unset<Name>();   }




// Macro to register name→type for this handler (must be at namespace scope)
#define REGDYN_REGISTER_NAME_TYPE(name_lit, ...) \
    template<> struct RegistryDynamic::NameToType<fixed_string{name_lit}> { using type = __VA_ARGS__; }












// // A safe, modern dynamic registry
// class ViewRegistry {
// private:
//     std::unordered_map<std::string, std::any> m_storage;

// public:
//     // Store any object that can be copied into an 'any'
//     template<typename T>
//     void set(const std::string& name, T object) {
//         m_storage[name] = object;
//     }

//     // Retrieve a shared_ptr of a specific type
//     template<typename T>
//     std::shared_ptr<T> get(const std::string& name) const {
//         auto it = m_storage.find(name);
//         if (it == m_storage.end()) {
//             return nullptr;
//         }
//         // Try to cast the 'any' back to the requested type.
//         // Returns nullptr on failure.
//         try {
//             return std::any_cast<std::shared_ptr<T>>(it->second);
//         } catch (const std::bad_any_cast&) {
//             return nullptr;
//         }
//     }

//     void unset(const std::string& name) {
//         m_storage.erase(name);
//     }
// };