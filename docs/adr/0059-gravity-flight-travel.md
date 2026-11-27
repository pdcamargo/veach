# 0059: Gravity, flight and travel: sphere-of-influence gravity, flight-model plug-ins, the travel governor

Date: 2026-11-27

## Status

Accepted.

## Context

The first slice flies from orbit to the ground without loading and lands, at s = 0.01 and 1 with the same code. "Up" is
a constant today ([0013](0013-conventions-and-math.md)); there is no gravity, flight model or analog input. With surface
gravity fixed ([0049](0049-scale-model.md)), distances grow 100× across the scale range, so a drive tuned for one end
fails at the other. The tick never pauses, so speed near a surface is bounded by streaming and collider cooking.
Research: [10-gravity-flight-travel](../research/10-gravity-flight-travel.md), [04-terrain](../research/04-terrain.md).

Realism versus gameplay (default flight model, entry heating, FTL between systems, no time warp in multiplayer) is a
product decision pending the owner's review packet; the working defaults below are the dev lead's.

## Decision

**Gravity.** One registry of gravity sources, queried per body and per tick:

- **Natural:** the dominant body by sphere of influence, as in the on-rails model ([0048](0048-bodies-on-rails.md)),
  in that body's frame; inverse-square outside, linear inside; μ = g·R² with g = g_physical(s = 1) × creator
  multiplier. Other bodies contribute nothing. Near small irregular bodies the direction follows the coarse density
  gradient, with the gravity floor of [0058](0058-physics-jolt.md).
- **Artificial:** volumes (decks, bases) that override natural gravity by priority, blended over about 0.5 m.

**Characters.** up = −ĝ, approached by the minimal rotation (time constant about 0.15 s); below 0.05 m/s², no
alignment. Heading is parallel-transported, never derived from north. The camera's up is smoothed separately. Walking
is in the body-fixed frame ([0045](0045-large-world-positions-and-reference-frames.md)).

**Flight models are plug-ins.** One `FlightModel` seam, called every fixed step: inputs are control intent from analog
actions, ship mass and thrusters, gravity, air density, wind and the reference frame; outputs are force and torque in
the local frame. The core ships assisted Newtonian (stick input becomes target velocity within thruster authority, hover
counters gravity, assist-off is plain Newtonian); orbital and arcade models are modules. Analog axes (VCH-101) are an
engine prerequisite.

**The travel governor.** One cap law for every travel tier (local flight, in-system cruise):

    |v| ≤ max(v_floor, d / τ)

with v relative to the dominant body's surface frame, d the distance to the nearest significant surface. Starting
values: **τ = 5 s near bodies** (descent and atmospheric flight), **τ = 7 s in cruise**, **v_floor = 100 m/s**, which
takes over below d = 500 m. Everything is in meters and identical at every scale, so travel time from d₀ to d₁ is
τ·ln(d₀/d₁) whatever s is. Cruise is a kinematic drive, off inside atmospheres, dropping out on obstruction. Between
systems the only travel is an FTL jump behind a transition of fixed length, the only place generation may be hidden
(working default). No time warp in multiplayer ([0047](0047-universe-clock.md)).

**The approach-speed budget** (estimates, reconciled to the terrain ADR's constants:
16 m LOD0 chunks, refinement ratio λ = 4, a 1.5-chunk shell, [0053](0053-terrain-density-field.md)):

- Above about 64 m, N ≈ 96·v/h chunks/s. Under the governor v = h/τ, so **N ≈ 96/τ ≈ 19 chunks/s at τ = 5 s,
  independent of altitude and s** (the flight research's placeholder constants gave 156/τ ≈ 31).
- Near the ground N ≈ 1.5·v: **about 150 chunks/s at v_floor = 100 m/s**. The terrain research estimates a CPU budget
  B ≈ 300 chunks/s on an M1-class machine and about 600 on M3 Pro-class, so v_floor ≤ B/1.5 allows up to 200 m/s on
  the M1 class. 100 m/s keeps half of that for edits and collider cooking.
- The constants are simulation: the same for every preset ([0064](0064-quality-presets.md)), sized for the slowest
  machine. A machine that falls behind coarsens render LOD or stalls on a collider install.

| *(computed)* | s = 0.01 | s = 0.1 | s = 1 |
|---|---|---|---|
| Radius (Earth-like) | 63.7 km | 637 km | 6,371 km |
| Low-orbit speed √(gR) | 791 m/s | 2.50 km/s | 7.91 km/s |
| Atmosphere top, 100 km·√s ([0049](0049-scale-model.md)) | 10 km | 31.6 km | 100 km |
| Cap at the atmosphere top, d/τ | 2.0 km/s | 6.3 km/s | 20 km/s |
| Cap at 10 km / 1 km / 100 m | 2,000 / 200 / 100 m/s | same | same |
| Descent from h = R to 100 m, τ = 5 s | 32 s | 44 s | 55 s |
| Moon-distance hop (384,400 km × s), h = R to h = R, τ = 7 s | 48 s | 48 s | 48 s |
| 1 AU × s hop, h = R to h = R, τ = 7 s | 131 s | 131 s | 131 s |

**Entry.** Drag ½ρv²C_dA from the shared atmosphere record ([0063](0063-astronomical-rendering.md)); heating and its
effects are driven by v relative to the local orbital speed, mild and not destructive by default (working default).

**Placement.** Gravity sources, flight models and the governor are `space/`
([0065](0065-engine-areas-for-the-toolbox.md)).

## Consequences

- Streaming load is scale-invariant above the floor, so one terrain budget serves every s.
- Changing τ or the cap law later changes travel times, traffic timing and the chunk budget everywhere.
- **Validation:** VCH-085 (SP-09): a descent under the governor at s = 0.01 and 1 and onto a 1 km asteroid with no
  holes, and peak chunks/s within 20% across scales and within budget; its measurements replace τ and v_floor.
  VCH-089 (SP-13): hover and landing on a rotating planet with matching replay hashes.
- **Reopen if** VCH-085 makes a 300 m/s floor free, playtests find the approach's end slow (shorter τ in the last
  band), or the owner picks orbital or arcade flight as the default (the seam stays).

## Alternatives considered

- **All bodies summed.** Physics would leave the on-rails trajectory, so going on and off rails would jump.
- **Fixed speeds per tier.** One minute at s = 0.01 becomes 100 at s = 1.
- **Speed capped by gravity strength.** Ties travel time to a creator gravity setting.
- **Artificial gravity summed with natural.** Tilted floors in a ship parked on a slope.
