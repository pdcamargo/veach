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

struct LabModule {
    explicit LabModule(veach::World& world);
};

}  // namespace lab
