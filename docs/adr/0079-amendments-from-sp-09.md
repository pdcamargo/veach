# 0079: Amendments from SP-09: companded samples, measured streaming rates, a visual-only speed floor

Date: 2026-11-27

## Status

Accepted. Amends [0053](0053-terrain-density-field.md) (sample encoding, rate contract) and
[0059](0059-gravity-flight-travel.md) (streaming estimates under the travel governor). Validated by SP-09 (VCH-085,
[findings](../research/spikes/sp-09-descent.md)).

## Context

SP-09 streamed a body-local octree (16 m to 4,194 km chunks, levels 0 to 18) during scripted descents under the
travel governor, at s = 1 and s = 0.01 and onto a 1 km asteroid. Every checkpoint had zero open edges, up to 5.3 M
edges. At equal slopes the demand is flat across altitude and scale: s = 1 streams 52 to 65 chunks/s from 10,000 km
down to 1 km, and s = 0.01 with relief exponent 1 stays within 10 % of it. Two things did not hold. 0053's linear
int16 samples (1/1024 m, clamped to ±32 m) saturate in chunks whose cells exceed 32 m, so crossings fall at mid-edge
and the planet looks terraced from orbit. And the measured rates are 3 to 4 times 0053's and 0059's estimates: about
60 chunks/s in the high band (estimated 19) and 450 to 520 at 10 m above ground at 100 m/s (estimated 150), peaks
660; about 40 % of jobs are remeshes after a neighbor changes level. This generator does 129 / 457 / 1,678 chunks/s on
1 / 4 / 17 threads of the M5 Max.

## Decision

1. **Samples are companded int16**: linear at 1/1024 m within ±16 m, logarithmic beyond (about 0.1 % steps out to
   10,000 km), still a pure function of position ([0074](0074-amendments-from-sp-08.md)). The mesher decodes to meters
   before interpolating and differencing. Codes within ±16 m are unchanged, so SP-08's hashes and SP-12's integer
   brushes hold; a brush converts with `encode_distance` only where it writes distances beyond 16 m. This is the save
   format's encoding from the first saved universe.
2. **The rate contract uses the measured constants**: N ≈ 60 chunks/s above about 64 m, N ≈ 2.5·v at 100 m and ≈ 5·v
   at 10 m above ground. `TerrainStreamingBudget` is set from chunks/s measured per hardware tier, not from a formula.
3. **v_floor stays 100 m/s and is a gameplay constant.** A machine below the near-ground budget draws coarser terrain
   near the ground (0053's lowered effective λ); it never slows the ship or changes the simulation. The only hard
   deadline is colliders within 32 m of a dynamic body, installed at t + 2 ([0077](0077-amendments-from-sp-12.md)), and
   that is about 60 LOD0 chunks/s at 100 m/s (estimated from the swept band, not measured).
4. **Before reopening 0053's CPU generation**, two measured levers come first: SP-06's batched field-graph interpreter
   (about 1.6× here) and cached samples for remeshes (about 1.3×). GPU meshing of the far field is not needed yet.
   SP-15 measures the near-ground rate on the M3 Pro design target ([0066](0066-owner-decisions-research-phase.md)).
5. **Relief exponent 0 warns creators**: it multiplies near-ground streaming by 2.5 to 5 at small scale and produces
   slopes beyond vertical. The default stays 0.5, which costs 15 to 20 % more than exponent 1 at s = 0.01.

## Consequences

- Planets read as smooth relief from orbit instead of terraces; the encoding is fixed before any save exists.
- Fast low flight looks coarser on slower machines instead of stalling or changing what players can do.
- The 0053 estimate of B ≈ 300 chunks/s on an M1 is not reached by this generator (about 200 projected, unmeasured);
  that machine is not the design target, and the M3 Pro number decides.

## Alternatives considered

- **A per-level scale on linear samples:** also fixes the terraces, but a sample's code would depend on the level
  asking for it, which breaks 0074's rule 1.
- **Lower v_floor to about 40 m/s below 50 m:** sized for the projected M1 budget; it changes gameplay to fit a machine
  that is not the target.
- **GPU sampling for the far field now:** a large change before the cheaper CPU levers are measured.
