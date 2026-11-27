# 0064: Quality presets never change simulation outcomes

Date: 2026-11-27

## Status

Accepted.
Hardware target set by the owner in [0066](0066-owner-decisions-research-phase.md).

## Context

The reset deleted the old quality tiers ([0001](0001-reset.md)). They return because one feature set does not fit every
Mac: the full set is estimated at about 19 ms on a base M1-class GPU at 1080p and 12 ms on M3 Pro-class at 1440p
(unmeasured). Simulation LOD, streaming and physics have knobs that look like quality settings; if one changes what
happens, players on different hardware live in different universes and multiplayer desyncs. Research:
[08-renderer](../research/08-renderer.md), [07-simulation-lod](../research/07-simulation-lod.md),
[10-gravity-flight-travel](../research/10-gravity-flight-travel.md).

The hardware target is a product decision pending the owner's review packet. Until the owner decides,
[0003](0003-platforms-and-hardware-target.md) stands (60 fps at 1080p on a base M1-class GPU). The working default from
the dev lead: design target M3 Pro-class at 1440p60 with everything on, floor base M1-class at 1080p60 on the low
preset.

## Decision

**The rule.** A quality preset may change only visual-tier work
([0043](0043-determinism-tiers-and-strict-floating-point.md)) and how many individuals are presented near the observer.
It may never change canonical or replay-tier results: the canonical hashes and the aggregate simulation state are
identical under every preset for the same inputs.

**Presets are data.** A preset is a named JSON object with a schema generated from reflection, selected in the project
config and overridable per run. The engine ships two, `low` (M1-class at 1080p60) and `high` (M3 Pro-class at
1440p60). Each engine area and `space/` reads its own section and registers the keys it accepts; an unknown key is an
error that names the valid ones. Presets live in the engine ([0065](0065-engine-areas-for-the-toolbox.md)).

**What a preset may touch:**

| Area | Key | `low` | `high` |
|---|---|---|---|
| Shadows ([0062](0062-shadows-ambient-reflections.md)) | cascades | 2 × 1024² | 3 × 2048² |
| Reflections | SSR | off | on |
| Clouds (later) | technique | 2D layers | volumetric with reprojection |
| Ocean (later) | FFT bands | 3 | full set |
| Terrain render LOD ([0053](0053-terrain-density-field.md)) | refinement ratio λ, chunk jobs per frame | λ = 4 | λ = 6 |
| GTAO, probes, impostor and background bake resolutions | quality and size | lower | higher |
| Far-body tiers ([0063](0063-astronomical-rendering.md)) | impostor-to-terrain handoff size | larger (later handoff) | smaller |
| Simulation LOD presentation ([0057](0057-simulation-lod.md)) | T0 and T1 radii, live agent caps | T0 1 km surface / 20 km space, T1 2 km; ~150 agents | T0 2 km / 50 km, T1 4 km; ~300 agents |

The radii and caps are working values, replaced by VCH-093 and VCH-097 measurements. They are safe to vary only
because of 0057's fold-back rule: live individuals are a presentation of the aggregate, and only player-caused,
declared perturbations flow back.

**What a preset may never touch:** the fixed step and physics rate; the LOD0 cell size (0.5 m) and sample encoding;
which terrain chunks have colliders, the collider region and the install delay K ([0058](0058-physics-jolt.md)); the
travel governor's τ and v_floor ([0059](0059-gravity-flight-travel.md)); gravity; the sim-cell grid, coarse-tick periods
and catch-up rules; RNG keys; stage outputs and versions; anything in the universe fingerprint
([0055](0055-universe-identity-and-versioning.md)); anything replicated
([0056](0056-server-authoritative-multiplayer.md)). Simulation values that must hold on the slowest machine are sized
for it; a faster machine gains frame rate and visual range, not a different world.

**Falling behind.** A machine that cannot keep up degrades visually (render LOD coarsens, the terrain queue lowers the
effective λ) or stalls (a collider not ready at t + K holds the step). It never skips or reorders simulation work.

**Multiplayer.** The server's T0 radius and interest settings are server configuration; a client's preset changes only
what it draws and presents locally, which is cosmetic tier.

**Recording and tests.** A recording stores the preset in its header, because live individuals differ between presets
and the per-frame state hash covers them. A CI test replays one scenario under `low` and `high` and compares the
canonical and aggregate hashes, which must be equal.

**Not decided here:** a spatial render scale on `low` (shading at 0.75 with a non-temporal upsample, about −1.5 ms on
an M1-class GPU by estimate). It waits on the hardware-target packet; [0061](0061-temporal-accumulation-no-upscaling.md)
rules out temporal upscaling either way.

## Consequences

- Hardware decides how the world looks and how many creatures you see, never what happens.
- A simulation-affecting knob is a creator or server setting, never a preset key.
- Sizing simulation for the slowest machine leaves fast ones headroom unused.
- Budget estimates: `low` on an M1-class GPU at 1080p about 14.5 ms (2D clouds −2.8, smaller cascades −1.2, three ocean
  bands −0.4 from 18.9), about 13 ms with a spatial render scale; `high` on M3 Pro-class at 1440p about 11.9 ms, leaving
  about 4.5 ms for terrain and procgen compute.
- **Validation:** VCH-097 (SP-21): the VCH-085, VCH-086, VCH-090 and VCH-092 scenes on real base M1 and M3 Pro machines
  replace the estimates (needs hardware: a product decision). VCH-093 (SP-17) with T0 radius changes leaving the
  aggregate hash unchanged. VCH-085 (SP-09) shows the governor's peak chunk rate within budget on the `low` sizing.
- **Reopen if** the preset hash test ever differs and the cause cannot be moved to the visual tier, or real M1
  measurements show `low` cannot reach its target without touching a simulation value (then the floor or the target
  changes, through the owner).

## Alternatives considered

- **Simulation radii in presets with full fold-back, or a speed limit per tier.** Outcomes would follow hardware.
- **No presets, one feature set.** Either the M1 class runs at about 30 fps or the M3 Pro class wastes half its GPU.
- **Fully adaptive quality (dynamic resolution, automatic feature toggles).** Images would depend on load from frame to
  frame, which makes goldens and replays unreproducible; it can come later on top of fixed presets.
