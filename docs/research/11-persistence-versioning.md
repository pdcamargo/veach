# 11: Persistence and generator versioning

How player and simulation edits are stored on top of generated content, what a save and a server world look like,
what makes a universe "the same universe", and how saves survive changes to procgen algorithms and mods. Split out of
Q3 and Q6 by the [scope analysis](00-scope.md#questions-added-or-split) because it is the most expensive decision
to undo: a wrong choice here invalidates every save and every server world. Raw notes and sources:
[notes/11-persistence-versioning-notes.md](notes/11-persistence-versioning-notes.md).

## 1. The problem in Veach's terms

Almost nothing in a Veach universe is stored. Galaxies, systems, bodies, terrain, flora and creatures are functions
of a seed, run through the staged pipeline Q3 defines and the graph Q5 defines. What *is* stored is everything that
differs from those functions: a hole a player dug, a base, a ship, a tree that was cut, a tamed creature, a lava flow
from an eruption Q7 simulated, the state of an economy. Persistence is the contract between those stored differences
and the generator they are differences *from*.

That contract breaks in three ways, and the design must handle all three:

1. **The generator changes.** A toolbox release improves terrain noise, or a creator edits their Luau biome stage. A
   hole dug into the old terrain now floats in the air or is buried, and a base sits on a hill that moved. NMS,
   Elite, Minecraft and Valheim all hit this (section 2).
2. **The format changes.** A component gains a field, a type is renamed, an edit record grows. This is ordinary
   schema migration, and it is a separate axis from (1).
3. **The universe isn't the one the save was made in.** Different mods, a different scale, different creator
   settings, a client with a different Luau hook. With Luau procgen hooks ([ADR 0042](../adr/0042-space-toolbox-layering.md),
   [00-scope finding 3](00-scope.md#the-five-findings-that-matter-most)), two machines can produce two universes
   from one seed and neither knows.

Veach-specific constraints that shape the answer:

- **Text everywhere** (VISION principle 1): data is JSON with schemas generated from reflection
  ([ADR 0010](../adr/0010-asset-formats-and-identity.md), [0026](../adr/0026-reflection-everywhere.md)). Agents must
  be able to read, diff and validate a save without running the engine.
- **The reflection walker can't yet express a save.** Arrays, vectors, maps and non-string opaque types are
  `unsupported` and skipped by JSON ([ADR 0026](../adr/0026-reflection-everywhere.md)); JSON numbers lose int64
  above 2^53; there are no persistent entity ids or entity references. These are engine gaps, not toolbox ones.
- **Determinism** ([ADR 0018](../adr/0018-engine-lifecycle-and-phases.md)): simulation is a function of seed,
  script and input, and [record/replay](../specs/record-replay/spec.md) proves it with per-frame state hashes. A
  save is the snapshot a recording starts from.
- **Luau writes through command buffers** ([ADR 0007](../adr/0007-scripting-luau.md)): an edit is already a
  transaction that either applies fully or not at all. That maps directly onto edit records.
- **Scale is a creator setting.** Bodies are generated in physical units and s_p, s_sys scale celestial lengths.
  Edits and buildings are human-scale and never scale. The format must not assume a scale, yet a hole's position on
  a planet does depend on the planet's radius.

## 2. Options, with precedents

### 2.1 Universe identity

| Option | What defines the universe | Precedent | Verdict |
|---|---|---|---|
| A. Seed only | `seed` | Most seed-sharing games | Wrong for Veach: mods and Luau hooks change output silently. |
| B. Seed + version string | `seed`, toolbox version | Factorio requires exact game version and mods for multiplayer ([wiki](https://wiki.factorio.com/Desynchronization)) | Too coarse: every patch "changes" the universe, and it ignores mods. |
| C. Seed + full manifest | seed, scale, creator settings, generator stage versions, mods by id/version/content hash | Factorio's mod sync and CRC checks, extended with content hashes | Right shape, but it treats one change as a new universe. |
| **D. Genesis + epoch table** | An immutable *genesis* (seed, scale, creator settings, universe UUID) plus an append-only list of *generator epochs* (the stage versions and mods in force from a tick onwards). Places record the epoch they were pinned at. | Minecraft's per-chunk `DataVersion` ([gist](https://gist.github.com/aikar/90cb65d9b6a1bb761e65023ae10f43ca)) applied to generator output instead of data format; NMS's per-layer freezing | **Recommended.** The universe keeps its identity across updates, and every place knows which generator made it. |

### 2.2 Generator versioning: what a generator change may do to existing places

| Option | Rule | Precedent | Cost |
|---|---|---|---|
| A. Always latest | Every load regenerates with today's code; only edits are stored | Early NMS updates, per player reports (**unverified**) | Edits float or get buried. Unacceptable for terrain once bases exist. |
| B. Freeze everything, add new content in new address space | Old stages are never changed for existing places; new generation goes to new systems or new kinds | NMS Worlds Part II: new terrain "applied to all planets found within purple star systems", "terrain in existing systems has not been regenerated" ([Shacknews](https://www.shacknews.com/article/142889/no-mans-sky-update-55-patch-notes)); Origins added new planets beside old ones | Maximum stability, least freedom; old code runs forever. |
| C. Freeze per layer | Layers that things are built on (terrain shape, body physics, orbits) are frozen; dressing (flora, fauna, colors, weather) refreshes | NMS Worlds Part I: "Underlying terrain shapes have not been reset", while planets were "transformed in wild and radical ways" ([nomanssky.com](https://www.nomanssky.com/worlds-part-I-update/)); Origins reset creatures; Elite Odyssey kept Stellar Forge inputs and replaced the surface generator ([PC Gamer](https://www.pcgamer.com/heres-how-frontier-rebuilt-a-galaxys-worth-of-planets-for-elite-dangerous-odyssey)) | Needs a stability class per stage. |
| D. Pin on first visit | A region generated at version v keeps v; unvisited regions get the newest | Minecraft (chunks stored once generated, 1.18 new terrain only in new chunks, with blending at borders, [minecraft.net](https://www.minecraft.net/en-us/article/minecraft-1-18-pre-release-1)); Valheim ("biome generation will only work properly on areas that have not yet been explored", [PC Gamer](https://www.pcgamer.com/games/survival-crafting/should-you-start-a-new-character-and-world-for-valheim-1-0/)) | Seams between regions of different versions; Minecraft's blending is imperfect ([MC-243089](https://bugs.mojang.com/browse/MC-243089)). "Visited" is fuzzy when you see half a planet from orbit. |
| **E. Pin on anchor, per body, per stage class** | Each stage declares a stability class. A *body* is pinned to the current epoch the first time something persistent depends on it (an edit, a base, a placed object, a recorded discovery). Pinned bodies run their stages at the pinned epoch; unpinned bodies float to the newest. | Combines C (per-layer classes) with D (pin where it matters), using the body as the seam-free unit | **Recommended.** Old stage versions must stay runnable while any pinned body uses them. |
| F. Store the generated base | Bake the generated data of visited regions into the save; regenerate nothing | Minecraft region files store every generated chunk ([wiki](https://minecraft.fandom.com/wiki/Region_file_format)); Valheim zones | Impossible at planet scale for everything you fly over; viable only for *edited* chunks, as the escape hatch when an old version is retired. |

### 2.3 Edits on generated content

Three kinds of stored state, which need different representations:

**Terrain edits.**

| Option | What is stored | Size | Survives a base change | Replay/load cost |
|---|---|---|---|---|
| Op log | Brush operations: kind, version, tick, author, chunk-local position, radius, strength, material | Tiny: ~32-48 B per op; a flattened 100 m × 100 m pad is tens of KB (estimate) | Best: intent re-applies ("dig here") | Grows without bound: 100 h of mining at 10 ops/s ≈ 3.6 M ops, 115-170 MB raw (estimate) |
| Sparse voxel delta | Per chunk, the quantized density (and material) difference from the generated base, narrow band only | Scales with edited *surface area*: the same pad at 0.25 m voxels ≈ 2.3 MB raw, well under 1 MB with zstd (estimate) | Fair: a relative delta re-applies roughly ("2 m lower than the ground") | Bounded per chunk |
| Absolute frozen chunk | The chunk's full final density | Largest | Perfect for that chunk, seams with regenerated neighbors | Bounded, no base needed |

Space Engineers uses a binary octree voxel store (`.vx2`) next to XML world and object files
([SE wiki](https://spaceengineerswiki.com/Save_file_sbc)); whether it stores only modified planet data is
**unverified**. All numbers above are back-of-envelope estimates, not measurements; spike S2 measures them.

**Generated objects** (a tree, a rock, a creature herd, a ruin). Options: (a) store their absolute state like any
entity once touched, or (b) store an *override record* keyed by the object's generated id (Q3's address path:
body → chunk → object index): `removed`, `modified` (component deltas), or `promoted` (it became a persistent entity,
e.g. a tamed creature, with a link to the generated id it came from). (b) costs bytes only for what changed and lets
untouched dressing refresh when its stage changes.

**Authored objects** (bases, ships, placed items, players). They have no generator to diff against: store them
absolutely, with persistent ids, through reflection. Position in their parent frame (a body-fixed frame for a base, a
ship frame for an interior, Q1), in meters.

**Simulation-made edits** (eruptions, landslides, Q7). Either regenerate them from seed and clock each time, or record
them as edit ops at the moment they happen. Recording wins: it freezes the past against later changes to the eruption
algorithm, orders them correctly against player edits on the same chunk, and replays like any other edit. The op stores
the event's parameters (vent, tick, volume, event seed), not the voxels it produced, so it stays small.

### 2.4 Format

| Option | Precedent | Verdict |
|---|---|---|
| All JSON | Veach principle | Unworkable for voxel deltas: megabytes of numbers as text, slow, and no gain in readability. |
| All binary (one file) | Many console games | Violates text-everywhere; agents and diffs can't see anything. |
| **Directory of canonical JSON plus binary region files** | Space Engineers (`Sandbox.sbc`, `.sbs` XML + `.vx2` voxels), Minecraft (level.dat + Anvil region files: 32×32 chunks, offset table, each chunk compressed individually, [wiki](https://minecraft.fandom.com/wiki/Region_file_format)) | **Recommended**, with a `veach save dump` that turns any binary blob into JSON, so nothing is opaque to tools. |

Compression: zstd per chunk record. On the zstd README's benchmark (Silesia corpus, desktop x86), zstd -1 reaches
ratio 2.90 at 510 MB/s compress and 1550 MB/s decompress, against zlib -1 at 2.74 and 105 / 390 MB/s; LZ4 is faster
(3850 MB/s decompress) at ratio 2.10 ([facebook/zstd](https://github.com/facebook/zstd)). Trained dictionaries help
many small records, which edit chunks are. JSON parts stay uncompressed in the directory (diffable) and get compressed
only when a save is packed for transport.

### 2.5 Data-format versioning (separate from generator versioning)

Every saved record type carries a format version; loading runs a chain of migrations from the stored version to the
current one, the way Minecraft's DataFixerUpper converts each chunk from its `DataVersion`
([wiki.vg](https://wiki.vg/Map_Format)). Mods ship their own migrations; Factorio's model is the one to copy:
JSON migrations for renames, script migrations for state changes, run in mod order then file name, and the save
records which ones were applied so none runs twice
([Factorio docs](https://lua-api.factorio.com/latest/auxiliary/migrations.html)).

### 2.6 Mods

Identity = id + semantic version + content hash. Factorio requires the exact same mods and mod settings for
multiplayer and lets clients sync mods from the server
([wiki](https://wiki.factorio.com/Desynchronization)). Veach adds what Factorio doesn't need: a *role* per mod,
because only some mods change the universe:

- `generation`: registers or replaces procgen stages. Part of the epoch; must match on every machine.
- `simulation`: authoritative gameplay systems or persisted components. Must match in multiplayer.
- `presentation`: shaders, UI, audio, render features. Client-only; excluded from the fingerprint.

## 3. Tradeoffs

| | Recommended design (genesis + epochs, pin on anchor, op log compacted to deltas, JSON + region files) | Main alternative (store generated base, Minecraft/Valheim style) | Simplest (seed + always-latest + absolute edits) |
|---|---|---|---|
| Complexity | Highest: stage registry with versions and stability classes, epoch table, pin records, compaction, migrations | Medium: no regeneration of old versions, but huge storage and streaming of stored chunks | Low |
| Performance | Load cost = regenerate base at pinned epoch + apply compacted delta + op tail. Generation cost is already paid for unedited terrain | Load from disk is fast; save size grows with exploration, not edits; flying over a planet writes gigabytes | Fast, but wrong after updates |
| Determinism | Needs bit-exact base regeneration on authoritative paths (Q12) for the `base_hash` check; edits applied in fixed point are exact | Needs determinism only at first generation | Same as recommended |
| Extensibility | Luau and C++ stages version independently; a creator can refresh flora without touching terrain; mods can be removed via a new epoch | Generator changes never reach visited places | Any change breaks old saves |
| Testability | Golden saves, base hashes and per-version output hashes catch silent generator drift in CI | Easy round trips; no drift detection needed | Nothing to test, nothing protected |
| Scale range | Scale-free format; scale fixed per universe once anchored | Same | Same |
| Storage | ~edited surface area; independent of exploration | ~explored area | ~edits |

The hard cost of the recommendation is **code retention**: a frozen or pinned stage version must stay runnable as long
as any save uses it. For Luau stages this is cheap (the save vendors the script by content hash, section 4.3). For
C++ stages it means old implementations stay in the binary, side by side, under golden-hash tests. The escape hatch
when a version must go is a *bake migration*: the last release that has both versions writes absolute frozen chunks
for every edited chunk on bodies pinned to the old version (plus a border band), then repins those bodies.

## 4. Recommendation

### 4.1 Universe identity: `universe.json`

```jsonc
{
  "$schema": "../schemas/universe.schema.json",
  "format": 1,
  "genesis": {                                  // immutable after creation
    "universe_id": "6f0c…-uuid-v4",             // two universes with one seed differ by this
    "seed": "13377345120000000001",             // uint64 as a decimal string (JSON loses ints above 2^53)
    "scale": { "planet": 0.1, "system": 0.01, "galaxy": 1.0 },   // factors from Q1/Q2; immutable once anchored
    "creator_settings": { "surface_gravity": "physical", "day_length": "physical" },  // per-quantity choices (Q2)
    "determinism_contract": 1,                  // Q12's contract version
    "created_tick": "0"
  },
  "epochs": [                                   // append-only
    { "epoch": 0, "since_tick": "0", "toolbox": "0.4.0",
      "stages": { "space.galaxy.distribution": 1, "space.body.physical": 1, "space.terrain.shape": 3,
                  "space.flora.placement": 2 },
      "mods": [ { "id": "com.example.alien-biomes", "version": "1.2.0", "roles": ["generation"],
                  "hash": "sha256:…" } ] }
  ]
}
```

- **Fingerprint** = hash of the canonical JSON of `genesis` plus the current epoch's generation- and simulation-role
  entries. It is checked on load, sent in the Q6 handshake, written into recordings and used to key client-side chunk
  caches. A mismatch is an error that names the difference and the fix ("mod com.example.alien-biomes 1.2.0 has hash
  …, you have 1.2.1: sync mods with the server"), per the "errors tell you the fix" rule.
- A new epoch is appended whenever the generation- or simulation-relevant set changes (toolbox update, mod added,
  removed, or updated). Epochs are small; a universe that lives for years accumulates tens of them.
- **Scale is genesis**, so immutable. Changing it is a new universe (a content-equal fork with a new `universe_id`
  and no edits carried). A hole's position is meaningful only for one planet radius; moving bases between scales
  is at most a later tool that relocates them by (latitude, longitude, height above generated ground).

### 4.2 Stages are versioned and classed; bodies pin on anchor

- Every procgen stage (C++ or Luau, Q5) registers `id`, `version` (integer, bumped whenever output for any input
  changes) and a **stability class**:
  - `frozen`: the universe uses the version current at genesis forever; new versions apply only to new address
    space or new kinds (NMS Worlds II). Galaxy distribution, system layout, body physical parameters, orbits.
    Changing these moves stars on maps players navigate by.
  - `pinned`: pinned per body when the body is anchored; unanchored bodies use the newest. Terrain shape, water
    levels, biome map, resource placement.
  - `refreshable`: always newest. Flora and fauna appearance and placement detail, colors, names, weather patterns.
    Edits on refreshable output are override records keyed by generated id; when an id no longer exists after a
    refresh, its override is orphaned, kept in the save and reported, not silently dropped.
- **Addressing is version 1 forever.** The scheme that turns (sector, system, body, chunk, object index) into ids
  (Q3) is outside versioning: changing it would break every reference in every save. New generation adds new
  address ranges; it never renumbers old ones.
- A **body** is the pin unit: `bodies/<body-id>/body.json` holds `{ "pinned_epoch": 3, "anchors": [...] }`. One
  body never mixes versions, so there are no in-body seams (Minecraft's blending problem). Region-level pins (one
  face-quadtree node of a large body) are an extension point, not a first build.
- A body is anchored by: the first terrain edit, the first placed persistent entity, the first override on its
  generated objects, or a recorded discovery. Merely flying past or landing does not anchor (product decision P1).

### 4.3 Old versions stay runnable

- C++ stages keep every released version in a versioned registry (`terrain_shape_v2`, `_v3` side by side). CI holds
  a golden output hash per stage version for a fixed set of addresses, on macOS arm64 and Windows x64 (shared with
  Q12). Changing a version's output without bumping it fails CI.
- Luau generation mods are **vendored into the save** by content hash (`mods/<sha256>/…`). A pinned body whose epoch
  names a Luau mod finds its exact source in the save, even after the user updated or removed the mod. A save is
  self-contained for everything written in Luau.
- C++ module stages can't be vendored. A save that needs a missing module version fails to load with an error
  listing the bodies, the module and the options: install it, bake (needs it once), or reset those bodies' edits.
- Retiring a C++ version requires a bake migration first (section 3).

### 4.4 Edits

- **Terrain:** the authoritative form is an **op log per chunk**: versioned op kinds (sphere add/subtract, flatten,
  smooth, raise, lower, paint material, plus system ops like `eruption`), each with tick, author
  (`player:<uuid>` or `system:<stage>`), chunk-local position in fixed point and its parameters. The same records are
  the Q6 wire format for edits. When a chunk's log passes a threshold (op count or replay cost), it is **compacted**
  into a quantized sparse density delta (difference from the base, narrow band, int16 density + material byte),
  stored with the `base_hash` of the generated base it was computed against, and the log restarts. Ops are applied
  in fixed point on quantized density, so the result is bit-exact on every CPU (Q12) and server and clients agree.
  Op kinds are versioned like stages; compaction limits how long old op code must live.
- **Luau composite ops** (a creator's "terraform pad" tool) expand to primitive ops at commit time through the
  existing command buffer, so the log holds only primitives the core knows how to apply.
- **Generated objects:** override records keyed by generated id (`removed`, `modified`, `promoted`).
- **Authored objects:** persistent entities with a UUID (not a Flecs entity id), saved through reflection, position
  as (frame id, local position) per Q1, entity references by persistent id. Ship blueprints are standalone JSON
  assets (shareable), separate from ship instances.
- **Simulation state** (Q7 aggregates: populations, economies, factions, automated bases): JSON records keyed by
  generated ids, with the tick of their last update for catch-up.
- **System edits** are materialized as ops when they happen (section 2.3).

### 4.5 Save layout

```text
my-universe.vsave/                    # a directory; packed (zip + zstd) only for transport
  universe.json                       # genesis + epochs (section 4.1)
  migrations.json                     # data-format migrations applied (Factorio style)
  clock.json                          # universal tick (int64 as string, Q2)
  mods/<sha256>/...                   # vendored Luau generation and simulation mods
  players/<uuid>.json
  entities/<frame-id>.jsonl           # persistent entities by parent frame, canonical JSON lines
  bodies/<body-id>/body.json          # pin record, anchors, overrides, simulation state
  bodies/<body-id>/terrain/<region>.vreg   # binary region file: offset table + per-chunk zstd records
  journal/                            # write-ahead log of edits since the last full save
```

- Canonical JSON: sorted keys, a fixed float format, int64 and ids as strings, `$schema` keys pointing at schemas
  generated from reflection. Save → load → save is byte-identical.
- Region file record: `{format, epoch, base_hash, compacted_delta?, op_tail}`; the layout is documented and
  versioned; `veach save dump <file>` prints any record as JSON.
- Writes are atomic (write to a temporary name, then rename); the journal means a crash loses at most the edits
  since the last journal flush.
- Single player and dedicated servers use the same format. A client never writes the server's save; it may cache
  server chunks keyed by fingerprint and chunk epoch.
- Save files are user data, not repository assets; [ADR 0021](../adr/0021-git-lfs-for-binaries.md) applies only to
  golden saves committed for tests (`*.vreg` goes to LFS).

### 4.6 Mods

- Mod manifest: `id`, `version` (semver), `roles`, `depends` (with version ranges), content hash computed by the
  toolbox over the mod's files.
- Load order is a topological sort of `depends`, ties broken by `id`, never by filesystem or install order.
- **Adding** a generation mod appends an epoch; unanchored bodies see it at once, pinned ones don't.
- **Removing** a generation mod appends an epoch without it. Pinned bodies keep running it from the vendored copy
  (Luau) or fail with the fix (C++). Persisted components from a removed simulation mod are kept as opaque JSON
  through load and save, reported as orphaned, and dropped only by an explicit `veach save prune --mod <id>`.
- In multiplayer the server's required set (generation + simulation roles) must match by hash. Luau mods are
  downloaded from the server into the sandbox; C++ modules must be installed by the user.

### 4.7 Testability

- **Golden saves in CI:** each release that changes a save format or a stage version commits a small save
  (tests/golden/saves/), and CI loads every one with the current build, regenerates its pinned bodies, checks every
  chunk's `base_hash`, and round-trips it byte-identically.
- **Stage output hashes** per stage version on both CI platforms (Q12).
- **MCP tools:** `universe_manifest`, `save_inspect`, `chunk_edits`, `pin_status`, so an agent can ask "why does
  this hole look wrong" and see the epoch, the base hash and the op tail.
- **Record/replay:** the header gains the universe fingerprint and a reference to the starting save; a fingerprint
  mismatch is `replay.invalid`. A save is a snapshot and a recording is the input log on top, so "record a bug in a
  heavily edited world" works. The state hash already covers Scriptable components; it extends to edit-log heads.

## 5. Extension points

| What | Luau | C++ module | Toolbox core / engine |
|---|---|---|---|
| A new procgen stage with a version and stability class | Yes | Yes | Registry, epoch table: core |
| A new persisted component | Yes (script components are reflected; add a `Persistent` tag) | Yes | `Persistent` tag, persistent ids, entity references: **engine** |
| Data-format migrations for a mod's components | Yes (`migrations/*.json` renames, `*.luau` state) | Yes | Migration runner and applied-list: **engine** |
| Composite terrain tools | Yes (expand to primitive ops) | Yes | |
| New primitive terrain op kinds | No (performance and determinism) | Yes, versioned, fixed-point | Op application contract: core |
| New edit layer kinds (vegetation density paint, water level overrides, 2D masks) | No | Yes, with a binary codec and a JSON dump | Region file container: **engine** (generic chunked blob store) |
| Generated-object override handling (what "harvested" means) | Yes | Yes | Override record format: core |
| Catch-up of simulation state on load | Yes | Yes | Q7 owns the schedule |
| Region-level pinning for very large bodies | No | No | Core, later |
| Fingerprint, handshake, vendoring, bake migration | No | No | Core (handshake mechanics generic: engine) |

Per [ADR 0042](../adr/0042-space-toolbox-layering.md), the generic pieces move down into `engine/`: persistent
entity ids and references, the `Persistent` tag, int64 and arrays in the reflection walker, canonical JSON, format
migrations, the content-addressed blob store and the region container. The toolbox owns what is space-specific:
genesis, epochs, stage classes, body pins, terrain ops and overrides.

## 6. Interactions with the other questions

- **Q1 coordinates:** saved positions are (frame id, local position) with int64 or double components as strings in
  JSON; body-fixed frames for bases. Because chunk addresses and positions depend on the radius, scale must be
  immutable per universe.
- **Q2 mechanics and time:** the universal tick (int64) is saved; generated orbits are not. Player ships on rails
  store orbital elements and their epoch tick. Creator settings that Q2 makes per-quantity go into genesis.
- **Q3 pipeline:** Q3 owns stages and ids; Q11 needs from it a stable addressing scheme outside versioning, a stage
  stability class on every stage, and "new generation goes into new address space" as a rule.
- **Q4 terrain:** Q4 picks the density representation and chunk layout; Q11 needs quantized density in edit layers,
  deterministic base generation for authoritative chunks, a chunk-local fixed-point coordinate, and a compaction
  hook. If Q4 meshes on the GPU in a non-bit-exact way, the base must still be regenerable bit-exactly on the CPU for
  `base_hash`, or the design falls back to absolute frozen chunks (section 7).
- **Q5 procgen graph:** the stage registration API carries version, stability class and golden-hash tests; Luau
  hooks are content-hashed and vendored.
- **Q6 networking:** the handshake compares fingerprints; edit ops are the wire format; the server is the single
  writer of the save; Luau mods download from the server. Edits as deltas are the same records in both places.
- **Q7 simulation LOD:** system edits are materialized as ops; aggregate state is JSON with a last-update tick.
  Algorithms in Q7 can change freely without rewriting history.
- **Q8 renderer:** nothing rendered is saved. Screenshot goldens of a pinned body are a useful regression test
  for "an update changed a pinned planet".
- **Q9 physics:** colliders are derived from chunks and never saved; body velocities of persistent entities are.
- **Q10 flight:** ship state in its frame, travel tier and FTL state are saved like any persistent component.
- **Q12 determinism:** `base_hash` and stage golden hashes require bit-exact authoritative generation across ARM and
  x64. Fixed-point op application removes floats from edit replay. Q12's contract version is in genesis.

## 7. M1-class vs M3 Pro-class

Minor, as expected. Saves are CPU and storage work, not GPU work.

- Load of an edited chunk = regenerate base + decompress delta + replay op tail. zstd decompression runs at well over
  1 GB/s per core on desktop CPUs (README benchmark, not measured on Apple silicon), so the cost is the base
  generation and the op tail. On a base M1 (4 performance cores), a lower compaction threshold keeps op tails short;
  the threshold is a tunable, not a format change.
- Base M1 machines often have 8 GB of unified memory: edit layers must stream with the chunks, never sit fully
  resident. The format already allows it (per-chunk records in region files).
- Nothing in the format differs between tiers.

## 8. What would change my mind

- **Q12 finds bit-exact cross-platform base generation infeasible** for terrain. Then `base_hash` can't be strict,
  servers can't trust clients to regenerate edited chunks, and edited chunks should be stored as absolute frozen
  chunks (option F for edited chunks only), sent by the server. Saves grow by roughly the compacted-delta size times
  a constant, and generator changes stop mattering for edited chunks.
- **Spike S2 shows op logs stay small** in realistic heavy play (long tails compress well, replay is cheap). Then drop
  compacted deltas and keep only the op log, which also gives the best migration behavior.
- **The owner wants planets to refresh even where people built** (NMS-style "universal refresh" every big release).
  Then relative deltas plus a bake-and-blend migration become the main path, not the escape hatch.
- **No generation mods in multiplayer** (product): the fingerprint and download path simplify a lot.

## 9. What is expensive to undo

1. **The addressing scheme** of generated ids: every override, pin and edit references it.
2. **Scale immutable per universe** and edits in chunk-local meters: relaxing it later means a relocation tool for
   every kind of stored thing.
3. **The epoch and pin model**: switching later to "store generated base" or "always latest" changes every save.
4. **Edit semantics** (relative delta vs absolute) decide how edits behave under migration; once players have
   years of edits, changing them changes what they built.
5. **int64 and ids as JSON strings**: a switch later breaks every JSON consumer, including MCP clients.
6. **Mod identity by content hash with roles**: a mod ecosystem grows on top of it.
7. **Persistent entity ids** distinct from Flecs ids.

Cheap to change later: compression codec (per-record header names it), region file size, compaction thresholds, the
directory layout below the top level, MCP tools.

## 10. Proposed ADRs

1. Universe identity: an immutable genesis plus an append-only epoch table
2. Procgen stages are versioned with stability classes (frozen, pinned, refreshable)
3. Bodies pin to an epoch when first anchored; generated-id addressing is never versioned
4. Old stage versions stay runnable: versioned C++ registry, vendored Luau, bake migration to retire
5. Terrain edits are versioned fixed-point operations, compacted into quantized density deltas with a base hash
6. Edits on generated objects are override records keyed by generated id
7. Scale is immutable for a universe once anything is anchored
8. Save layout: a directory of canonical JSON plus binary region files (toolbox)
9. Persistent entities, persistent ids and entity references (engine)
10. Data-format migrations, separate from generator versions (engine)
11. Mod identity: id, version, content hash, roles and deterministic load order
12. Stage output hashes in CI on both platforms (joint with Q12)

## 11. Spike candidates

Each is the smallest throwaway prototype that tests one decision, sized by scope.

- **S1: pinning survives a generator change.** A toy terrain stage with two versions (v1 and v2 differ visibly), two
  toy bodies, one anchored by a single dig at v1. Switch the default to v2 and reload. Success: side-by-side
  screenshots show the anchored body identical (FLIP 0 against the pre-update screenshot) and its `base_hash`
  matching, the other body changed; a test that edits v1's output without bumping its version fails. Scope: a
  stage registry with versions, an epoch table, one pin record, one hash. No real terrain needed.
- **S2: what heavy edits cost.** A headless Luau script drives a scripted "heavy base" session on a toy density chunk
  grid (a 100 m pad, tunnels, 10,000 random digs, a simulated eruption op) at two voxel sizes. Success: a table of
  op-log size, compacted-delta size raw and zstd, and Tracy-measured load time per chunk on the M5 Max; reloading
  gives the same mesh hash. Scope: ops in fixed point, a narrow-band delta, zstd. Depends on Q4 for the
  representation; can run on a placeholder.
- **S3: reflection can carry a save.** Persistent entities with int64 ids as strings, an array field and an entity
  reference; save → load → save is byte-identical; a committed golden save with a renamed component loads through
  a JSON migration recorded in `migrations.json`. Success: CTest passes on macOS and Windows CI. Scope: engine
  reflection walker extensions and a canonical writer.
- **S4: identity and mod removal.** Two lab processes (or `veach` CLI runs) exchange manifests; a mismatched Luau
  mod is refused with an error naming the mod and both hashes; removing a Luau generation mod appends an epoch and a
  pinned body still loads from the vendored copy. Success: the error text and a screenshot of the unchanged pinned
  body. Scope: manifest, fingerprint, vendoring.

## 12. Product decisions surfaced

- **P1. What players can rely on after an update.** Options: (a) places where anything was built or recorded never
  change; (b) every place anyone visited never changes; (c) only edits survive and the world may refresh under
  them. **Recommend (a)** for terrain and body physics, with flora, fauna and dressing refreshable by default and
  creators able to reclass their own stages.
- **P2. Can a universe's scale change after creation?** **Recommend no** once anything is anchored; offer a fork
  instead.
- **P3. Mods in multiplayer.** **Recommend** Luau mods downloaded automatically from the server (sandboxed); C++
  modules installed by the player; presentation mods free per client.
- **P4. Removing a generation mod from a live universe.** **Recommend allowed** through a new epoch; pinned bodies
  keep it.
- **P5. How far back saves are supported.** **Recommend** that frozen and pinned stage versions are supported until a
  release ships a bake migration for them, and that a bake migration is announced one release ahead.
- **P6. One format for single player and servers.** **Recommend yes.**

## Sources

Shipped games and tools:

- No Man's Sky Worlds Part I notes: https://www.nomanssky.com/worlds-part-I-update/
- No Man's Sky 5.5 (Worlds Part II) notes: https://www.shacknews.com/article/142889/no-mans-sky-update-55-patch-notes
- No Man's Sky Origins: https://nomanssky.com/origins-update/
- No Man's Sky Foundation: https://www.nomanssky.com/foundation-update/
- Elite Dangerous Odyssey planets: https://www.pcgamer.com/heres-how-frontier-rebuilt-a-galaxys-worth-of-planets-for-elite-dangerous-odyssey,
  https://pcgamer.com/elite-dangerous-odysseys-new-worlds-wont-exist-outside-the-expansion,
  https://forums.frontier.co.uk/goto/post?id=8957363
- Minecraft 1.18 blending: https://www.minecraft.net/en-us/article/minecraft-1-18-pre-release-1,
  https://www.windowscentral.com/minecraft-java-edition-21w43a-snapshot, https://bugs.mojang.com/browse/MC-243089
- Minecraft DataVersion upgrades: https://gist.github.com/aikar/90cb65d9b6a1bb761e65023ae10f43ca, https://wiki.vg/Map_Format
- Minecraft region file format: https://minecraft.fandom.com/wiki/Region_file_format
- Valheim and unexplored areas: https://www.pcgamer.com/games/survival-crafting/should-you-start-a-new-character-and-world-for-valheim-1-0/,
  https://thunderstore.io/c/valheim/p/JereKuusela/Upgrade_World/v/1.12.0/
- Factorio migrations: https://lua-api.factorio.com/latest/auxiliary/migrations.html
- Factorio multiplayer determinism and mod sync: https://wiki.factorio.com/Desynchronization
- Space Engineers save files: https://spaceengineerswiki.com/Save_file_sbc, https://spaceengineerswiki.com/Modding_Guide;
  planets: https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/

Libraries:

- zstd benchmarks and dictionaries: https://github.com/facebook/zstd
- Flecs v4 JSON serialization: docs/guides/flecs-v4/upstream/FlecsRemoteApi.md (in repo)

Repository: [ADR 0007](../adr/0007-scripting-luau.md), [0010](../adr/0010-asset-formats-and-identity.md),
[0018](../adr/0018-engine-lifecycle-and-phases.md), [0021](../adr/0021-git-lfs-for-binaries.md),
[0026](../adr/0026-reflection-everywhere.md), [0042](../adr/0042-space-toolbox-layering.md),
[record-replay spec](../specs/record-replay/spec.md), [VISION](../VISION.md), [00-scope](00-scope.md).

Unverified, and marked where used: early NMS updates resetting the universe; Space Engineers storing only modified
planet voxels; individual Minecraft 1.18 blending bug ids beyond MC-243089; all size estimates (spike S2 measures
them).
