# SP-13: walk, hover and land on a rotating planet (VCH-089)

Validates the frame handoff of [ADR 0045](../../adr/0045-large-world-positions-and-reference-frames.md), the physics
scenes of [ADR 0058](../../adr/0058-physics-jolt.md) and the flight model and governor of
[ADR 0059](../../adr/0059-gravity-flight-travel.md) on an Earth-like rotating, tilted planet at s = 0.01 and s = 1.
Research: [10-gravity-flight-travel](../10-gravity-flight-travel.md) §5, [09-physics](../09-physics.md) §2.3 and §2.6.
All numbers are from an Apple M5 Max, 2026-11-27, with other worktrees building (load average 4 to 7).

**Verdict: the handoff mechanism holds.** Nothing drifts and nothing jumps:

- **Bases don't drift.** After 24 simulated hours (5,184,000 steps, one sidereal rotation) of a character walking
  circles around them, with 3,201 surface-scene re-anchors, a static base has moved 0 m at s = 0.01 and 8.5e-14 m at
  s = 1, and a sleeping crate and a character standing still 2.8e-14 to 5.7e-14 m. A crate that is never allowed to
  sleep moved 1.8e-8 m.
- **|Δv| stays under 1 mm/s through every handoff**, both directions, at both scales. The worst is 2.8e-6 m/s, when the
  craft joins the surface scene at 95 m/s and Jolt stores its velocity as float32. Inertial ↔ body-fixed is 2.7e-13
  m/s at 1,471 m/s; every re-origin of either scene is exactly 0.
- **A landing replays.** Flown through the input path (six gamepad axes on the 1/65536 grid), recorded with `--record`
  and replayed with `--replay`, the engine's per-frame state hash never diverges, and the final hash equals the one
  from flying the same controls straight into the simulation, in Debug and Release. The hashes are committed in
  `[sp13.AC1]` and `[sp13.AC3]`.

Two rules have to change for it to hold everywhere (proposed amendments below): free flight in a rotating frame needs
its Coriolis and centrifugal forces, and a co-moving scene re-centers velocity as well as position.

![The craft hovering 50 m over the pad at s = 1](sp-13/hover_s1.png)

## What was built

Throwaway, under `tests/sp13/` like SP-11, plus a lab view. Nothing in `engine/` except one input fix (below).

| Piece | Where | Notes |
|---|---|---|
| The simulation | `tests/sp13/sp13_sim.{hpp,cpp}` (`veach-sp13` library) | The only file with Jolt. Frames, scenes, transfers, terrain ring, flight model, characters, autopilot |
| Engine module | `tests/sp13/sp13_module.{hpp,cpp}` | `Sp13Run` (set by a startup script) and `Sp13Craft` (published each step), both Scriptable; six analog actions |
| Tests | `tests/sp13/sp13_tests.cpp` | `[sp13.AC1]`–`[sp13.AC3]` in `veach-tests`, the fast set: 2.2, 0.4, 3.7 and 8.7 s in Debug |
| `veach-bench-sp13` | `tests/sp13/sp13_bench.cpp` | `landing`, `freefall`, `walk`, `frames`: the tables below |
| Lab view | `apps/lab/src/sp13_spike.cpp` | `VEACH_SP13=s [VEACH_SP13_AT=landed]`: runs the Sim to a moment and draws it with runtime meshes (ADR 0072). Compiled into the lab from `tests/sp13/CMakeLists.txt` |
| Shared Jolt init | `tests/support/jolt_runtime.hpp` | One `Factory` and `RegisterTypes` per process for SP-11 and SP-13 together (ADR 0075, decision 5) |

**The planet.** Earth-like at body scale s ([ADR 0049](../../adr/0049-scale-model.md)): R = 6,371 km × s, g = 9.80665
m/s², GM = gR², a sidereal day of 86,164.09 s and 23.44° obliquity at both scales, the atmosphere top at 100 km × √s.
The body-fixed frame's orientation comes from the tick through `space::orientation` (SP-04's exact phase, ADR 0069),
never from integrating a rotation. The pad is at latitude 30°.

**Frames and scenes** (ADR 0045, 0058). Two frames: the planet's body-centered inertial frame I and its body-fixed frame
B. Two Jolt `PhysicsSystem`s, both with world gravity zero and a per-body gravity force (inverse-square, GM = gR²):

- the **surface scene** in B, anchored at a body-fixed point, with the terrain, a static base (12 × 5 × 12 m), two
  crates (one may sleep, one never does), two `CharacterVirtual`s (one walking a 200 m circle around the pad at 5 m/s,
  one standing still) and the craft when it is low. Re-anchored on the walker every 60 ticks once it is more than
  128 m from the anchor: every body and character shifts by the difference, in double;
