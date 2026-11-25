#pragma once

// The lab's own module: one C++ component and one system, written with veach::
// names only. Apps never name Flecs directly (CI enforces it).

#include <veach/veach.hpp>

namespace lab {

/// Turns at a constant rate. Reflected and Scriptable, so tools and Luau see it.
struct Spinner {
    float radians_per_second = 1.0f;
    float angle = 0.0f;
};

/// Flies the entity's Transform with the project's move/turn/look actions (W/A/S/D, Q/E, the arrows, shift to go
/// faster). It writes the Transform only while a key is held, so a scene keeps its exact starting view.
struct FreeCamera {
    float speed = 2.0f;       ///< meters per second
    float turn_speed = 1.5f;  ///< radians per second
};

struct LabModule {
    explicit LabModule(veach::World& world);
};

}  // namespace lab
