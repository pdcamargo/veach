#pragma once

// The Luau `Position` userdata (ADR 0046, docs/specs/frames/spec.md): three doubles, a point relative to a reference
// frame. `Position + vector -> Position`, `Position - vector -> Position`, `Position - Position -> vector` (the
// difference in double, then float32), `==`, `tostring`, and the fields x, y, z. No Position + Position, no scaling.
// Private to engine/script.

#include <veach/core/math.hpp>

#include <lua.h>

namespace veach::script {

/// Luau userdata tag for Position values (24-byte payload: x, y, z as doubles). Entity is tag 1.
inline constexpr int position_tag = 2;

/// Installs the Position metatable in the state (once, before any Position is pushed).
void open_position(lua_State* L);
void push_position(lua_State* L, const math::dvec3& p);
/// The Position at `index`, or null if the value is not one.
[[nodiscard]] const math::dvec3* to_position(lua_State* L, int index);
/// The Position at `index`; raises script.invalid_value naming `what` otherwise.
[[nodiscard]] math::dvec3 check_position(lua_State* L, int index, const char* what);

/// veach.position(x, y, z) and veach.position(v: vector).
int api_position(lua_State* L);

}  // namespace veach::script
