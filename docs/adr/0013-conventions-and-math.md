# 0013: Conventions and math

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0010).
Positions amended by [0045](0045-large-world-positions-and-reference-frames.md): doubles in reference frames; float32
only on the GPU.

## Context

Coordinate-system and unit mismatches are among the most common engine bugs, and agents get them wrong often.
Imported assets should never need conversion. Guide: [conventions](../guides/conventions.md).

## Decision

**Match glTF:**

- right-handed: **+Y up**, **+X right**, cameras and "forward" look down **−Z**;
- **meters, seconds, radians**;
- **column-major** matrices with **column vectors** (`v' = M * v`), glm's and glTF's layout;
- **reverse-Z** depth: near maps to 1, far (infinity) to 0, clip depth in [0, 1];
- a fixed **60 Hz** simulation step (`veach::fixed_delta_seconds`, `veach/core/time.hpp`, [0018](0018-engine-lifecycle-and-phases.md)).

**Math library:** glm 1.0.3, exposed as `veach::math` (`veach/core/math.hpp`): `using namespace glm;` plus helpers:

- `world_up` (0, 1, 0), `world_right` (1, 0, 0), `world_forward` (0, 0, −1);
- `perspective_reverse_z(fov_y, aspect, near)`: right-handed, reverse-Z, infinite far. Use it instead of
  `glm::perspective`, which is not reverse-Z.

**Global defines** on `veach_core` (PUBLIC, so every TU agrees): `GLM_FORCE_DEPTH_ZERO_TO_ONE`, `GLM_FORCE_RADIANS`,
`GLM_FORCE_XYZW_ONLY` (makes `x`, `y`, `z`, `w` plain members, so member-pointer reflection works).

**Reflection:** `vec2`, `vec3`, `vec4` and `quat` are reflected structs.

**Luau:** the native `vector` maps to `veach::math::vec3` (32-bit float, 3-wide). JSON writes `vec3` as `[x, y, z]`.

These rules are restated in `AGENTS.md` because they are easy to get wrong.

## Consequences

- glTF data loads without axis or unit conversion.
- Reverse-Z with an infinite far plane gives good depth precision at outdoor distances. It does not solve
  planet-scale positions: 32-bit float world coordinates lose precision far from the origin, and how large-world
  coordinates flow through the engine is a research-phase question.
- Hand-built projections must use the helper; `tests/core_tests.cpp` pins near → 1, far → 0, monotonic.
- Including glm in a TU that bypasses `veach_core`'s defines is a bug.

## Alternatives considered

- **Left-handed / +Z forward (D3D, Unity, Unreal)** or **Z-up (Blender, Unreal):** conversion at glTF import.
- **Our own math library or DirectXMath:** more code, or Windows-centric.
- **Standard Z (near → 0):** worse depth precision.