- the **free-flight scene** for the craft, in I or B, with a double origin and a double velocity, re-centered on the
  craft (origin := its position, velocity := its velocity) whenever its local offset passes 256 m or 4 m/s.

**Handoff policy** (toolbox, ADR 0045). ADR 0059 leaves the altitude to the research, which puts it at the atmosphere
top (10 km at s = 0.01, 100 km at s = 1): free flight goes from I to B below it and back above 1.1 × it (hysteresis).
The craft joins the surface scene below 500 m and leaves it above 750 m, the band where the governor's floor rules.
Every move happens at a tick boundary: read the state from the old scene, convert pose and velocity in double
(`v_B = Oᵀ v_I − ω × r_B`), create the body in the new scene, read it back, and log the difference in the old frame.

**Terrain.** A smooth sphere, no SP-12 dependency: a lattice in the pad's tangent plane projected onto the sphere, cut
into 16 m chunks of 2 m cells. Each chunk is a static `MeshShape` body at its own body-fixed origin, its vertices
relative to that origin (float32 under 23 m), so re-anchoring moves chunk bodies without rebuilding them. Chunks are
installed within ±2 of each occupant's position 0.25 s ahead and removed beyond ±3, in key order at the tick boundary
(ADR 0058's rule with K = 0).

**Flight model** (ADR 0059's assisted Newtonian). The sticks give a target velocity relative to the surface frame
(lift × cap along local up, strafe east and north), capped by the governor `max(100 m/s, d / 5 s)`; the assist
commands `(target − v) / 0.5 s − g`, clamped to 40 m/s². The target is surface-relative in both frames, so the
assist's reference does not change at the I ↔ B handoff. The craft's rotation is locked in the solver and follows
local up with the characters' 0.15 s time constant.

**Characters** (ADR 0059). `up = −ĝ` approached by a normalized lerp (time constant 0.15 s, no trig), heading
parallel-transported by projection onto the new tangent plane, turning by `dmath` sin and cos. Jolt's
`ExtendedUpdate` takes its stick-to-floor and stair vectors in world space (default `(0, −0.5, 0)`); they must be
built from `up` each step.

**Autopilot.** A stand-in for a player with a gamepad: descend on a 10 m/s² braking curve, hover 5 s at 50 m, land
at 0.5 m/s, cut the engine; optionally climb back out at the governor's cap. It reads only the published `CraftView`
and returns quantized controls, so the same code drives the direct runs and the engine runs. A replay never runs it.

**Determinism.** Everything that feeds the simulation uses `+ − × ÷ √`, `floor`, `nearbyint` and `veach::dmath`; the
hash is `veach::hash` over raw bits of every scene origin, body, character, up and heading. Debug and Release give
the same hashes. Windows CI checks them on the next push.

## Handoffs: |Δv| and |Δx|

`veach-bench-sp13 landing --climb`, Release. The craft starts 1.15 × the atmosphere top above the pad in the inertial
free-flight scene, co-rotating with the surface plus 20 m/s east, descending at 80 % of the braking-curve speed. It
lands, waits 2 s, climbs out through the handoff altitude. Max over each kind, in the old frame; speed is |v| in the
old frame:

| Transfer | s = 0.01: count, max \|Δv\| m/s (at \|v\|) | s = 1: count, max \|Δv\| m/s (at \|v\|) |
|---|---|---|
| inertial → body-fixed, at the atmosphere top | 1, 1.1e-13 (446 m/s) | 1, 2.7e-13 (1,471 m/s) |
| free flight → surface scene, 500 m | 1, **2.8e-6** (95 m/s) | 1, **1.1e-6** (95 m/s) |
| surface scene → free flight, 750 m | 1, 0 | 1, 0 |
| body-fixed → inertial, 1.1 × the top | 1, 1.1e-13 (817 m/s) | 1, 2.3e-13 (2,579 m/s) |
| free-flight re-origin (position and velocity) | 267, 0 | 1,009, 0 |
| surface re-anchor: craft, characters, crates | 18, 0 | 41, 0 |

The largest |Δx| is 4.7e-10 m. The only error above double rounding is the float32 velocity Jolt keeps even with
`JPH_DOUBLE_PRECISION`: entering a scene that doesn't move with the body rounds |v| to float, half an ULP, 3.8e-6 m/s
at 100 m/s but 0.24 mm/s at 7.9 km/s and 0.98 mm/s at 20 km/s (the governor's cap at the atmosphere top at s = 1).
That is why the free-flight scene must re-center velocity too (amendment 2).

Landing quality is the same at both scales, as the governor's meters-and-seconds law predicts: hover between 49.09 and
49.94 m above the ground, touchdown at 0.501 m/s, 0.02 m (s = 0.01) and 0.01 m (s = 1) from the pad center, 71.6 s
and 175.8 s after the start.

## Jumps: an unpowered fall through the handoff

A conversion can be exact while the physics on the other side differs. `veach-bench-sp13 freefall`: the craft falls
from 1.15 × the atmosphere top, engine off, through the I → B handoff, next to a control that stays in inertial free
flight. Largest difference from the control over the run, inertial frame:

| | s = 0.01, 40 s | s = 1, 90 s |
|---|---|---|
| ADR 0058 as written: no Coriolis or centrifugal force in B | \|Δv\| **0.54 m/s**, \|Δx\| 4.6 m | \|Δv\| **3.1 m/s**, \|Δx\| 49 m |
| with Coriolis and centrifugal forces in body-fixed free flight | \|Δv\| 0.32 mm/s, \|Δx\| 1.5 mm | \|Δv\| 0.67 mm/s, \|Δx\| 1.6 cm |

Without the fictitious forces nothing jumps at the handoff, but the trajectory bends from then on: 2ω × v is 0.1 m/s² at
700 m/s at s = 1 and 0.02 m/s² at 150 m/s at s = 0.01. A powered craft doesn't notice (its assist works against the
surface-relative velocity and absorbs it), but a falling crate, a projectile or a ship coasting on the way down would.
With them the two runs agree to the integrator's difference. The surface scene doesn't need them: its bodies are slow,
and at walking speed Coriolis is 7e-4 m/s².

## Drift: bases, characters, re-anchors

`veach-bench-sp13 walk --hours 24`, Release, after 5 s of settling (characters start 2 cm and crates 1 cm up):

| After 24 h (1.003 rotations, 3,201 re-anchors) | s = 0.01 | s = 1 |
|---|---|---|
| Static base | **0 m** | **8.5e-14 m** |
| Character standing still | 2.8e-14 m | 5.7e-14 m |
| Crate, sleeping | 2.8e-14 m | 5.7e-14 m |
| Crate, never sleeps | 1.8e-8 m | 2.8e-14 m |
| Walker's height above the sphere | −0.11 to 10.2 mm | −0.10 to 10.2 mm |
| Walker steps without ground support | 13 of 5,184,000 | 14 of 5,184,000 |
| Walker vs a control run with no re-anchors, max \|Δx\| | 2 min: 1.4e-14 m; 24 h: 4.0 cm | 2 min: 1.1e-13 m; 24 h: 8.8 mm |

- **Nothing placed in B moves.** Its pose is constant by construction, and the only arithmetic on it is the re-anchor
  shift (one double subtraction per re-anchor, never accumulated into the stored pose's frame).
- **Re-anchoring doesn't jump.** Velocities aren't touched, positions round at 1e-14 m. `CharacterVirtual` caches its
  contacts' world positions, so it needs `RefreshContacts` after the shift. Over 24 h and 432 km of walking the
  walker's path and the control's separate by centimeters: Jolt's collision math rounds differently relative to a
  different anchor, and a long walk accumulates it. A replay re-anchors at the same ticks, so it is not a replay
  problem.
- **Frames from the tick.** `veach-bench-sp13 frames`: integrating the planet's rotation step by step in double puts
  an equatorial base at s = 1 2.4 µm off after 1 h, 0.37 mm after 16 h and about 5 mm after 10 to 30 days; in float32,
  km within the first hour. The tick phase has no such error: SP-04's residue arithmetic is exact at any tick.
- The 13-14 unsupported steps are single steps where `ExtendedUpdate` lost the floor while crossing between chunk
  bodies; the walker never left the 10 mm band. Not investigated further (follow-up).

## Replay through the input path

`[sp13.AC3]`: an engine with the SP-13 module, headless and null RHI, the startup script
`veach.spawn("sp13"):set(veach.components.Sp13Run, { scale = s, craft = true, walkers = true })`. Before each frame the
test reads `Sp13Craft`, runs the autopilot and injects six `axis_moved` events (lift, strafe east, strafe north,
engine, walk, turn on the gamepad sticks and triggers, fixed point). The module reads `veach::axis_value`, quantizes
it back and steps the Sim in Simulation. Then the recording replays headless.

| | s = 0.01 | s = 1 |
|---|---|---|
| Frames (the landing, until the engine has been off 2 s) | 4,418 | 10,665 |
| Final state hash, direct run = engine `--record` = engine `--replay` | `0x9e25df28fd4d4a02` | `0x5fff33f2b5008ada` |
| `replay.diverged` | never | never |
| 2 minutes of walking ([sp13.AC1]) | `0x16e318bdcca0b65b` | `0xd9d5140a4fbe07bd` |

The hash also covers the walker and idler, crates and terrain chunk count during the landing.

**An engine bug, fixed here.** `bind_action` and `bind_axis` created `veach::actions::<name>` relative to the current
scope, so a module that binds in its constructor (inside `world.module<T>()`'s scope) got an entity under its own
path, and `axis_value` and `action_down` read 0 forever. They now create it from the root; `[input.AC17]` covers it.

## Cost

`veach-bench-sp13`, Release, per step: transfers, terrain ring, flight model, gravity, two characters and both
`PhysicsSystem::Update`s (`JobSystemSingleThreaded`):

| Run | Mean | p50 | p95 | p99 |
|---|---|---|---|---|
| Walk, s = 0.01, 24 h (2 characters, 2 crates, base, about 65 chunks) | 0.0122 ms | 0.0113 | 0.0130 | 0.0254 |
| Walk, s = 1, 24 h | 0.0122 ms | 0.0113 | 0.0130 | 0.0256 |
| Landing and climb, s = 0.01 (adds the craft and the free-flight scene) | 0.0137 ms | 0.0126 | 0.0156 | 0.0260 |
| Landing and climb, s = 1 | 0.0129 ms | 0.0121 | 0.0138 | 0.0225 |

Debug (Jolt at `-O2`, our code at `-O0`) is about 0.08 ms. A whole scene this size is far under a frame; worker
threads would not pay off (SP-11: below about 200 bodies they don't).

## Proposed ADR amendments

1. **ADR 0058, fictitious forces.** "No Coriolis or centrifugal forces" holds for the surface scene. A co-moving
   free-flight scene in a rotating frame applies them (−2ω × v − ω × (ω × r) per body, two cross products), or its
   unpowered trajectories bend by meters after the handoff (table above).
2. **ADR 0058, co-moving scenes re-center velocity.** Jolt keeps linear velocity in float32 even with double
   positions, so a co-moving scene re-centers on its body's position *and* velocity (here past 256 m or 4 m/s), and
   its origin advances with exact tick time in double (Jolt's `1.0f / 60.0f` step is 8.7e-10 s too long, 7 µm a step
   at orbital speed if positions were absolute).
3. **ADR 0059, the handoff altitude.** State it: body-fixed below the atmosphere top (100 km × √s), inertial above
   1.1 × it; the surface scene below 500 m above the ground, free flight above 750 m. The flight assist works on the
   surface-relative velocity in both frames, so its reference doesn't change at the handoff.

An alternative to 1 that SP-13 makes possible but did not measure: keep free flight in the inertial frame down to the
surface scene (500 m), so the only I ↔ B handoff is the scene change and no scene needs fictitious forces. Wind and
drag would convert the air's velocity (ω × r) instead. It removes a frame change and a per-body term at the price of
the atmosphere's natural frame.

## Follow-ups

- **engine/physics.** Take from here: tick-boundary moves read back into a log, the double origin and velocity per
  scene, the chunk-local terrain origins, `RefreshContacts` after a re-anchor, up-relative `ExtendedUpdateSettings`.
- **Fast bodies entering the surface scene.** In the free-fall run at s = 0.01 the craft entered at 425 m/s and fell
  through the ground (discrete collision, a ring placed by a lead point rather than ADR 0058's swept region). Bodies
  above the governor's floor in a surface scene need `LinearCast` and the swept ring (SP-12 owns the ring).
- **Chunk seams.** The 13-14 unsupported walker steps per day; try `mEnhancedInternalEdgeRemoval` or one mesh per
  collider region.
- **Windows CI** runs `[sp13.AC1]`–`[sp13.AC3]` on the next push; the cross-platform claim covers `CharacterVirtual`
  for the first time.

## Reproduce

```sh
build/debug/tests/veach-tests -tc="*sp13*"                          # the committed hashes, handoffs, replay
cmake --preset release && cmake --build --preset release --target veach-bench-sp13
build/release/tests/sp13/veach-bench-sp13 landing --scale 1 --climb  # transfers, landing quality, ms/step
build/release/tests/sp13/veach-bench-sp13 freefall --scale 1 --seconds 90 [--fictitious]
build/release/tests/sp13/veach-bench-sp13 walk --scale 0.01 --hours 24
build/release/tests/sp13/veach-bench-sp13 frames --days 10
VEACH_SP13=1 [VEACH_SP13_AT=landed] build/debug/apps/lab/veach-lab --rhi metal --headless --frames 3 \
  --script scripts/hello.luau --screenshot out.png
```

Screenshots (the interim real-time renderer has no shadows, ADR 0040): [hover, s = 0.01](sp-13/hover_s0.01.png),
[hover, s = 1](sp-13/hover_s1.png), [landed, s = 0.01](sp-13/landed_s0.01.png), [landed, s = 1](sp-13/landed_s1.png).
Both scales look the same on purpose: people, craft and buildings don't scale, and the horizon is past the drawn patch.
