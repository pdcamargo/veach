# Q11 raw notes: persistence and generator versioning

Research date 2026-11-27. Repository inputs read: notes/_brief.md, 00-scope.md (sections 8 and "Questions added or
split"), ADR 0007 (Luau, command buffers), 0010 (asset formats and identity), 0021 (Git LFS), 0026 (reflection
everywhere), 0042 (toolbox layering), specs/record-replay/spec.md, VISION "AI-native principles", Flecs v4 JSON
addon docs vendored in docs/guides/flecs-v4/upstream/FlecsRemoteApi.md. Web sources below. Items marked
**unverified** were not confirmed from a primary page.

## 1. How shipped games survive generator changes (versioning precedents)

### Takeaway
Every precedent that kept saves alive after changing procgen did it by layer: the layer players build on (terrain
shape, star positions) is frozen or pinned, layers nobody builds on (flora, fauna, dressing) are refreshed, and
genuinely new generation goes into new address space or unvisited regions. Nobody regenerates edited ground under a
player and keeps the edits meaningful.

### Cited findings
- NMS Worlds Part I (5.0, 2024): "Underlying terrain shapes have not been reset and existing planetary bases will not
  be moved", while the same notes say "Planets across the universe have been transformed in wild and radical ways"
  (floating islands, visuals). So the terrain-shape layer was frozen and the dressing/visual layers changed —
  [nomanssky.com Worlds Part I](https://www.nomanssky.com/worlds-part-I-update/)
- NMS Worlds Part II (5.5, 2025): new terrain settings "applied to all planets found within purple star systems"
  (a new class of systems), "Terrain in existing systems has not been regenerated, and bases have not been reset",
  added "without regenerating existing planets", aim to "minimise change to existing worlds" —
  [Shacknews 5.5 patch notes](https://www.shacknews.com/article/142889/no-mans-sky-update-55-patch-notes)
- NMS Origins (3.0, 2020): terrain of existing planets kept, bases unaffected, creatures reset, new planets added
  beside old ones — [nomanssky.com Origins](https://nomanssky.com/origins-update/) (from 00-scope-notes; Sean Murray
  quote "We didn't want to change the terrain of existing planets... introduce new planets" via TechRadar, page not
  fetched: **unverified wording**)
- NMS Foundation (1.1, 2016) changed generation (more lush planets, fewer lifeless ones, "altered per-planet terrain
  resource generation", fewer buildings, changed asteroid resources); the page does not say how existing saves were
  affected — [nomanssky.com Foundation](https://www.nomanssky.com/foundation-update/). Player claims that Foundation,
  NEXT or Worlds I "reset the universe" exist only as forum posts: **unverified**
  ([Steam thread](https://steamcommunity.com/app/275850/discussions/0/4526764179299288300)).
- Elite Dangerous Odyssey (2021) replaced the landable-planet surface generator (previously separate rocky and icy
  generators fed by Stellar Forge inputs) with new tech, only for landable bodies; Horizons players kept the old
  planets and Odyssey and Horizons players do not see each other on planets —
  [PC Gamer, rebuilt planets](https://www.pcgamer.com/heres-how-frontier-rebuilt-a-galaxys-worth-of-planets-for-elite-dangerous-odyssey),
  [PC Gamer, new worlds won't exist outside the expansion](https://pcgamer.com/elite-dangerous-odysseys-new-worlds-wont-exist-outside-the-expansion),
  [Kay Ross stream summary, Frontier forum](https://forums.frontier.co.uk/goto/post?id=8957363). Physical
  parameters (Stellar Forge) were kept; the surface stage was swapped. Elite has no player terrain edits, which
  made this cheap for them (inference).
- Minecraft stores every generated chunk absolutely (region files) and tags each chunk with `DataVersion`; upgrade
  code runs every converter between the stored and the current version, treating a missing tag as -1 —
  [aikar gist of decompiled upgrade code](https://gist.github.com/aikar/90cb65d9b6a1bb761e65023ae10f43ca),
  [wiki.vg Map Format](https://wiki.vg/Map_Format). Mojang's DataFixerUpper is the library for these stepwise
  conversions — [Modrinth DFU-related page](https://modrinth.com/project/oySfX6cZ) (secondary).
- Minecraft 1.18 (2021): new generation only in new chunks; biome blending (pre-release 1) and terrain blending
  (snapshot 21w43a) adjust newly generated chunks next to chunks saved by an earlier version to hide the seam —
  [minecraft.net 1.18 pre-release 1](https://www.minecraft.net/en-us/article/minecraft-1-18-pre-release-1),
  [Windows Central on 21w43a](https://www.windowscentral.com/minecraft-java-edition-21w43a-snapshot). Bug reports
  show it is imperfect: hard borders after upgrading, deleted old/blended chunks don't regenerate well, nether
  blending issues ([MC-243089](https://bugs.mojang.com/browse/MC-243089); other ids from search, individually
  **unverified**).
- Valheim: zones are generated on first visit and saved; after a biome update, "biome generation will only work
  properly on areas that have not yet been explored" (Iron Gate FAQ quoted by
  [PC Gamer](https://www.pcgamer.com/games/survival-crafting/should-you-start-a-new-character-and-world-for-valheim-1-0/)).
  Players report Mistlands on old seeds generated poorly; the community "Upgrade World" mod regenerates zones by
  biome, including explored ones — [Thunderstore Upgrade World](https://thunderstore.io/c/valheim/p/JereKuusela/Upgrade_World/v/1.12.0/).
  The ~500 m "explored" radius is a forum guess: **unverified**.
- Factorio migrations: JSON migrations rename prototypes (references in `storage` stay valid), Lua migrations alter
  loaded state; they run sorted by mod order then file name, JSON before Lua; "each save records which migrations
  have been applied" and never applies one twice; adding a mod to a save runs all its migrations; removed prototypes
  invalidate references; un-migratable ghosts are deleted —
  [Factorio auxiliary docs: Migrations](https://lua-api.factorio.com/latest/auxiliary/migrations.html).

### Inferences
- Two axes must be versioned separately: **data format** (Minecraft DataVersion, Factorio migrations: the shape of
  saved records) and **generator output** (NMS, Elite, Valheim: what the seed produces). Minecraft can mix them because
  it stores generated chunks; a seed-regenerating universe cannot.
- "Store every generated chunk" (Minecraft, Valheim) is the simplest stability guarantee, and it does not scale to
  planets you fly over from orbit (millions of LOD chunks). It is usable only for edited chunks.
- Pinning per region needs seam handling (Minecraft blending, Valheim's poor Mistlands); pinning per body avoids
  seams because bodies are separate objects.

### Gaps
- No primary source found for what NMS stores per base (whether it stores the terrain delta relative to the
  generated surface or absolute). Its base terrain edits survived terrain-freezing updates, which is consistent with
  either.
- No primary source on Starbound's per-planet world files, Astroneer's or Enshrouded's edit storage; not used.

## 2. Universe and mod identity, multiplayer agreement

### Takeaway
Deterministic games that regenerate or simulate in lockstep require exact agreement on game version, mods, mod
file contents and startup settings, and they let clients fetch mods from the server. Veach's universe identity must
therefore include mod content hashes, not only names and versions.

### Cited findings
- Factorio: all instances must use "exactly the same game-versions and mods"; clients can "Sync mods with server";
  desyncs are detected by CRC of game state at a tick and are usually caused by mods; mod startup settings must also
  match — [Factorio wiki: Desynchronization](https://wiki.factorio.com/Desynchronization), forum threads
  ([1](https://forums.factorio.com/viewtopic.php?p=451927), [2](https://forums.factorio.com/101654)).
- A Factorio mod author added a prototype checksum so differences are detected before connecting (forum report;
  whether core Factorio does this today **unverified**) — [forum](https://forums.factorio.com/viewtopic.php?p=451921).
- Record/replay in Veach already refuses a different seed, script, engine version or fixed step (`replay.invalid`)
  and hashes Scriptable components per frame (FNV-1a) — repo `docs/specs/record-replay/spec.md`.

### Inferences
- The fingerprint must cover: seed, scale, creator settings, toolbox and determinism-contract versions, and every
  generation- or simulation-affecting mod by content hash. Cosmetic client-only mods must be excluded or no two
  players could ever join each other.
- Luau mods are text and small, so a server can ship them; C++ modules cannot be shipped safely.

### Gaps
- No primary source on how NMS validates mods in multiplayer (it officially doesn't support mods): not used.

## 3. Formats: text vs binary, containers, compression

### Takeaway
Shipped voxel games use a hybrid: structured text or tagged structured data for the world description and objects,
and a binary container for bulk voxels. zstd dominates zlib at the same ratio; LZ4 is faster but compresses less.

### Cited findings
- Space Engineers world folder: `Sandbox.sbc` (world description, XML), `SANDBOX_0_0_0_.sbs` (positions and states of
  objects, XML), `.vx2` (proprietary octree voxel storage; old `.vox` converted on first load); a `Storage` folder
  for mod data; the game deletes unrecognized files in the world folder on save —
  [SE wiki: Save file sbc](https://spaceengineerswiki.com/Save_file_sbc),
  [SE wiki: Modding Guide](https://spaceengineerswiki.com/Modding_Guide),
  [xgamingserver import guide](https://xgamingserver.com/docs/space-engineers/import-save). (Exact page for each
  claim approximate; no byte-level spec of .vx2 found.)
- Space Engineers planets are volumetric, destructible and persistent —
  [Marek Rosa blog](https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/). That only modified
  voxel data is stored for planets: **unverified**.
- Minecraft Anvil region files: 8 KiB header (4 KiB of chunk offsets, 4 KiB of timestamps) for 32×32 chunks, data in
  4 KiB sectors, each chunk compressed individually, zlib by default, padding to a 4096 B multiple —
  [Minecraft wiki: Region file format](https://minecraft.fandom.com/wiki/Region_file_format). Whether newer
  versions offer LZ4: **unverified**.
- zstd README (Silesia corpus, i7-9700K): zstd 1.5.7 -1 ratio 2.896 at 510 MB/s compress / 1550 MB/s decompress;
  zlib 1.3.1 -1 2.743 at 105 / 390 MB/s; lz4 1.10.0 2.101 at 675 / 3850 MB/s; brotli -1 2.883 at 290 / 425 MB/s.
  Trained dictionaries substantially improve ratio for many small (~1 KB) records —
  [facebook/zstd](https://github.com/facebook/zstd).
- Flecs v4 JSON addon serializes the whole world, entities and queries to JSON and back
  (`world.to_json()`, `e.to_json()`) — repo `docs/guides/flecs-v4/upstream/FlecsRemoteApi.md`. It dumps
  everything, including derived components, so a save needs a filtered walker.
- Veach today: JSON with generated schemas and reflection-driven reading (ADR 0010, 0026); arrays, vectors, maps and
  non-string opaque types are "unsupported" and skipped by JSON and Luau (ADR 0026); JSON numbers lose int64 above
  2^53 (00-scope finding 5); binaries go to Git LFS (ADR 0021).

### Inferences
- A save directory (SE, Minecraft) beats a single opaque file for Veach: diffable, inspectable, partial writes, and
  agent-readable. Pack it for transport only.
- Per-chunk compression with zstd in a region container (Anvil's shape, modern codec) is the obvious binary layer.

### Gaps
- No measured numbers for density-delta sizes in any shipped game; Veach must measure its own (spike).

## 4. Edit representations (derived, not sourced)

### Takeaway
Three ways to store terrain edits: an operation log (tiny, keeps intent, replay cost grows), a sparse voxel delta
(bounded replay cost, size scales with edited surface area), or absolute frozen chunks (robust to generator change
for that chunk, largest, seams with neighbors). A log compacted into a quantized delta gets most of both.

### Inferences (Veach-specific estimates, unmeasured)
- One brush op as a record (kind, version, tick, author, chunk-local position as 3×int32 fixed point, radius,
  strength, material) is roughly 32-48 bytes binary.
- A "heavy base" excavation, 100 m × 100 m × 5 m: edited surface area ≈ 10,000 m² floor + 2,000 m² walls =
  12,000 m². At 0.25 m voxels with a narrow band of ~4 voxels on each side and 3 bytes per voxel (int16 density +
  material), about 12,000 × 16 × 4 × 3 ≈ 2.3 MB raw; smooth fields compress several-fold with zstd, so well under
  1 MB. The same edit done with a flatten tool is hundreds to a few thousand ops, i.e. tens of KB. Interior
  solid→air runs are constant and compress to almost nothing.
- Continuous mining at 10 ops/s for 100 hours is 3.6 M ops ≈ 115-170 MB raw: too long to replay at load, so the log
  must be compacted per chunk.
- Applying ops in fixed point on quantized density is bit-exact on any CPU (integer math), which Q12 needs if the
  server and clients both apply edits.

## Sources
See inline links above; repo files cited by path.
