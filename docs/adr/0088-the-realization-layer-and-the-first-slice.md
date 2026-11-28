# 0088: The realization layer and the first slice: an atmosphere stage after B1, a cruise drive that hands over at a brakeable speed, two collider fixes, the slice as its own app

Date: 2026-11-28

## Status

Accepted. Amends [0051](0051-staged-generation-pipeline.md) (where the atmosphere is generated),
[0059](0059-gravity-flight-travel.md) (the cruise drive's handover) and [0053](0053-terrain-density-field.md) /
[0086](0086-amendments-from-building-voxel.md) (how LOD0 colliders classify). Implements
[0049](0049-scale-model.md)'s realization layer and [0080](0080-amendments-from-sp-13.md)'s handoff bands. Built in
VCH-115 ([spec](../specs/first-slice/spec.md), [findings](../research/spikes/sp-15-first-slice.md)), which closes
VCH-091.

## Context

VCH-115 assembled frames, procgen, physics, voxel terrain, SP-04's orbits, SP-05's stages, SP-10's atmosphere and
SP-14's exposure into a realization layer in `space/` and an app that flies from orbit to a dug hole at s = 0.01 and
s = 1. Every piece existed; joining them left five things the ADRs did not decide and showed two engine costs that only
a whole flight exposes.

## Decision

1. **Realization is one function and one module.** `veach::space::realize(UniverseConfig)` runs G0, S1, B1 and the
   atmosphere stage on the engine's stage runtime with no scale input, then applies s_p, s_o and s_a and builds the
   orbit tree with gameplay GMs (g R²); nothing upstream sees a scale. `UniverseModule` turns it into frames: one
   inertial system frame, per body an inertial center frame on rails and a rotating, radial-up body-fixed child,
   all written in PostSimulation for the end of the step from the int64 tick (0085 rules 4 and 5). The clock
   advances one tick per step while physics runs: rails warp with live physics scenes is not supported yet.
2. **The atmosphere's composition is its own stage, `space:body.atmosphere` v1, after B1.** 0051 lists atmosphere
   under B1, but B1 is frozen at version 1 with SP-05's committed hashes; a new stage that reads B1 adds air without
   changing any existing record. Its rules are a small classifier (temperate N2/O2 on potentially habitable bodies,
   N2/CH4 haze on cold ones that keep methane, CO2 otherwise, none without retention or a surface) to be replaced by
   the climate research's stage; the render record comes from it through `atmosphere_from` and 0049's s_a rule.
3. **The cruise drive hands over at a speed the thrusters can brake from.** 0059's cruise is kinematic and "off
   inside atmospheres", which leaves the handover speed open: at s = 1 the cruise cap d / 7 s is 14 km/s at the
   atmosphere top, which a 40 m/s² assist cannot stop in 100 km. The cruise limit is
   `min(max(v_floor, d / τ_c), √(2 a h_top) + (h − h_top) / τ_c)` with a = 20 m/s² (`CraftParams::entry_braking`):
   it falls to the entry speed √(2 a h_top) at the top (632 m/s at s = 0.01, 1,991 m/s at s = 1) and stays under
   the governor everywhere. Below the top the assisted Newtonian model flies under the cap `max(v_floor, d / 5 s)`.
   Both work on the surface-relative velocity in the body-fixed frame, with horizontal axes from the craft's
   parallel-transported heading.
4. **The surface scene caps entry speed at 150 m/s** (0080's consequence, until continuous collision detection):
   a faster craft is slowed to it at the 500 m band and the slice logs a warning. Under the governor the slice
   enters at 112 m/s at both scales.
5. **LOD0 colliders classify shallow and once, and reuse rings.** `TerrainBody::update_colliders` classified every
   needed LOD0 chunk with the full 6-level split on every call and welded every collider's ring again. Near the
   surface the bound proves nothing (the meter band's octaves count by their range), so each chunk cost the full
   8⁶-node split: a Debug slice stalled for minutes at 50 m. Collider chunks now use
   `DensitySource::classify_shallow` (one split at most, `ProgramDensity`), cached per chunk while needed (edits still
   decide live), and a ring is rebuilt only when its chunk's or a neighbor's mesh hash changed. The installed
   colliders' shapes are unchanged (SP-12's and the seams sweep's committed hashes still pass); a ring's hash no longer
   absorbs neighbors without triangles, so `[voxel.AC8]`'s committed ring hash changed once.
6. **`PhysicsModule`'s step runs with no `PhysicsBody` entity, and small scenes step on the calling thread.** Flecs
   never runs a system whose own query matches nothing, so bodies created through `physics_world(world)` (the craft,
   the pilot) never stepped; the step system now has no query terms and writes `Transform`s through a separate query.
   A scene under 256 Jolt bodies (colliders included) now steps without the job system: the slice's one-craft scene
   spent most of its step waking workers, and SP-11 measured no gain from workers below about 200 bodies. Results are
   bit-identical either way (the committed physics hashes are unchanged).
7. **The slice is its own app, `apps/slice` (`veach-slice`), not a lab mode.** It is the first program shaped like a
   game on `veach::veach` plus `veach::space`: its own project, input bindings and scene shaders, a module the tests
   drive through the real input path. The lab keeps its spikes.
8. **Configuration is the `Universe` component, set by the startup script.** `scripts/slice_s0.01.luau`,
   `slice.luau` (s = 0.1) and `slice_s1.luau` differ only in `scale`. No new CLI flag: the genesis file of 0055
   (`universe.json`) replaces the component's source when persistence lands.
9. **Toolbox error codes share the engine's catalog** (`universe.settings_invalid`, `universe.system_invalid`). The
   catalog is data the engine never interprets; 0042 forbids code dependencies on `space/`, and none was added.

## Consequences

- The same seed gives the same stage hashes at every scale (committed in `[slice.AC1]`) and a different, canonical
  realized geometry per scale (also committed).
- A whole flight from orbit, landing and dig replays bit for bit through `--record`/`--replay` and is committed per
  scale; Windows CI checks it, so the slice's cross-platform claim covers the voxel colliders, Jolt and the
  realization together.
- The landing site differs between scales (the relief scales with √s_p and the meter band does not, so "the flattest
  candidate" moves): the same planet, not the same place.
- Descent from one radius up takes 75 s at s = 0.01 and 168 s at s = 1, bound by the thrusters' authority in the
  atmosphere, not by the governor's 0.5 × τ ln(R / 100 m) estimate.

## Alternatives considered

- **Atmosphere in B1 version 2:** changes every committed B1 hash for a field no stage reads yet.
- **A cruise drive that drops out at d / τ_c:** at s = 1 it enters the air at 14 km/s; the assist crashes the craft.
- **Keeping deep classification for colliders with a cache only:** the first visit to each chunk still costs the
  full split, seconds per chunk in Debug.
- **A lab mode (`VEACH_SLICE=s`):** env-var-driven spikes are the lab's pattern; a game needs its own project and
  bindings, and `--record` of the lab would replay its fly camera too.
- **A `--config` flag or a `settings` object in `veach.project.json`:** engine surface for a toolbox notion that
  0055's genesis file will own.
