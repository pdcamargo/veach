# Transforms and hierarchy

**Status:** Implemented
**Area:** ecs
**Owner:** dev lead

## Summary

`Transform` is an entity's pose relative to its parent. `GlobalTransform` is its world pose, computed by
TransformPropagation, parents before children. Scripts build hierarchies with `veach.spawn{ parent = e }`, walk
them with `e:parent()` and `e:children()`, and compose rotations with `veach.quat`. Cameras read
`GlobalTransform`.

## Motivation

A camera on a rig, a weapon on a hand and a light on a lamp all need poses relative to a parent. VCH-011 added
`Transform` as a world pose, and this spec adds the hierarchy. Backlog: VCH-007.

## Requirements

- Every entity with `Transform` MUST also have `GlobalTransform` (Flecs `With` trait); nothing writes it but the
  propagation.
- `GlobalTransform = parent.GlobalTransform * Transform.matrix()` (ChildOf), or `Transform.matrix()` at the root.
  Parents MUST be computed before children (`cascade`), so any depth settles in one pass.
- Propagation MUST run in the `TransformPropagation` phase (per fixed step, for gameplay) and again in
  `PreExtract` (per frame), so a pose written in any phase reaches extraction in the same frame.
- `veach.spawn(name?)` and `veach.spawn({ name = ..., parent = e })`. The parent is applied with the entity when
  the invocation succeeds, and the name is scoped to the parent.
- `e:parent() -> Entity?` and `e:children() -> { Entity }` (id order) read the world now.
- `veach.quat`: `identity`, `from_axis_angle`, `from_euler(yaw, pitch, roll?)`, `mul`, `rotate`, `inverse`,
  `normalize`, `look_rotation(forward, up?)`, computed with glm, so results match C++.
- Other relationships stay hidden from scripts.

## Acceptance criteria

- **transform.AC1**: Setting `Transform` adds `GlobalTransform`. After propagation, a root's GlobalTransform
  equals its Transform matrix.
- **transform.AC2**: In a three-level hierarchy (a translated and rotated parent, a child, a scaled grandchild),
  each GlobalTransform is its parent's times its local, in one propagation.
- **transform.AC3**: A script spawns a rig and a child with `veach.spawn{ parent = rig }` and moves the rig in
  the per-frame `Input` phase. After that frame, the child's GlobalTransform already follows the rig. `parent()`
  and `children()` report the hierarchy.
- **transform.AC4**: `veach.quat` helpers: rotating −Z by `from_axis_angle(+Y, π/2)` gives −X, and so does
  `from_euler(π/2, 0)`. `look_rotation(+X)` turns −Z to +X. `mul(inverse(q), q)` is the identity. A
  non-quat argument fails with `script.invalid_value`.
- **transform.AC5**: A camera parented to a rig views from the rig's world pose.

## Out of scope

- Reparenting from Luau (`e:set_parent`), other relationship kinds, and static or dirty-flag optimizations for
  large hierarchies.

## Links

- ADRs: [0013 conventions](../../adr/0013-conventions-and-math.md), [0006 Flecs barrel](../../adr/0006-flecs-barrel-not-wrapper.md)
- Specs: [render-views](../render-views/spec.md)
- Backlog: VCH-007
