#pragma once

// Generates veach.d.luau — the type definitions for `require("@veach")` —
// from Flecs reflection. Every component tagged veach::Scriptable becomes an
// exported Luau type and an entry in `veach.components`. Run `veach types`.

#include <veach/ecs/flecs.hpp>

#include <string>

namespace veach {

[[nodiscard]] std::string generate_luau_types(const World& world);

}  // namespace veach
