# 0051: Generation is a staged pipeline of typed physical records, registered by stable id, that never reads scale

Date: 2026-11-27

## Status

Accepted.
Amended by [0068](0068-amendments-from-sp-05.md): a body's radius is computed in S1.

## Context

A planet's look must follow from its physics: mass, composition and orbit set gravity, temperature, which gases it
keeps, its climate, biomes and life. Independent noise layers give deserts at the poles of ice worlds. Several rules
are non-local (snow line, orbit spacing, Roche limit, Hill sphere, drainage), so some stages need a whole system or a
whole body as context. The sky from anywhere must show the stars really there without generating full systems. Mods
must add kinds without reshuffling existing content, and the same seed must give the same planet at every scale
([0049](0049-scale-model.md)). Precedents: Elite's Stellar Forge, SpaceEngine's climate model.
Research: [03-generation-pipeline](../research/03-generation-pipeline.md), [05-procgen-graph](../research/05-procgen-graph.md) §4.2.

## Decision

**Stages and their records** (toolbox stages in `space/gen/`, run by the engine's stage runtime,
[0052](0052-procgen-runtime.md)):

| Stage id | Context | Main outputs | Cached |
|---|---|---|---|
| `space:universe` (G0) | seed, manifest | galaxy kind and parameters | always |
| `space:galaxy` (G1) | whole galaxy | density, age and metallicity fields; dust; deep-sky and background catalogs | forever |
| `space:cell` (G2) | G1 + cell address | star stubs per layer brightness band: position, mass, age, metallicity, multiplicity, luminosity | LRU |
| `space:system` (S1) | whole system | evolved stars, binary orbits, snow line, habitable zone; bodies with Hill spacing and Roche limits, rings, belts; per body mass, composition, Kepler elements, spin, obliquity, tidal lock | per visited or targeted system |
| `space:body.physics` (B1) | its system | radius, g, v_esc, equilibrium temperature, retention, atmosphere, greenhouse, internal heat, hydrosphere, **body kind** by classifier | with its system |
| `space:body.topography` (B2) | whole body | cube-map fields in dimensionless units (elevation as a fraction of R): plates, continents, basins, ranges, ocean mask, drainage | per near body |
| `space:body.climate` (B3) | B1 + B2 | seasonal temperature and precipitation, ice cover | per near body |
| `space:body.biomes` (B4) | B2 + B3 | biome id and blend weights from registered rules | per near body |
| `space:body.life` (B5) | B1 + B4 | biosphere record, species pools, carrying capacities | per body |
| `space:chunk.detail` (C) | B1-B5 + chunk key | density and materials ([0053](0053-terrain-density-field.md)), placement candidates | streaming |
| `space:objects` (O) | C + placement layers | instances: kind id, body-local transform in meters, object seed | streaming |

**Rules:**

- **Records are physical and unscaled** (SI units; light-years for galaxy positions) and are reflected structs
  ([0026](0026-reflection-everywhere.md)), so JSON, schemas, Luau and MCP come free. Lists in records need the
  reflection walker's array support (VCH-099).
- **A stage reads only its ancestors and its own address.** Anything that needs siblings moves into the parent's
  whole-context stage. A read from a later stage, or a cycle, fails at load with an error code and a hint.
- **No stage reads s.** `StageContext` (address, read-only ancestor records, `HashStream`s from
  [0044](0044-counter-based-hash-randomness.md), dmath) has no scale field; G0 to B5 hashes are equal at every s. C and
  O are pure functions of address, ancestors and coarse fields that also receive realized radii, so which tree stands
  where legitimately differs across scales.
- **Every stage registers** `id`, integer `version` (bumped whenever any output changes), stability class (`frozen`,
  `pinned`, `refreshable`, [0055](0055-universe-identity-and-versioning.md)), tier
  ([0043](0043-determinism-tiers-and-strict-floating-point.md)), input and output record types, and a test seed set.
  G0 to B5, C at collider resolution and O for gameplay placement are canonical.
- **Registries by stable string id (`namespace:name`), run in id order**, never load order, never enums: body kinds,
  star kinds, biomes, phenomena, deep-sky kinds, placement layers, stages, nodes. Mods claim new slots
  ([0050](0050-universe-addresses-and-seeds.md)); reclassifying existing addresses is a version bump.
- **The galaxy is continuous:** G2 places candidates at a majorant density and keeps each with probability ρ(x)/ρ_max
  at its own position, so no per-cell constants and no seams at cell borders.

**The query service** `space::Universe` ([0019](0019-veach-modules-and-services.md)) memoizes stages under the key
(address, stage id, version, input hash, manifest fingerprint) and exposes `galaxy()`, `cell(addr)`, `system(addr)`,
`body(addr)`, `field(body, field_id)` (async on the job system, VCH-015), `stars_visible(pos_ly, limiting_magnitude)`
(a fixed neighborhood of cells per layer), `nearest_systems(pos_ly, n, filter)` and `stage_hash(addr, stage)` (64-bit,
over the record's canonical serialization). The module registers MCP tools `universe_query` and `universe_hash`
(VCH-100); a `universe` CLI subcommand dumps any address at any stage headless.

**Overlays** (owned by [0055](0055-universe-identity-and-versioning.md)): authored overrides replace a record and feed
downstream stages; annotations (names, discoveries) never feed generation; terrain edits
([0054](0054-terrain-edits-and-water.md)) and object overrides are keyed by address.

**Tests:** stage-hash goldens `tests/golden/determinism/<stage>.json` on the macOS gate and Windows CI, and a CI check
that G1-B5 hashes are equal at s ∈ {1, 0.1, 0.01} and across thread counts. A failing stage is
`procgen.stage_failed`; nothing partial is kept.

## Consequences

- Planets are coherent by construction, and any property of any address is computable and hashable headless.
- Stage boundaries set versioning granularity: too coarse and a small change resets too much (NMS Origins reset all
  creatures), too fine and the manifest grows. The table is the starting cut.

**Validated by** VCH-081 (100,000 systems headless: histograms, zero orbit-stability violations, hashes equal across s
and thread counts, ms per system) and VCH-094 (a two-version stage leaves an anchored body unchanged and changes an
unanchored one). **Reopen if** VCH-081 shows S1 + B1 too slow to hide behind an FTL transition on an M1-class CPU
(simplify formation to direct sampling before giving up coherence), or canonical doubles prove unreproducible across
platforms (gameplay-read records move to fixed point).

## Alternatives considered

- **Independent noise layers:** cheap and incoherent.
- **Full simulation** (accretion, n-body, a climate GCM): slow and hard to make deterministic.
- **Generating at the scaled size:** the planet's physics changes with s.
- **One node graph for records too:** a greenhouse model is fifty lines of code and two hundred nodes.
