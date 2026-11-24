#pragma once

// Engine services reachable from a World. Modules only receive World& (that is
// the Flecs module contract), so engine-owned objects that are not ECS data —
// the platform backend, the asset server, the script runtime, the RHI device —
// are looked up here. The engine provides them in init order and revokes them
// in reverse order. This is per-world state, not a global.
//
//   auto& assets = veach::services(world).get<veach::AssetServer>();

#include <veach/core/assert.hpp>
#include <veach/ecs/flecs.hpp>

#include <string_view>
#include <typeinfo>
#include <unordered_map>

namespace veach {

class Services {
public:
    template <class T>
    void provide(T& service) {
        entries_[key<T>()] = Entry{&service, typeid(T).name()};
    }

    template <class T>
    void revoke() {
        entries_.erase(key<T>());
    }

    template <class T>
    [[nodiscard]] T* find() const {
        const auto it = entries_.find(key<T>());
        return it == entries_.end() ? nullptr : static_cast<T*>(it->second.ptr);
    }

    template <class T>
    [[nodiscard]] T& get() const {
        T* service = find<T>();
        VEACH_ASSERT(service != nullptr, "service '{}' is not available (not initialized yet, or already shut down)",
                     typeid(T).name());
        return *service;
    }

private:
    struct Entry {
        void* ptr = nullptr;
        std::string_view type_name;
    };

    template <class T>
    static const void* key() {
        static const char tag = 0;
        return &tag;
    }

    std::unordered_map<const void*, Entry> entries_;
};

/// Attaches (or with nullptr, detaches) the registry to a world.
void attach_services(World& world, Services* services);
/// The registry attached to `world`. Panics if none is attached.
[[nodiscard]] Services& services(const World& world);

}  // namespace veach
