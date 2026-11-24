#pragma once

// Reflection helpers on top of Flecs meta (ADR 0022, "every component is
// reflected"). One walker serves JSON data files, JSON Schema generation, Luau
// marshalling and Luau type generation, so all of them agree on what a type is.
//
// Reflect a C++ component with plain Flecs idioms:
//   world.component<Spin>().member(&Spin::speed, "speed");
// The engine refuses to start if a component with data has no reflection.

#include <veach/core/error.hpp>
#include <veach/ecs/flecs.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace veach {

/// How a reflected type is represented outside C++ (JSON, Luau).
enum class ValueKind : std::uint8_t {
    boolean,      ///< bool
    integer,      ///< any signed/unsigned integer primitive
    number,       ///< f32, f64
    string,       ///< std::string
    vec3,         ///< veach::math::vec3 (Luau `vector`)
    enumeration,  ///< reflected enum, represented by its constant name
    structure,    ///< reflected struct, represented as an object/table
    unsupported,  ///< anything else (arrays, vectors, maps, opaque types, ...)
};

struct FieldInfo {
    std::string name;
    ecs::entity_t type = 0;
    std::int32_t offset = 0;
    ValueKind kind = ValueKind::unsupported;
};

/// Registers reflection for std::string (as an opaque string) and the
/// veach::math vector types. Part of CoreModule.
void register_builtin_reflection(World& world);

[[nodiscard]] ValueKind value_kind(const World& world, ecs::entity_t type);
[[nodiscard]] std::vector<FieldInfo> struct_fields(const World& world, ecs::entity_t type);
[[nodiscard]] std::vector<std::string> enum_constants(const World& world, ecs::entity_t type);
/// Short type name for messages and generated code, e.g. "Window" or "vec3".
[[nodiscard]] std::string type_name(const World& world, ecs::entity_t type);

/// Reads/writes a value whose kind is integer or number.
[[nodiscard]] double read_number(const World& world, ecs::entity_t type, const void* ptr);
void write_number(const World& world, ecs::entity_t type, void* ptr, double value);
/// Reads/writes an enumeration as its constant name.
[[nodiscard]] std::optional<std::string> read_enum(const World& world, ecs::entity_t type, const void* ptr);
[[nodiscard]] bool write_enum(const World& world, ecs::entity_t type, void* ptr, std::string_view name);

/// True if the component has reflection data (or is a tag, which needs none).
[[nodiscard]] bool is_reflected(const World& world, ecs::entity_t component);

/// Fails with ecs.unreflected_component listing every component outside the
/// flecs.* namespace that has data but no reflection.
[[nodiscard]] Result<> check_reflection(const World& world);

}  // namespace veach
