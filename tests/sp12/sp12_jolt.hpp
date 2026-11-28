// Spike SP-12 (VCH-088), throwaway: the Jolt side of a cooked collider, for the .cpp files that link Jolt (SP-12's
// physics scene and the seams sweep's walker, tests/seams). Include after <Jolt/Jolt.h>.
#pragma once

#include "sp12/sp12_physics.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/Shape/Shape.h>

namespace veach::sp12 {

struct CookedShape {
    JPH::RefConst<JPH::Shape> shape;
};

}  // namespace veach::sp12
