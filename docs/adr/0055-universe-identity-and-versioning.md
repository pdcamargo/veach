# 0055: Universe identity and generator versioning: genesis, epochs, stability classes, body pinning, save layout

Date: 2026-11-27

## Status

Accepted.

## Context

Almost nothing in a Veach universe is stored: it is generated from a seed ([0051](0051-staged-generation-pipeline.md)).
What is stored is what differs: a dug hole, a base, a cut tree. That breaks three ways: the generator changes, the data
format changes, or the universe is not the one the save was made in (other mods, another scale, another Luau hook). A
wrong choice invalidates every save and server world. Research:
[11-persistence-versioning](../research/11-persistence-versioning.md).

## Decision

**Identity: `universe.json`.**

- `genesis` is written once and never changes: `universe_id` (UUID v4), `seed` (uint64 as a decimal string,
  [0046](0046-64-bit-values-across-luau-json-mcp.md)), the scale factors `s_p`, `s_o`, `s_a`
  ([0049](0049-scale-model.md)), the creator settings, the determinism contract version
  ([0043](0043-determinism-tiers-and-strict-floating-point.md)) and `created_tick`.
- `epochs` is append-only: `epoch`, `since_tick`, toolbox version, every stage's version and the mods (`id`, semver
  `version`, `roles`, `depends`, `sha256`). A new epoch is appended when the generation or simulation set changes.
- **Fingerprint** = hash of the canonical JSON of `genesis` plus the current epoch's generation- and simulation-role
  entries. It is checked on load, sent in the multiplayer handshake ([0056](0056-server-authoritative-multiplayer.md)),
  written into recordings and keys client chunk caches. A mismatch is an error naming the difference and the fix.
- Scale is genesis, so it is immutable. A different scale is a new universe with a new `universe_id`.

**Stages are versioned and classed.** Every stage registers `id`, `version` (an integer, bumped whenever its output
changes for any input) and a stability class:

| Class | Rule | Examples |
|---|---|---|
| `frozen` | The version current at genesis, forever; new versions apply only to new address space or new kinds | galaxy distribution, system layout, body physical parameters, orbits |
| `pinned` | Pinned per body when the body is anchored; unanchored bodies use the newest | terrain shape, water levels, biome map, resource placement |
| `refreshable` | Always the newest | flora and fauna appearance, colors, names, weather patterns |

Overrides on refreshable output are keyed by generated id; one whose id vanishes after a refresh is kept and reported
as orphaned. **Addressing is outside versioning**
([0050](0050-universe-addresses-and-seeds.md)): new generation takes new address ranges and never renumbers old ones.

**Body pinning.** A body is the pin unit: `bodies/<body-id>/body.json` holds `pinned_epoch` and the anchors. A body is
anchored by its first terrain edit, first placed persistent entity, first override on its generated objects, or a
recorded discovery; flying past or landing does not. One body never mixes stage versions, so it has no seams.

**Old versions stay runnable.** C++ stages keep every released version side by side in the stage registry
(`terrain_shape_v2`, `terrain_shape_v3`), each with committed golden output hashes for a fixed address set, compared
on the macOS gate and Windows CI. Changing a version's output without bumping it fails CI. Luau generation and
simulation mods are vendored into the save by content hash (`mods/<sha256>/`). A save that needs a C++ module version
that is not installed fails to load with an error that lists the bodies, the module and the options (install it, bake,
or reset those bodies). A bake migration is the only way to retire a C++ version.

**Save layout** (one format for single player and servers; a directory, packed only for transport):

```text
<name>.vsave/
  universe.json  migrations.json  clock.json
  mods/<sha256>/...
  players/<uuid>.json
  entities/<frame-id>.jsonl
  bodies/<body-id>/body.json
  bodies/<body-id>/terrain/<region>.vreg
  journal/
```

- JSON is canonical (sorted keys, fixed float format, int64 as strings, `$schema` from reflection); save, load, save
  is byte-identical.
- A region file is an offset table plus one compressed record per LOD0 chunk: `{format, epoch, base_hash, overrides}`,
  where `overrides` are the sparse absolute samples of [0054](0054-terrain-edits-and-water.md) (int16 distance plus u8
  material) and `base_hash` is the hash of the generator output they were made on. The codec is named in the record
  header; zstd is the working choice, added to the dependency set by its own commit when the writer lands.
- `journal/` holds brush operations since the last full save, in 0054's wire format; writes are atomic (temporary
  name, then rename).
- `veach save dump <file>` prints any record as JSON.
- Persistent entities carry a persistent id (VCH-103), positions as (frame id, local position)
  ([0045](0045-large-world-positions-and-reference-frames.md)). Format migrations, a separate axis from stage versions,
  are recorded in `migrations.json`.
- Mod load order is a topological sort of `depends`, ties broken by `id`.

**Layering** ([0065](0065-engine-areas-for-the-toolbox.md)): persistent ids, reflection arrays and int64 (VCH-099),
canonical JSON and the region container are engine; genesis, epochs and pins are `space/`.

## Consequences

- A generator update never moves a hole or a base on an anchored body; unvisited bodies still get improvements.
- Old stage code lives as long as some save pins it, until a bake retires it.
- Agents can read, diff and validate a save without running the engine.
- **Validation:** VCH-094 (SP-18): a two-version stage, an anchored body unchanged after the update (FLIP 0, same
  hash) and an unanchored one changed; byte-identical save, load, save. Stage hashes on both platforms come from
  VCH-077 (SP-01).
- **Reopen if** bit-exact cross-platform base generation proves infeasible (`base_hash` cannot be strict: store
  edited chunks as absolute frozen chunks instead), or the owner wants planets to refresh where people built.

## Alternatives considered

- **Always the latest generator.** Every update breaks edits (the No Man's Sky experience).
- **Store the generated base of edited chunks.** Saves grow, and generator fixes never reach them.
- **Op logs compacted into relative deltas** (the research proposal). Relative deltas change meaning when the base
  changes; absolute overrides plus `base_hash` detect it. Ops stay as journal and wire format.
- **Pin per chunk or region.** Seams where versions meet; region pins for huge bodies stay an extension point.
