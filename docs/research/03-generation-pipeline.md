# 03: The universe generation pipeline

How a seed becomes a galaxy, star systems, bodies, climates, biomes, life and objects, in a way that is physically
coherent, queryable without generating everything, extensible from Luau and C++ modules, and gives the same universe
at any scale s. Respects the (a) items [00-scope.md](00-scope.md) assigns to Q3. Generator versioning belongs to
**Q11** and numeric determinism to **Q12**. This document covers only what the pipeline design depends on from them
and points to them for the rest. Raw notes and every source: [notes/03-generation-pipeline-notes.md](notes/03-generation-pipeline-notes.md).

Marking: **(unverified)** means a value or formula from standard references that I did not fetch;
**(community)** means reverse-engineered by players, not published by the developer.

## 1. The problem in Veach's terms

Everything in a Veach universe must derive from one universe seed plus the generator manifest (versions and mods,
Q11). The pipeline has to deliver six things at once:

1. **Addressing.** Every generated thing (system, star, planet, moon, ring, belt, biome patch, chunk, tree, rock,
   creature spawn) needs a stable id. The id is both how you name it (saves, network, MCP, discoveries) and how you
   seed it. Ids go through JSON, Luau and MCP. Today Luau marshals `u64` fields through a `double`
   (`engine/script/src/marshal.cpp:48`), so a 64-bit id silently loses bits above 2^53 in scripts.
2. **Coherence.** A planet's look follows from its physics: mass, composition and orbit set gravity, temperature,
   which gases it keeps, how much greenhouse warming it gets, its climate, its biomes and its life. This is a staged
   pipeline, not independent noise layers. Several rules are non-local (snow line, orbit spacing, Roche limit, Hill
   sphere, drainage, plates), so some stages need a whole system or a whole body as context.
3. **Queryability.** From anywhere in the galaxy, the sky must show the stars, nebulae, clusters, the galactic center
   and the background quasars that are really there (Q8). FTL targeting needs "nearest systems" (Q10). None of this
   may require generating full systems.
4. **Extensibility.** Body kinds, star kinds, biomes, deep-sky objects, life and placed objects are registered data
   plus generators, from Luau or C++ modules ([ADR 0019](../adr/0019-veach-modules-and-services.md),
   [ADR 0042](../adr/0042-space-toolbox-layering.md)), never hardcoded enums. A mod must not reshuffle what other
   content generates.
5. **Scale.** The same seed gives the same planet (composition, climate, biomes, life) at s = 1 and at s = 0.01. Only
   geometry changes. The pipeline must say exactly where s enters.
6. **Inspectability and cost.** Headless and in CI, any property of any address can be computed and hashed. MCP can
   query it. At runtime, record stages run rarely and cheaply on the CPU, and chunk-level generation runs within a
   frame budget (Q4/Q5), on a base M1-class GPU as well as on an M3 Pro-class one.

What Veach already has: `veach::Rng` (PCG32 with plain, reflectable state: 24-bit `next_float`, modulo-biased
`range`), the `veach::Random` singleton seeded from `--seed` for gameplay simulation, reflection that drives JSON,
schemas, Luau and MCP ([ADR 0026](../adr/0026-reflection-everywhere.md)), 128-bit asset UUIDs
([ADR 0010](../adr/0010-asset-formats-and-identity.md)), and per-world services. What it lacks: a stateless hash for
deriving seeds, 53-bit doubles from the RNG, an opaque 64-bit id type in Luau, array support in the reflection walker
(an orbit tree is a list), a job system, and a way for toolbox modules to add MCP tools.

## 2. Options, with precedents

### 2.1 Addressing

- **A. Flat random ids (UUIDs) stored in a database.** What assets use. It needs storage for everything generated,
  which a procedural universe can't afford. No precedent at universe scale.
