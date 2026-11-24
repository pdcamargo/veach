# 0010: Conventions and math

Date: 2026-11-24

## Status

Accepted

## Context

Coordinate-system and unit mismatches are among the most common bugs in engine code, and agents get them wrong often. Imported assets should never need conversion. This ADR was written before any math or rendering code beyond the helpers below existed.

## Decision

**Match glTF:**

- right-handed coordinates: **+Y up**, **+X right**, cameras and "forward" look down **−Z**;
- units are **meters, seconds, radians**;
- matrices are **column-major** with **column vectors** (`v' = M * v`), which is glm's and glTF's layout;
- **reverse-Z** depth: the near plane maps to 1, far (infinity) maps to 0, and clip depth is in [0, 1];
- a fixed **60 Hz** simulation timestep (`veach::fixed_delta_seconds`, `veach/core/time.hpp`). See [0014](0014-engine-lifecycle-and-phases.md).

**Math library:** glm 1.0.3, exposed as `veach::math` (`veach/core/math.hpp`). It is a namespace with `using namespace glm;` plus our helpers:

- constants `world_up` (0, 1, 0), `world_right` (1, 0, 0) and `world_forward` (0, 0, −1);
- `perspective_reverse_z(fov_y, aspect, near)`: right-handed, reverse-Z, infinite far.

Use it instead of `glm::perspective`, which is not reverse-Z.

**Global defines** on `veach_core` (PUBLIC, so every TU agrees): `GLM_FORCE_DEPTH_ZERO_TO_ONE`, `GLM_FORCE_RADIANS` and `GLM_FORCE_XYZW_ONLY`. The last makes `x`, `y`, `z`, `w` plain members, so member-pointer reflection works.

**Reflection:** `vec2`, `vec3`, `vec4` and `quat` are reflected structs.

**Luau:** the native `vector` type maps to `veach::math::vec3` (32-bit float, 3-wide, Luau's default `LUA_VECTOR_SIZE`). JSON writes `vec3` as `[x, y, z]`.

These conventions are restated in `AGENTS.md` because they are easy to get wrong.

## Consequences

- glTF data loads without axis or unit conversion (fastgltf is in the backlog).
- Reverse-Z with an infinite far plane gives good depth precision for large outdoor scenes.
- Code that hand-builds projections must use the helper. A unit test (`tests/core_tests.cpp`) pins the mapping: near → 1, far → 0, monotonic.
- Every engine and user TU sees glm through `veach_core`'s defines. Including glm without them, in a TU that bypasses `veach_core`, is a bug.

## Alternatives considered

- **Left-handed / +Z forward (D3D, Unity, Unreal).** Forces conversion at glTF import and contradicts the primary interchange format.
- **Z-up (Blender, Unreal).** Same conversion problem.
- **Own math library or DirectXMath.** More code, or Windows-centric. glm is what every model and contributor already knows.
- **Standard Z (near → 0).** Worse floating-point precision distribution.
