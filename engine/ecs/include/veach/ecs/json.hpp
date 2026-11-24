#pragma once

// Reflection-driven JSON (ADR 0007). Data files (project config, .meta files,
// materials later) are read into reflected C++ types with actionable errors, and
// their JSON Schemas are generated from the same reflection data.

#include <veach/core/error.hpp>
#include <veach/ecs/flecs.hpp>

#include <nlohmann/json_fwd.hpp>

#include <string_view>

namespace veach {

/// Assigns `json` onto the value at `ptr` (which already holds defaults).
/// Missing fields keep their defaults; unknown fields and type mismatches are
/// errors that name the file (`context`), the field path and a suggestion.
[[nodiscard]] Result<> from_json(const World& world, ecs::entity_t type, void* ptr, const nlohmann::json& json,
                                 std::string_view context);

[[nodiscard]] nlohmann::json to_json(const World& world, ecs::entity_t type, const void* ptr);

/// JSON Schema (draft 2020-12) for a reflected type. `defaults` (optional)
/// supplies "default" values for documentation.
[[nodiscard]] nlohmann::json json_schema(const World& world, ecs::entity_t type, const void* defaults = nullptr);

template <class T>
[[nodiscard]] Result<> from_json(const World& world, T& value, const nlohmann::json& json, std::string_view context) {
    return from_json(world, world.id<T>(), &value, json, context);
}

}  // namespace veach
