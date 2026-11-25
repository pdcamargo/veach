#pragma once

// Luau <-> component memory, driven by cached per-type plans. Built once per
// reflected type from the reflection walker, so the per-row cost is a switch
// on a precomputed kind and offset (no reflection queries, no allocations
// besides the Luau values themselves). Private to engine/script.

#include <veach/core/error.hpp>
#include <veach/ecs/flecs.hpp>
#include <veach/ecs/reflection.hpp>

#include <lua.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace veach::script {

struct TypePlan;

struct FieldPlan {
    std::string name;
    std::int32_t offset = 0;
    const TypePlan* type = nullptr;
};

/// Luau userdata tag for Entity values (8-byte payload: the entity id).
inline constexpr int entity_tag = 1;

/// Pushes an Entity userdata, or nil for 0 / dead entities.
void push_entity(lua_State* L, const World& world, ecs::entity_t id);
/// The entity id of the Entity userdata at `index`, or 0 if it is not one.
[[nodiscard]] ecs::entity_t to_entity(lua_State* L, int index);

struct TypePlan {
    ecs::entity_t id = 0;
    ValueKind kind = ValueKind::unsupported;
    ecs_primitive_kind_t primitive{};                             ///< integer/number/enumeration storage
    std::vector<FieldPlan> fields;                                ///< structure
    std::vector<std::pair<std::int64_t, std::string>> constants;  ///< enumeration (value, name)
    std::string name;                                             ///< for messages
    const TypePlan* inner = nullptr;                              ///< optional: the wrapped value type
    std::int32_t present_offset = 0;                              ///< optional
    std::int32_t value_offset = 0;                                ///< optional
};

/// A field path built lazily, only when an error message needs it.
struct Path {
    const Path* parent = nullptr;
    std::string_view segment;
    [[nodiscard]] std::string str() const;
};

class Marshal {
public:
    explicit Marshal(const World& world) : world_(world.c_ptr()) {}

    [[nodiscard]] const TypePlan& plan(ecs::entity_t type);

    /// Pushes the value at `ptr` onto the Luau stack.
    void push(lua_State* L, const TypePlan& plan, const void* ptr) const;

    /// Writes the Luau value at `index` into `ptr`. Raises a Luau error with
    /// script.invalid_value / script.unsupported_field on mismatch.
    void read(lua_State* L, int index, const TypePlan& plan, void* ptr, const Path& path) const;

private:
    World world_;
    std::unordered_map<ecs::entity_t, std::unique_ptr<TypePlan>> plans_;
};

/// Raises a Luau error carrying a Veach error code. Never returns.
[[noreturn]] void raise(lua_State* L, ErrorCode code, const std::string& message, const std::string& hint = {});

}  // namespace veach::script
