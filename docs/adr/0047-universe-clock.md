# 0047: The universe clock: int64 ticks, exact orbital phase, time-warp policy

Date: 2026-11-27

## Status

Accepted.
Its phase formula is superseded by [0069](0069-amendments-from-sp-04.md) (it overflowed above 1.13-year periods); pausing is allowed.

## Context

`veach::Time` counts process steps: `frame` and `fixed_step` as `uint64`, and `elapsed = fixed_step × 1/60` as a
double ([0018](0018-engine-lifecycle-and-phases.md)). There is no universe time separate from "steps this process has
run": no epoch, no warp, nothing to save. The toolbox needs a clock that every orbit, spin, season, sim-LOD catch-up
and save reads, that survives save and load, that can jump forward, and that gives the same orbital phase on every
platform after any number of years.

Double seconds lose phase slowly (a 90-minute orbit drifts 1.8 mm in 100 years, an Earth-speed orbit 1.8 m in 10,000
years) and resonances drift by ulps. The clock's representation goes into every save, recording, packet and script,
so it is expensive to change later. Research: [02-celestial-mechanics-time](../research/02-celestial-mechanics-time.md)
§2.3, §2.4, §4.1, §4.5.

## Decision

**The clock.** `space::UniverseClock` is a reflected, Scriptable singleton in `space/` (record/replay hashes it):

- `tick` (`std::int64_t`): universe ticks since the universe epoch, at the fixed step (60 Hz, 1 tick = 1/60 s).
- `rate` (`std::int64_t`, ≥ 1): ticks advanced per fixed step; 1 normally, W under rails warp. A rate below 1
  (server slowdown) is left for later through fractional accumulation; nothing reads wall-clock time, so it stays
  possible.
- The tick is **bounded below 2^53** (4.76 million years). Advancing past it is an error, so Luau carries the tick as
  an exact number ([0046](0046-64-bit-values-across-luau-json-mcp.md)). JSON and MCP carry it as a decimal string.
- `Time.fixed_step` keeps counting process steps. The two differ as soon as a save is loaded or warp is used. Celestial
  and sim-LOD code reads `UniverseClock.tick`, never `Time`.
- Seconds are derived, `tick / 60.0`, never accumulated from `delta`.

**Exact orbital phase.** Every period is stored as `period_q = round(P_seconds × 60 × 2^16)`, an `int64` in units of
2^-16 tick (0.25 µs). Phase in [0, 1) is

```text
phase = double(((Δtick mod period_q) × 2^16) mod period_q) / double(period_q)
```

with `Δtick = tick − epoch_tick` and a non-negative `mod`. It is integer arithmetic plus one IEEE division, so it is
identical on every platform ([0043](0043-determinism-tiers-and-strict-floating-point.md)), and periods in integer
ratios stay locked forever. It needs `period_q < 2^47` (periods under about 74,000 years) so the product fits in
int64. Longer periods (wide binaries, Oort-cloud comets) use `Δtick / 60.0 / P` in double, which is deterministic but
not exact; the generator marks them. Angles are `2π × phase` through `veach::dmath`.

**Time warp.**

| Mode | What it does | Where |
|---|---|---|
| Physics warp, ≤ 4× | Runs up to 4 fixed steps per frame, inside the engine's 8-step cap. Fully simulated, deterministic | Single player |
| Rails warp, ≤ 100,000× | `rate = W`. Implemented as repeated skips: stepped systems are suspended and catch up from the tick delta on exit ([0057](0057-simulation-lod.md)) | Single player, only while every player-controlled body is landed and at rest, docked, or coasting ([0059](0059-gravity-flight-travel.md)) |
| Skip | `skip_to(tick)`: the primitive, with mandatory catch-up | Single player, tests (MCP jumps to a solstice), and the multiplayer consensus skip |

A stepped system that can't catch up from a tick delta blocks rails warp and says so in the log. Terrain streaming is
unaffected: the player is in the body-fixed frame, where the ground does not move.

**Multiplayer.** The server owns the clock ([0056](0056-server-authoritative-multiplayer.md)): one tick and one rate
on the wire, clients extrapolate. There is no free warp. An optional consensus skip ("everyone sleeps") is an event
carrying its target tick, so clients predict across it instead of seeing a state jump. Subspaces (one timeline per
player) are rejected.

**Persistence.** The save stores the tick and the rate (`clock.json`, [0055](0055-universe-identity-and-versioning.md)),
never body positions. The handshake carries the tick.

**Location.** The clock is toolbox (`space/`), because its only consumers today are celestial mechanics and sim LOD.
A monotonic, savable, warpable simulation clock is generic; it moves into the engine
([0042](0042-space-toolbox-layering.md)) when a second, non-space consumer appears.

## Consequences

- Phase never degrades: a server at year 300 computes the same moon position as one at year 0, on both platforms.
- Saves, packets and recordings carry one integer for all of celestial state.
- Rails warp forces every stepped system to define catch-up; that is sim LOD's founding constraint.
- The tick rate is locked to 60 Hz. Changing it later rescales every stored time.
- The 2^53 bound caps a universe at 4.76 million years of game time, far beyond any session or server.

**Validated by** VCH-080 (orbit hash equal on the macOS gate and Windows CI at ticks {0, 1, 10^6, 10^9, 2^46,
2^53 − 1}) and VCH-093 (live vs caught-up hashes equal; a year of 6,144 sim cells caught up in under 1 ms, which is
what makes rails warp cheap). **Reopen if** the product wants time warp in multiplayer (subspaces or per-player
rates), or VCH-093 shows catch-up can't be made equivalent for a system the toolbox needs (then rails warp narrows to
the systems that can).

## Alternatives considered

- **Double seconds:** precise enough for centuries, but not exact; "works for centuries" doesn't belong in a save
  format, and cross-platform `fmod` of a rounded double invites doubt.
- **int64 ticks without exact phase** (phase from double seconds): loses the integer-ratio lock between resonant
  orbits.
- **Put the clock in `veach::Time` now:** generic in principle, but nothing outside the toolbox needs it yet.
- **Subspaces for multiplayer warp** (KSP mods): split one world into timelines, which fights persistence, edits and
  interest management.