- **B. Fixed-width hierarchical address (NMS).** 48 bits as `P-SSS-YY-ZZZ-XXX`: a 4-bit planet index, a 12-bit
  system index and a 32-bit region voxel, plus a separate galaxy byte. The 12 portal glyphs are the same nibbles
  ([nms-core, community](https://docs.rs/nms-core/0.1.0/src/nms_core/address.rs.html)). It is simple, and the
  address is shareable as glyphs. The budget is fixed, though: 16 bodies per system at most, and special meaning
  squatting on index values (0x79 is always a black hole, [community](https://nomanssky.fandom.com/wiki/Galactic_Hub_Naming_Guidelines)).
- **C. Layered octree address (Elite Stellar Forge).** One 64-bit integer encodes sector coordinates, the octree
  layer (one of eight), the system id and the body id; client and server agree from it
  ([80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)). Players report that the layer is
  the mass code: heavier systems live in bigger boxels (10 ly at layer a up to 1280 ly at h), numbered from 0 within
  a boxel "until the boxel runs out of mass" ([community](https://forums.frontier.co.uk/threads/latest-updates-to-understanding-system-names-in-the-black.632624/latest)).
  The layering is what makes a sky query cheap (see 2.4).

### 2.2 Deriving seeds

- **A. One sequential RNG walked down the tree** (generate system 0, then 1...). Order-dependent: generating system 7
  means generating 0 to 6 first, and adding one draw anywhere shifts everything after it. No serious precedent uses
  this across siblings.
- **B. Hash the address (noise-based or counter-based RNG).** `seed(child) = H(seed(parent), child_index,
  stage_salt)`. Order-independent, parallel, random access. Precedents: Eiserloh's noise-based RNG, GDC 2017
  ([squirrel-rng](https://docs.rs/crate/squirrel-rng/0.4.0/source/src/lib.rs)); Philox/Threefry counter-based
  generators that run on CPUs and GPUs ([Random123](https://sources.debian.org/data/main/libr/librandom123/1.09%2Bdfsg-2/docs/html/index.html),
  [SC11 paper](https://users.cs.utah.edu/~hari/teaching/bigdata/random123sc11.pdf)); Minecraft's population and
  region seeds hashed from world seed and chunk coordinates ([Yarn docs](https://maven.fabricmc.net/docs/yarn-1.21.3+build.1/net/minecraft/util/math/random/ChunkRandom.html)).
- **C. Hybrid: hash to seed each record, then a sequential RNG inside one stage of one record.** Stage code reads
  naturally (`rng.next_double()` ten times) while siblings stay independent. Minecraft is close to this: hashed
  population seeds, then per-feature salts. Minecraft's salt is `index + 10000 * step` with `index` the position
  in the feature list, so inserting a feature reseeds every feature after it. That shows salts must come from stable
  names.

### 2.3 Pipeline shape

- **A. Independent noise layers** (heightmap noise, then biome noise, then color). Cheap and local, but
  incoherent: a desert can sit at the pole of an ice world. McKendrick says NMS "couldn't simulate erosion" because
  everything was generated locally, and approximated it instead
  ([GDC 2017 notes](https://blather-rinse-repeat.blogspot.com/2018/11/gdc-2017-part-ii-main-conference-notes.html)).
- **B. Full simulation** (accretion, N-body, climate GCM). Correct but costly, and hard to make deterministic across
  platforms (Q12).
- **C. Staged physical records with cached whole-context stages, then local evaluation (Stellar Forge).** Top-down:
  galaxy distributions, a star of consistent type and age, a protoplanetary disc stepped through epochs, then
  proto-planets tracked for tidal locking and heating. That gives per-planet physical ranges (tectonics, volcanism,
  craters, ice), averaged into a GPU buffer that modulates noise
  ([80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)). SpaceEngine's published climate
  model works the same way: equilibrium temperature first, then per planet type day/night, latitude and obliquity
  terms, stars combined as (Σ T⁴)^¼, then greenhouse and internal heat
  ([SpaceEngine](https://spaceengine.org/articles/the-climate-model/)). Star Citizen Planet Tech v4 picks biomes from
  temperature and humidity maps ([fan wiki](https://theimpound.com/blogs/star-citizen-wiki/planet-tech-v4)).

### 2.4 Galaxy queryability (the sky from anywhere)

- **A. Static skybox.** Incoherent the moment you travel. Ruled out by 00-scope.
- **B. Generate every system in range and draw them.** Doesn't bound: tens of thousands of cells to fill a sky.
- **C. Magnitude-layered octree plus a diffuse density integral.** Bright (massive) stars live in coarse cells and
  dim ones in fine cells, so "every star brighter than magnitude m from point p" is a fixed neighborhood of cells per
  layer. Unresolved starlight and dust come from integrating the analytic density along view rays. Elite's layered
  octree is this shape (community reading).
  Elite's skybox shows seams at its 1280 ly cubes because density is constant per cube, and the reported reason it
  can't be fixed is that changing the density would move discovered systems ([community](https://forums.frontier.co.uk/threads/borders-between-codex-regions-seem-to-also-be-borders-between-different-background-star-fields.616573)).

### 2.5 Where scale enters

- **A. Generate at the scaled size** (an s = 0.1 Earth is a smaller planet in the pipeline). Escape velocity drops
  by √s, so retention strips atmospheres, temperatures shift and the planet changes. That breaks the owner's
  requirement.
- **B. Generate in physical units, apply s only when realizing geometry.** The owner's idea. KSP is the
  gameplay precedent: a tenth of Earth's radius with full Earth gravity.

### 2.6 Extension

- **A. Enums plus `switch`**: closed, and every new kind is a core change.
- **B. Registered kinds with stable string ids, typed records and stage hooks.** NMS shipped gas giants, binary
  stars and new planets years after launch and kept old planets' terrain ([Origins](https://nomanssky.com/origins-update/),
  [Worlds Part II](https://news.xbox.com/en-us/2025/01/29/no-mans-sky-worlds-part-2-out-today/)). That only works
  if new content adds to the universe instead of reclassifying what exists.

## 3. Tradeoffs

| Choice | Complexity | Performance | Determinism | Extensibility | Testability | Scale range |
|---|---|---|---|---|---|---|
| Addressing: fixed NMS-style | Low | Trivial | Good | Poor: fixed budgets | Good | Neutral |
| Addressing: layered octree (Elite) | Medium: variable field widths per layer | Trivial | Good | Good if headroom is reserved | Good | Neutral (ids never encode scaled positions) |
| Seeds: sequential across siblings | Low | Bad: random access costs O(n) | Fragile: any added draw shifts the rest | Bad | Bad | Neutral |
| Seeds: hash per record + RNG inside a stage | Low | Good, parallel | Robust, given integer-only hashing (Q12) | Good: stages salted by name | Excellent: per-address hashes | Neutral |
| Pipeline: independent noise | Low | Best | Good | Poor: no physical inputs to extend | Medium | Breaks: nothing ties features to R |
| Pipeline: full simulation | Very high | Bad | Hard (Q12) | Medium | Hard | Neutral |
| Pipeline: staged records + cached whole-context stages | Medium | Good: record stages in µs to ms, coarse fields once per body | Good: integer seeds, CPU doubles, engine math (Q12) | Good: one hook per stage | Excellent: stage hashes | Good if the pipeline never reads s |
| Sky: magnitude layers + density integral | Medium | Bounded cells per query | Good | Good: deep-sky kinds add density terms | Good: star count and hash per query | Invariant (light-years) |
| Scale at realization | Low in the pipeline, design load in Q1/Q2/Q4/Q8 | Neutral | Same records at every s | Neutral | One CI test: hashes equal across s | The point of it |

## 4. Extension points

Rule: **record-level stages run rarely and are Luau-friendly; per-voxel and per-instance work is C++ or GPU, with
Luau supplying parameters and graphs (Q5).** Every hook gets a read-only view of its ancestors' records, its own
address, a stage RNG seeded from the address, and deterministic math from `@veach` (Q12). It returns a typed record
that is validated against the stage's reflected schema. Kinds and hooks run in order of their stable string id,
never load order, so the order mods load in can't change output.

| Extension point | Luau | C++ module | Toolbox core only |
|---|---|---|---|
| Galaxy kind (spiral, barred, elliptical, ring; authored density map as an asset) | parameters, catalog hooks | a new density model | the field interface |
| Deep-sky kind (nebula, cluster, SMBH, quasar, external galaxy, anything new) | record + placement generator | render feature (Q8) | the catalog schema |
| Star kind (giants, Wolf-Rayet, pulsars, exotic) | evolution and parameter hooks | render variant (Q8) | none |
| System formation | `post` hooks that add bodies in reserved index ranges; `override` a whole system for one address (authored systems) | a replacement formation model | the orbit-tree schema |
| Body kind (rocky, ocean, gas giant, ice giant, asteroid, comet, artificial, new) | classifier predicate + parameter generators | new surface representation (Q4) or render feature (Q8) | the classifier protocol |
| Derived physics refinements (per-gas Jeans escape, magnetic stripping, tidal heating models) | refinement hooks | faster versions | the base set: g, v_esc, T_eq, retention, greenhouse |
| Coarse body fields (new fields: soil, nutrients, radiation, minerals) | new field kinds at low resolution (measure first) | field generators in C++ or compute | grid layout and sampling API |
| Biome kind | rules (temperature, precipitation, altitude, solvent, light) + parameters | none | the blending scheme |
| Life and biochemistry | Luau | none | the life record schema |
| Placement layer (trees, rocks, resources, ruins, creature spawns) | sparse layers, per-kind parameters | dense layers (grass, debris) | the layer and object-index protocol |
| Authored overrides (a hand-made system or planet inside a procedural galaxy) | data | none | the override stage |
| Address layout, hash function, stage order, the "never read s" rule | none | none | core |

Mods that add content claim **new slots** (reserved body-index ranges, new placement layers, new deep-sky catalog
entries). Any change that reclassifies existing addresses is a stage version bump that Q11 handles. That is the NMS
Origins lesson: new planets went in next to the old ones.

## 5. Interactions with the other questions

- **Q1 coordinates.** Addresses never encode positions in meters or scaled units. The galaxy position of a system is
  (cell, offset within cell in light-years); Q1 turns that into the sector + local position. Body-local meters for
  edits come from Q1's body-fixed frame.
- **Q2 mechanics and time.** S1 outputs the orbit tree of barycenters with physical Kepler elements, the mean
  anomaly at epoch 0, spin period, obliquity and the tidal-lock flag. Q2 evaluates them on rails from the integer
  tick and decides how periods relate to s_sys. Q2 also owns gameplay gravity; S1/B1 only provide physical g.
- **Q4 terrain.** It consumes B2-B5 coarse fields and the two-band split (section 7.5). It owns chunk keys, density
  fields, water bodies and bodies without surfaces. The planetary band follows s_p; the detail band is in meters.
- **Q5 procgen graph.** The stages here are the coarse, cached graph; Q5 defines how C, O and hooks are expressed,
  what runs on the GPU, and how Luau authors nodes. B2-B4 can be Q5 graphs evaluated on a cube-map grid.
- **Q6 networking.** Records are regenerated on every peer, never sent. Only the manifest, deltas and simulation
  state cross the wire. Addresses go over JSON as strings.
- **Q7 simulation LOD.** B5 provides species pools and carrying capacities per biome, and B1/B2 provide volcanism
  and geology. Q7 runs populations, weather and eruptions on top. Eruptions write through the same delta path as
  players.
- **Q8 renderer.** Star records give blackbody color, luminosity and angular size for photometric lights
  ([ADR 0036](../adr/0036-lights-in-photometric-units.md)). Illuminance always uses physical distance. B1 gives the
  atmosphere record (composition, pressure, scale height) for the per-body sky. G1 gives the star query, the galaxy
  density volume and the deep-sky catalog for the starfield and nebulae. The magnitude limit is a visual knob.
- **Q9 physics.** Colliders come from chunks (Q4). Gravity per body comes from Q2/Q10's gameplay values.
- **Q10 travel.** `nearest_systems` and the star query serve FTL targeting. The FTL transition is the moment S1 and
  the new system's sky cubemap are generated.
- **Q11 persistence and versioning.** Every stage carries an id and a version. A universe is seed + manifest + scale
  settings. Edits are deltas keyed by the stable ids defined here (interface in 7.6). Q11 owns storage, migration and
  what happens when a stage version changes under existing edits.
- **Q12 determinism.** Record stages run on the CPU in doubles with engine-provided math, so they are authoritative.
  The address hash uses integer operations only, with a 32-bit-op variant for shaders, so it is bit-exact on Metal,
  DX12/WARP, ARM and x64. GPU-only outputs are visual-only. Stage hashes compared between macOS arm64 and Windows
  x64 CI are Q12's test, and this pipeline provides them.

## 6. M1-class vs M3 Pro-class implications

Nothing is measured yet; only the M5 Max can be measured (spike S3.2 and S3.3 produce the first numbers).

- **What must not change with hardware:** every record (G1, G2, S1, B1, B5) and every coarse field that gameplay
  reads (biome at a point, sea level, where resources are). If content varied by quality tier, two players on
  different Macs would see different universes. Quality tiers may change only visual-only outputs.
- **What can change:**
  - Coarse field resolution for visual use. For example 6×256² cells (~0.4 M cells, about 6 MB at 16 bytes a cell) on
    M1 vs 6×512² on M3 Pro. The gameplay copy stays at one fixed resolution on every tier.
  - Starfield limiting magnitude (number of point stars) and the resolution of the galaxy density volume and nebula
    volumes.
  - How many bodies keep their coarse fields cached (unified memory: a base M1 may have 8 GB shared with the GPU).
  - Chunk generation throughput (Q4/Q5). This is the real hardware-dependent cost, and it is a GPU compute budget,
    not a pipeline-design issue.
- **CPU:** record stages are small (S1 is the largest: one system, tens of bodies). Coarse body fields are the
  heaviest CPU work and should run on a job system. A base M1 has 4 performance cores, an M3 Pro 5 or 6
  **(unverified core counts)**, so on M1 a body's fields take longer to arrive during approach. Q10's approach speed
  has to leave room for that: start coarse fields when the body enters the "near" set, not at the surface.

## 7. Recommendation

### 7.1 Addresses: Elite-style layered octree, 64-bit system addresses, typed sub-addresses

A `SystemAddress` is 64 bits: layer (3 bits), cell coordinates at that layer, system index within the cell, and body
index. The illustrative budget below is for the ADR to fix:

| Field | Bits at layer 0 | At layer L | Notes |
|---|---|---|---|
| layer | 3 | 3 | 8 layers; cell edge c₀·2^L, with c₀ = 10 ly gives 10 to 1280 ly |
| cell x, z | 14 + 14 | 14−L each | 163,840 ly span at layer 0 |
| cell y | 11 | 11−L | 20,480 ly thick: disk, bulge and some halo |
| system index | 13 | 13+3L | 8,192 systems per 10 ly cell covers dense regions **(density unverified)** |
| body index | 9 | 9 | 512 bodies incl. moons, rings and belts; 0 = primary; 256 and up reserved for mods |

Below the system level, typed addresses compose: `BodyAddress = SystemAddress` with its body index set;
`SurfaceKey` = 64 bits (cube face 3, level 5, quadtree u and v 28 each, giving ~4 cm on an Earth-size cube face at
s = 1). Q4 may need a depth coordinate for caves. `ObjectAddress = (BodyAddress, ChunkKey, layer id, index)`,
where layer id is a 32-bit hash of the layer's stable string id. The full object address is 192 to 256 bits, which
is fine as a struct.

- **Galaxy distances do not follow s_p or s_sys** (00-scope recommends a separate s_gal, default 1). The address is
  in light-year cells and never moves with any s.
- **In Luau and MCP**, addresses are an opaque tagged userdata, like entity ids already are, with a canonical string
  form (`"S:3f2a…/B:4/C:…/L:trees/I:117"`). They are never numbers. JSON carries them as strings. A short
  checksummed share code (NMS glyphs) is a later UI feature.
- **Why not 128 bits for systems:** 64 bits fits the budget with headroom, keys hash tables cheaply and matches the
  precedent. Galaxy size is then bounded by the bit layout; the ADR records the maximum.

### 7.2 Seeds: hash the address, a stage RNG inside the record

- `stage_seed = H64(universe_seed, address, stage_id_hash, stage_version)`, with H64 a strong 64-bit integer mixer
  (SplitMix64-style or Philox-style; the ADR picks one and pins test vectors). Integer operations only, so it is
  bit-exact everywhere (Q12).
- Inside one stage of one record, a `veach::Rng` seeded from `stage_seed` with the stage's stream. The toolbox needs
  `next_double()` (53 bits) and an unbiased `range` added to the engine's RNG. Those are generic, so they go into
  `engine/core` ([ADR 0042](../adr/0042-space-toolbox-layering.md)).
- Salts are hashes of stable string ids (`"space.system.formation"`, `"layer:trees"`), never list positions (the
  Minecraft lesson).
- Each sub-decision draws from its own sub-stream (`planet_masses`, `orbits`, `spins`), so adding a draw to spin
  generation doesn't change orbits.
- The universe seed is data in the universe manifest. It is not `--seed`. `veach::Random` stays the gameplay
  simulation RNG; the lab may default one from the other.
- Shaders get a 32-bit-op hash (PCG hash or Philox4x32) keyed by the 64-bit chunk key split into two u32. 64-bit
  integer shader ops are optional on DX12 and not worth depending on.

### 7.3 The stages

All stages take and produce **physical, unscaled values**: SI units, plus light-years for galaxy positions.
Records are reflected structs ([ADR 0026](../adr/0026-reflection-everywhere.md)), so JSON dumps, schemas, Luau
access and MCP come free. Lists inside records (orbit trees) need array support in the reflection walker, which is
generic and goes in the engine.

| Stage | Context it needs | Main outputs | Cached |
|---|---|---|---|
| **G0 Universe** | seed, manifest | galaxy kind and parameters | always |
| **G1 Galaxy** | whole galaxy | analytic density, age and metallicity fields (disk, bulge, bar, arms, halo; optional authored density map asset, as Elite does); dust field; deep-sky catalog: central SMBH, globular clusters (halo, old, metal-poor), open clusters and H II regions (arms, young), dark nebulae (dust lanes); background catalog at infinity (quasars, external galaxies: direction, flux, color) | forever; small |
| **G2 Cell** | G1 + cell address; stateless | star stubs: position in cell, primary mass (from the IMF, Kroupa 2001 **(unverified)**, restricted to the layer's brightness band), age, metallicity, multiplicity flag, luminosity, T_eff | LRU; µs to regenerate |
| **S1 System** | whole system | evolved stars (main sequence, giant, remnant from mass and age); binary orbits; disc mass from metallicity; snow line; habitable zone (Kopparapu 2013/2014 flux limits); stable zones (S/P-type limits); bodies placed with ≥ ~10 mutual Hill radii spacing **(unverified threshold)**; giants beyond the snow line more often at high metallicity; moons within ~½ Hill radius; rings inside the Roche limit, moons outside; belts; comets; per body: mass, bulk composition, Kepler elements, spin, obliquity, tidal lock from the despinning time vs age | per visited or targeted system |
| **B1 Body physics** | its system | radius from mass and composition; density, g, v_esc; T_eq per star with albedo and redistribution factor, combined as (ΣT⁴)^¼ (SpaceEngine's method); retention (cosmic shoreline by default, per-gas Jeans as a hook); atmosphere composition, pressure, scale height; greenhouse; mean surface temperature; internal heat (radiogenic + tidal); volcanism; magnetic field slot; hydrosphere fraction and solvent; **body kind** by classifier (SpaceEngine's temperature × volatiles × mass × composition classes are a good default rule set) | with its system |
| **B2 Planetary topography** | whole body, coarse | cube-map fields in dimensionless units (elevation as a fraction of R): plates, continents, basins, ranges, ocean mask, drainage network and major rivers | per near body |
| **B3 Climate** | whole body, B1 + B2 | seasonal temperature and precipitation (energy balance per latitude and season, SpaceEngine-style terms; moisture from oceans; rain shadows from B2), ice cover | per near body |
| **B4 Biomes** | B2 + B3 | biome id and blend weights per cell, from registered rules (Whittaker-style temperature × precipitation, plus altitude, solvent, light) | per near body |
| **B5 Life** | B1 + B4 | biosphere record (present or not, complexity, biochemistry); per biome: species pool (ids seeded from body, biome, index), carrying capacities | per body |
| **C Chunk detail** | sampled B1-B5 + chunk key; stateless | density, materials and water (Q4); placement candidates | Q4 streaming cache |
| **O Objects** | C + placement layers; stateless | instances: kind id, body-local transform in meters, object seed | streaming |

Rules:

- **A stage reads only its ancestors and its own address.** Anything that needs siblings (spacing, Roche, shadows on
  rings, drainage) is lifted into the parent's whole-context stage. Elite works this way: a body "links to its parent
  and can access its siblings" through the parent ([80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)).
- **Chunk and object stages are pure functions** of (address, cached ancestor records, coarse fields at the
  point). That is what makes them streamable, GPU-friendly and regenerable on every peer.
- **Deep-sky objects live where their physics puts them.** Galaxy-scale ones go in G1. System-attached ones go in S1:
  a planetary nebula around a young white dwarf, a remnant around a young neutron star. Clusters are density terms
  in G1 that G2 samples, so the stars you reach are the cluster you saw. Nebulae carry a seed for a low-resolution
  density volume, generated on demand, so the far impostor and the fog you fly through are the same field (Q8,
  VCH-072).
- **No per-cell constants.** G2 places candidates at a majorant density and keeps each with probability ρ(x)/ρ_max
  at its own position (thinning). Density is then continuous across cells, with no "Great Cubes".

### 7.4 The galaxy query service

A toolbox service ([ADR 0019](../adr/0019-veach-modules-and-services.md)), `space::Universe`, memoizes stages and
exposes:

- `galaxy()`, `cell(addr)`, `system(addr)`, `body(addr)`: reflected records.
- `field(body, field_id)`: an async handle to a coarse field, built on a job system.
- `stars_visible(pos_ly, limiting_magnitude)`: for each layer, the cells within that layer's visibility radius.
  Each star sits in the layer whose cell size matches how far it can be seen: d = 10^((m_lim − M + 5)/5) pc. With
  m_lim = 6.5, a Sun-like star (M ≈ 4.8) is visible to ~70 ly, an O star (M ≈ −6) to ~10,000 ly, and an M dwarf to
  under 3 ly **(standard formula; magnitudes unverified)**. Per layer the query visits a fixed number of cells, so
  its cost is bounded wherever you are. Add dust extinction from G1.
- `nearest_systems(pos_ly, n, filter)` for FTL targeting and maps.
- `stage_hash(addr, stage)`: a 64-bit hash of the record's canonical serialization.

The sky is a cubemap baked per system on arrival (the galaxy is static and in-system parallax is negligible):
point stars from `stars_visible`, a diffuse band from ray-marching G1's density and dust, deep-sky impostors, and
the background catalog. It is rebaked only on FTL.

Inspection:

- MCP: `universe_query {address, stage}` returns the record JSON, and `universe_hash` returns the stage hash. That
  needs modules to be able to register MCP tools. This is a generic engine feature, so it goes in the engine. Until
  then, the `@veach` space API through `eval` covers it.
- CLI: a `universe` subcommand in a tool that links `veach::space` dumps any address at any stage, headless.

### 7.5 Scale: the pipeline never reads s

- **No stage's context contains s.** s_p, s_sys and s_gal exist only in the realization layer, the code that turns
  records into entities, geometry and render parameters. Q1, Q2, Q4 and Q8 own that layer. This is mechanical:
  the stage context type has no scale field, and a CI test asserts that G1 through B5 hashes are identical at
  s ∈ {1, 0.1, 0.01}.
- **Realized from records:** R = s_p·R_phys; atmosphere height and moon orbits × s_p; planet orbits and star radii
  × s_sys; galaxy positions × s_gal (default 1). Coarse fields are indexed by direction on the unit sphere, so they
  are invariant by construction. Physical g, v_esc, temperature, pressure and composition stay in the records; the
  gameplay values a creator picks (Q2/Q10) are realization settings. Illuminance always uses physical distance.
- **Two terrain bands (interface to Q4/Q5).** The planetary band (B2's elevation in units of R, plus planetary
  octaves) is realized × R_realized × a relief factor, so it follows s_p and keeps its slopes. The detail band is
  defined in meters and never scales. The split is at a realized wavelength λ_c in meters (a creator setting): any
  planetary octave whose realized wavelength falls below λ_c is dropped and the detail band takes over. Continents,
  ranges and basins are the same at every s; at small s, fewer planetary octaves survive above meter-scale detail.
- **What never scales:** players, creatures, plants, buildings, props, caves, edit brushes, object density per m²,
  river channel minimum width, chunk sizes in meters.
- **Consequence to state plainly:** same seed, same planet at any s means the same records and coarse fields. It
  does not mean the same individual trees: surface area goes as s_p², so object counts do too, and individual
  object addresses depend on s. That is why s belongs in the universe manifest (Q11), fixed once a universe has
  edits.
- **Edge to resolve with Q4:** river width. Topology comes from B2 and follows s_p, but channel width should come
  from the realized catchment area, with a floor in meters, or rivers on an s = 0.01 Earth come out absurdly wide.

### 7.6 Edits on top of generated content (interface; Q11 owns the rest)

The pipeline promises stable ids and a pure `generate(address, stage)`. The persistence layer applies overlays:

- **Authored overrides** (creator data replacing a record for one address) feed downstream stages and are part of
  the manifest.
- **Annotations** (player names, discoveries) attach to addresses and never feed generation.
- **Terrain deltas** are keyed by (BodyAddress, ChunkKey, detail-stage version), in body-local meters.
- **Object deltas** are keyed by ObjectAddress (removed, harvested, modified). Placement layers must therefore give
  objects stable indices within a chunk, versioned per layer.
- **System edits** (eruptions, erosion events from Q7) go through the same delta path, deterministically.

### What would change my mind

- If S3.2 shows S1 + B1 can't generate a system fast enough to stay hidden behind an FTL transition on M1-class CPUs,
  I would simplify formation (no epoch stepping, direct sampling of a calibrated distribution) before giving up
  physical coherence.
- If S3.1 shows the magnitude-layered query has visible popping or cost cliffs, I'd switch to a precomputed
  bright-star catalog per coarse region. It would be cached derived data, so a format change costs nothing.
- If the owner wants a real Milky Way with catalog stars (c in 00-scope today), the G2 stage gains a catalog source
  ahead of procedural fill. The address design already allows it (Elite does exactly this).
- If Q12 finds CPU doubles in record stages can't be made bit-exact across ARM and x64 with engine math, record
  stages move to fixed-point for whatever gameplay depends on (classification, placement), and doubles stay for
  visuals.

## 8. What is expensive to undo

1. **The address layout.** It ends up in saves, the network, discoveries, share codes and every delta. Changing it
   later needs a migration of every stored id.
2. **The hash function, the salts and the stage seed formula.** Changing any of them regenerates everything. Pin
   test vectors from day one.
3. **The galaxy density function.** Elite's lesson: once systems are discovered, density can't change without moving
   them.
4. **Stage boundaries and what lives in which record.** They set the granularity of versioning (Q11). Too coarse,
   and a small change resets too much (NMS Origins reset all creatures). Too fine, and the manifest explodes.
5. **Classification rules once players have discovered planets.** Reclassifying is visible: a discovered "temperate
   ocean world" turning into a desert. New kinds must claim new slots.
6. **"The pipeline never reads s."** If a stage starts reading s, every universe depends on its scale and the core
   promise is gone. The CI hash test keeps it honest.
7. **Physical units in records.** Retrofitting gameplay values into records mixes two meanings into one field.

## 9. Proposed ADR titles

1. Universe addresses: a layered-octree 64-bit system address with typed sub-addresses
2. Generation seeds are hashed from addresses; sequential RNG only inside one stage
3. Generation is a staged pipeline of typed physical records that never reads scale
4. Generators, kinds and stage hooks are registered by stable id and run in id order
5. The galaxy: a continuous density field, magnitude-layered stars and a deep-sky catalog
6. The universe query service: memoized stages, stage hashes, MCP and CLI inspection
7. Terrain splits into a planetary band that follows s_p and a detail band in meters (joint with Q4/Q5)

Engine-level ADRs or specs this needs (generic, so they move down): a stateless hash and a 53-bit double in
`engine/core`; opaque 64-bit ids in Luau and as strings in JSON (with Q1/Q11); array support in the reflection
walker; MCP tool registration from modules; a job system (likely shared with Q4/Q5).

## 10. Spike candidates

Each spike is a throwaway prototype, sized by scope, with a visible success criterion. Order: S3.2 first (it fixes
the record shapes), then S3.1 and S3.3, then S3.4.

- **S3.1 Sky from anywhere.** Scope: G1 analytic density with arms and bulge; G2 thinning sampler over 8 magnitude
  layers; a cubemap of point stars plus a ray-marched diffuse band; three observer positions (Sun-like radius in the
  disk, near the bulge, above the disk). Success: screenshots show the band and bulge where they belong, a spiral from
  above, and no cell seams; the printed star count per query stays bounded across positions; Tracy shows the query
  and bake cost on the M5 Max.
- **S3.2 System statistics.** Scope: S1 + B1 in C++, headless, no rendering. Generate 100,000 systems. Success:
  histogram images of star class, planet count, mass and period, and HZ planets per system, next to known
  exoplanet-population shapes; zero violations of Hill spacing, Roche and binary stability; G2-B1 stage hashes equal
  across s ∈ {1, 0.1, 0.01} and across two thread counts; ms per system measured.
- **S3.3 One Earth-like body's coarse fields.** Scope: B2-B4 on a cube map at 6×128², 6×256² and 6×512²;
  equirectangular images of elevation, temperature, precipitation and biomes; the same body at obliquity 0°, 23° and
  60° side by side. Success: deserts near ±30°, rain shadows behind ranges, ice caps growing with obliquity (judged
  by eye from the images); ms per resolution measured.
- **S3.4 A Luau mod adds a kind.** Scope: kind registry, one classifier and a `post` hook in Luau, adding a "crystal
  world" body kind in reserved slots. Success: two mods loaded in either order give identical hash tables; systems the
  mod doesn't touch keep their hashes; changed hashes appear only in reserved body indices.

## 11. Product decisions surfaced

1. **How common interesting worlds are.** Physically faithful generation makes temperate, life-bearing worlds rare
   (Elite). NMS makes them common. *Recommendation:* the pipeline stays physical, plus a creator "habitability bias"
   that biases sampling (for example, drawing an orbit toward the habitable zone) without breaking consistency.
   Default generous (NMS-like), 0 for realism.
2. **Default galaxy size.** Cost doesn't grow with size (everything is lazy), but discoverability does.
   *Recommendation:* Milky Way-sized by default, size a creator setting within the address limit.
3. **Scale is fixed per universe.** *Recommendation:* s is chosen at creation, stored in the manifest, and can't be
   changed once a universe has edits, because individual objects and edit positions depend on it.
4. **Relief at compressed scales.** Preserving slopes makes an s = 0.01 Everest 88 m tall: true to physics, but tame.
   *Recommendation:* a relief factor as a creator setting, default 1, decided from side-by-side screenshots in S3.3
   or Q4's spike.
5. **Mods and multiplayer.** Two clients with different generator mods see different universes. *Recommendation:*
   multiplayer requires identical manifests, with the handshake telling the player what is missing (Q6/Q11).
6. **Shareable addresses** (NMS-style glyph codes). *Recommendation:* yes, later (class b). The address design
   already supports it.

## 12. Sources

Games and tools:

- Doc Ross on Stellar Forge (80.lv, 2018): https://80.lv/articles/generating-the-universe-in-elite-dangerous/
- Elite Dangerous explore page: https://www.elitedangerous.com/explore; Braben (Space.com): https://www.space.com/31366-elite-dangerous-stellar-forge-interview.html
- Elite ID64 and boxels (community): https://forums.frontier.co.uk/goto/post?id=9906921,
  https://forums.frontier.co.uk/threads/latest-updates-to-understanding-system-names-in-the-black.632624/latest
- Elite skybox cube seams (community): https://forums.frontier.co.uk/threads/borders-between-codex-regions-seem-to-also-be-borders-between-different-background-star-fields.616573,
  https://forums.frontier.co.uk/goto/post?id=7388504
- Kay Ross livestream summary: https://forums.frontier.co.uk/goto/post?id=8957363
- NMS address layout (community crate): https://docs.rs/nms-core/0.1.0/src/nms_core/address.rs.html; naming
  guidelines: https://nomanssky.fandom.com/wiki/Galactic_Hub_Naming_Guidelines
- Innes McKendrick, GDC 2017: https://gdcvault.com/play/1024265/Continuous-World-Generation-in-No; notes:
  https://blather-rinse-repeat.blogspot.com/2018/11/gdc-2017-part-ii-main-conference-notes.html
- Sean Murray, GDC 2017 (not transcribed here): https://gdcvault.com/play/1024514/Building-Worlds-Using
- NMS Origins: https://nomanssky.com/origins-update/; Worlds Part II: https://news.xbox.com/en-us/2025/01/29/no-mans-sky-worlds-part-2-out-today/
- SpaceEngine climate model: https://spaceengine.org/articles/the-climate-model/; planet classification:
  https://spaceengine.org/news/blog170924/; press release: https://gamespress.com/SPACEENGINE-RELEASES-CLIMATE-MODEL
- Star Citizen Planet Tech v4/v5 (fan wiki): https://theimpound.com/blogs/star-citizen-wiki/planet-tech-v4,
  https://theimpound.com/blogs/star-citizen-wiki/planet-tech-v5; https://twinfinite.net/news/star-citizen-microtech-planet-tech-v4/
- Minecraft ChunkRandom (Yarn): https://maven.fabricmc.net/docs/yarn-1.21.3+build.1/net/minecraft/util/math/random/ChunkRandom.html;
  salts gist: https://gist.github.com/hube12/368e7331e497b17e092e8ca4ba206b3c
- Space Engineers planets: https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/

Random numbers:

- Eiserloh, noise-based RNG (GDC 2017), SquirrelNoise5 port: https://docs.rs/crate/squirrel-rng/0.4.0/source/src/lib.rs
- Salmon et al., "Parallel Random Numbers: As Easy as 1, 2, 3" (SC11): https://users.cs.utah.edu/~hari/teaching/bigdata/random123sc11.pdf;
  Random123: https://sources.debian.org/data/main/libr/librandom123/1.09%2Bdfsg-2/docs/html/index.html

Science:

- Kopparapu et al. 2013: https://ui.adsabs.harvard.edu/abs/2013ApJ...765..131K/abstract; 2014 mass dependence:
  https://arxiv.org/abs/1404.5292
- Zahnle & Catling 2017, The Cosmic Shoreline: https://arxiv.org/abs/1702.03386
- Tidal despinning: https://arxiv.org/abs/1204.3923
- Not fetched, unverified: Kroupa 2001 IMF; Hayashi 1981 snow line; Chambers et al. 1996 Hill spacing; Holman &
  Wiegert 1999 binary stability; Domingos et al. 2006 moon stability; Fischer & Valenti 2005 metallicity; Zeng et al.
  and Chen & Kipping 2017 mass-radius; Whittaker and Holdridge biome schemes;
  https://en.wikipedia.org/wiki/Roche_limit, https://en.wikipedia.org/wiki/Atmospheric_escape,
  https://en.wikipedia.org/wiki/Planetary_equilibrium_temperature

Repository: [rng.hpp](../../engine/core/include/veach/core/rng.hpp),
[components.hpp](../../engine/ecs/include/veach/ecs/components.hpp), `engine/script/src/marshal.cpp`,
ADRs [0010](../adr/0010-asset-formats-and-identity.md), [0018](../adr/0018-engine-lifecycle-and-phases.md),
[0019](../adr/0019-veach-modules-and-services.md), [0026](../adr/0026-reflection-everywhere.md),
[0036](../adr/0036-lights-in-photometric-units.md), [0042](../adr/0042-space-toolbox-layering.md).
