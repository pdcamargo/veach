# Q3: Universe generation pipeline: raw notes

Raw research notes behind [03-generation-pipeline.md](../03-generation-pipeline.md). Collected 2026-11-27. Every
claim carries a link. Formulas and numbers taken from memory of standard references, without fetching a page, are
marked **(unverified)**. Community reverse-engineering (forums, fan wikis, fan crates) is marked **(community)**.

## Repo facts this question builds on

### Takeaway
Veach has one seeded RNG (PCG32) meant for gameplay simulation, reflection that sends u64 to Luau through a double,
per-world services, and a strict layering rule that sends generic needs (hashing, job system) down into the engine.

### Cited Findings
- `veach::Rng` is PCG32 with public plain-data state (`state`, `increment`), `reseed(seed, stream)`, `next_u32`,
  `next_float` (24 bits of mantissa: `(next_u32() >> 8) * 2^-24`) and `range(lo, hi)` with accepted modulo bias —
  [engine/core/include/veach/core/rng.hpp](../../../engine/core/include/veach/core/rng.hpp)
- `veach::Random` is a singleton component `{seed, Rng rng}` seeded from `--seed`, for simulation —
  [engine/ecs/include/veach/ecs/components.hpp](../../../engine/ecs/include/veach/ecs/components.hpp);
  [ADR 0018](../../adr/0018-engine-lifecycle-and-phases.md)
- Luau marshalling converts `EcsU64` fields to `double` (`static_cast<double>(load<std::uint64_t>(ptr))`) and back,
  so any 64-bit id above 2^53 loses bits in scripts — `engine/script/src/marshal.cpp` lines 48 and 64. Entity ids
  already use a tagged userdata (`lua_newuserdatataggedwithmetatable`, same file line 88), which is the precedent
  for an opaque address type.
- Assets have 128-bit UUID ids in `.meta` sidecars; references survive renames —
  [ADR 0010](../../adr/0010-asset-formats-and-identity.md)
- Modules are Flecs modules; engine machinery is reached through per-world `Services` (`provide<T>`, `get<T>`) —
  [ADR 0019](../../adr/0019-veach-modules-and-services.md)
- One reflection walker feeds JSON, schemas, Luau, type generation and MCP; `unsupported` kinds (arrays, vectors,
  maps) are skipped by JSON and Luau today — [ADR 0026](../../adr/0026-reflection-everywhere.md)
- Toolbox lives in `space/`, engine never names it; generic needs (job system, procgen graph runtime, chunk
  streaming) move down into the engine — [ADR 0042](../../adr/0042-space-toolbox-layering.md)

### Inferences
- `veach::Rng` is usable inside a stage, but procgen needs a 53-bit `next_double`, an unbiased range, and a
  stateless hash to derive child seeds. Those are generic and belong in `engine/core` with Q12.
- Addresses must be a tagged userdata (or string) in Luau, never a number.
- Stage records with arrays (an orbit tree, a list of moons) hit the `unsupported` reflection kinds. The record
  format needs array support in the walker, or records that are flat components on entities.

### Gaps
- None for the repo; these were read directly.

## 1. Seed hierarchy and addressing

### Takeaway
Elite packs sector, octree layer, system and body into one 64-bit integer that is both address and seed, and NMS
packs planet, system and region voxel into 48 bits shown as 12 portal glyphs. Both make the address the seed path.
Both also show the cost of fixed widths and per-cell constants: Elite's sky shows seams at its sector cubes and
can't be fixed without moving discovered systems.

### Cited Findings
- Elite: "a 64-bit integer encodes a sector's x, y, z coordinate, its layer in an eight-layer octree, the star system
  ID, and the body ID"; client and server agree on the same object; noise takes the surface point and a unique
  planet id; everything is simulated top-down; a body stores only its own data, links to its parent and can reach
  its siblings — [80.lv, Doc Ross interview](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)
- Elite ID64 has the parts Body#, SystemID, SectorX/BoxelX, SectorY/BoxelY, SectorZ/BoxelZ; the main star's body
  number is 0, so its SystemAddress equals its ID64; systems in a boxel are numbered from 0 "until the boxel runs out
  of mass or there are no more system IDs free"; boxel size depends on the layer (the lowercase mass-code letter)
  **(community)** — [Frontier forum, via search summary](https://forums.frontier.co.uk/goto/post?id=9906921) (page
  returned 403 to direct fetch); [Frontier forum thread on system names](https://forums.frontier.co.uk/threads/latest-updates-to-understanding-system-names-in-the-black.632624/latest)
- Elite stars sit in cubes "that in my understanding have 1280 ly long sides"; skybox density visibly jumps at cube
  boundaries ("Great Cubes of Creation"); a reply says Frontier "can't go changing the galaxy stellar density or
  they'll mess up currently and previously discovered systems" **(community)** —
  [Frontier forum: codex regions and star fields](https://forums.frontier.co.uk/threads/borders-between-codex-regions-seem-to-also-be-borders-between-different-background-star-fields.616573);
  [Frontier forum: hiding the cubes](https://forums.frontier.co.uk/goto/post?id=7388504)
- That mass-code layers run a (10 ly boxels) to h (1280 ly), with heavier systems in larger boxels **(unverified;
  common community understanding, consistent with the 1280 ly figure above)**.
- NMS: the packed 48-bit address is `P-SSS-YY-ZZZ-XXX`: planet index 4 bits (0-15), solar system index 12 bits
  (0x000-0xFFE), voxel Y 8 bits signed, voxel Z and X 12 bits signed; the galaxy ("reality") index is a separate u8;
  the portal address is the same 12 nibbles as glyphs **(community, reverse-engineered)** —
  [nms-core crate source](https://docs.rs/nms-core/0.1.0/src/nms_core/address.rs.html)
- NMS system index 0x79 is always a black hole and 0x7A always an Atlas station **(community)** —
  [NMS wiki naming guidelines](https://nomanssky.fandom.com/wiki/Galactic_Hub_Naming_Guidelines); planet glyph
  values 1-6 in practice **(community)** — [Steam discussion](https://steamcommunity.com/app/275850/discussions/0/601891224286234438)
- Minecraft: population seed mixes world seed and chunk block coordinates "through some layers of hashing"; feature
  (decorator) seed salt is `index + 10000 * step`, where `index` is the feature's position in the feature list; region
  seeds take a per-structure salt; only the low 48 bits of the world seed matter because `setSeed` truncates —
  [Yarn ChunkRandom docs](https://maven.fabricmc.net/docs/yarn-1.21.3+build.1/net/minecraft/util/math/random/ChunkRandom.html)
- In 1.13 structures got their own salts, which temples did not have before **(community)** —
  [gist](https://gist.github.com/hube12/368e7331e497b17e092e8ca4ba206b3c)
- Noise-based RNG (Eiserloh, GDC 2017, "Math for Game Programmers: Noise-Based RNG"): a hash of (position, seed) is
  an order-independent, stateless "table" you index; SquirrelNoise5 replaced the talk's Squirrel3 after Peter
  Schmidt-Nielsen found a weakness — [squirrel-rng crate](https://docs.rs/crate/squirrel-rng/0.4.0/source/src/lib.rs);
  [Rust write-up](https://annardunster-lhli5hya9-ardunsters-projects.vercel.app/posts/2024/eiserloh-noise-rng-rust)
- Counter-based RNGs (Philox, Threefry; Salmon et al., SC11 best paper): each output is a function of a counter and
  a key, so any element is computed directly without generating the sequence; Philox uses wide integer multiplies;
  implementations exist for CPUs and GPUs; not for cryptography —
  [Random123 docs](https://sources.debian.org/data/main/libr/librandom123/1.09%2Bdfsg-2/docs/html/index.html);
  [paper PDF](https://users.cs.utah.edu/~hari/teaching/bigdata/random123sc11.pdf);
  [Wikipedia](https://en.wikipedia.org/wiki/Counter-based_random_number_generator)

### Inferences
- An address-as-seed-path design is the precedent. A layered octree whose layer depends on how bright (massive) the
  star is gives a bounded sky query from anywhere: a star visible to distance d lives in a layer whose cell size is
  about d / k, so a fixed neighborhood of cells per layer covers every star bright enough to see.
- The "Great Cubes" artifact comes from a per-cell constant density. A continuous density sampled at each candidate's
  position (thinning) removes it.
- Minecraft's list-index salt means inserting a feature shifts the seed of every later feature. Salts must come from
  stable string ids, never list positions.
- NMS's 4-bit planet index is a fixed budget it can't grow; leave headroom in body bits and reserve ranges for mods.

### Gaps
- No official bit layout of Elite's ID64; the community layout could not be fetched directly (403).
- No official NMS address spec; all of it is reverse-engineered.
- McKendrick's and Murray's GDC 2017 talks are video only; no transcript found covering seeds or addresses.

## 2. Galaxy structure and deep-space objects

### Takeaway
Elite's galaxy is mass and age distribution functions plus one hand-made density map; SpaceEngine fills unknowns
procedurally "based on current science". Neither publishes its galaxy model. A parametric model (disk, bulge, arms,
halo) plus an optional authored density map is the precedent-backed choice.

### Cited Findings
- Elite: "galaxy-scale material and age distribution functions" set how much material a sector holds and how long it
  has been there; the distribution map is "one of the only non-programmatic resources", used so "the spiral arms and
  galactic bulge form the correct shape"; Hipparcos and Gliese seed real stars —
  [80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)
- About 400 billion systems at 1:1, ~160,000 from catalogs —
  [elitedangerous.com](https://www.elitedangerous.com/explore);
  [Space.com, Braben](https://www.space.com/31366-elite-dangerous-stellar-forge-interview.html)
- Kay Ross livestream: raw inputs (mass distribution, sector ages, stellar lifetime equations) generate the galaxy
  procedurally and deterministically — [Frontier forum summary](https://forums.frontier.co.uk/goto/post?id=8957363)
  (from 00-scope notes)
- SpaceEngine fills gaps where real data is missing with procedural generation based on current science —
  [press release](https://gamespress.com/SPACEENGINE-RELEASES-CLIMATE-MODEL);
  [Wikipedia](https://en.wikipedia.org/wiki/SpaceEngine)
- Milky Way structure for defaults (disk scale length ~2.6-3 kpc, thin disk scale height ~300 pc, bar and bulge,
  central black hole Sgr A* ~4×10^6 M☉), and solar-neighborhood stellar density ~0.1 star/pc³ (~0.004 per ly³)
  **(unverified, textbook values)**.
- Kroupa 2001 broken power-law IMF (slopes ~0.3 below 0.08 M☉, ~1.3 for 0.08-0.5, ~2.3 above) **(unverified)**.

### Inferences
- The galaxy stage is small and global: an analytic density, age and metallicity field plus a catalog of a few
  thousand named deep-sky objects (SMBH, clusters, nebulae). It can be computed at universe load and cached forever.
- Quasars and other galaxies are a background catalog at infinity: directions and fluxes, no parallax, identical from
  everywhere in the galaxy, so they cost nothing to keep coherent.
- Clusters must be density terms in the star distribution, not decorations, or the stars you fly to won't match the
  cluster you saw.

### Gaps
- No primary source on how Elite or SpaceEngine model spiral arms, clusters or nebula placement.
- No published Elite skybox pipeline; community only.

## 3. Stars, system formation and body physics

### Takeaway
Elite runs a protoplanetary-disc simulation per system over epochs and tracks tidal locking and heating per
proto-planet; SpaceEngine classifies planets by temperature class, volatiles class, mass prefix and bulk
composition. Both derive the surface from physical parameters, not the reverse.

### Cited Findings
- Elite: leftover material sets a proto-planetary disc; the simulation steps through epochs and element
  distributions accumulate; solar winds, catastrophic events, tidal locking and gravitational heating are tracked per
  proto-planet; each planet gets a classification from mass and materials, volcanic parameters, crust/mantle/core
  temperature, trapped gases, tidal and radiative heating; these set ranges for tectonics, volcanism, basins,
  mountains, fissures, craters and ice; averages are "packed up into a buffer which is sent to the GPU" to modulate
  noise — [80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)
- SpaceEngine class scheme "[temperature class] [volatiles class] [mass prefix][bulk composition class]";
  temperature upper limits frigid 90 K, cold 170, cool 250, temperate 330, warm 500, hot 1000, torrid above (500 and
  1000 "arbitrary"); volatiles airless (< 1 nbar), desertic, lacustrine, marine, oceanic, superoceanic; composition
  ferria (> 50% metals), carbonia (> 25% carbon), aquaria (> 25% water), terra (the rest), jupiter (> 25% H/He),
  neptune; mass prefixes for solids micro 0.002, mini 0.02, sub 0.2, (none) 2, super 10 M⊕, mega above —
  [SpaceEngine blog, New planet classification 2](https://spaceengine.org/news/blog170924/)
- Habitable zones: Kopparapu et al. 2014 give HZs for T_eff 2600-7200 K and 0.1-5 M⊕; larger planets have wider HZs;
  the inner edge moves ~10% lower flux for low-mass planets and ~7% higher for larger ones; an analytic expression is
  given in the paper — [arXiv:1404.5292](https://arxiv.org/abs/1404.5292); 2013 base paper
  [ADS](https://ui.adsabs.harvard.edu/abs/2013ApJ...765..131K/abstract)
- The analytic form S_eff = S_eff☉ + aT + bT² + cT³ + dT⁴ with T = T_eff − 5780 K and d = √(L/S_eff) AU, and the
  2014 one-Earth-mass values (runaway greenhouse ~1.107, maximum greenhouse ~0.356) **(unverified coefficients;
  read them from the paper before use)**.
- Cosmic shoreline: atmospheres are retained or lost along a line where instellation scales roughly with v_esc⁴ —
  [Zahnle & Catling 2017](https://arxiv.org/abs/1702.03386)
- Jeans escape parameter λ = v_esc² / v₀² with v₀ = √(2kT/m); rule of thumb: a gas survives billions of years if
  v_esc ≳ 6× its mean thermal speed — [Atmospheric escape](https://en.wikipedia.org/wiki/Atmospheric_escape)
  **(not fetched, unverified)**
- Snow line ~2.7 AU for the Sun (Hayashi 1981), scaling ~√L **(unverified)**; giant-planet frequency rises with
  stellar metallicity (Fischer & Valenti 2005) **(unverified)**; Hill stability ≳ 10 mutual Hill radii (Chambers et
  al. 1996) **(unverified)**; prograde moons stable within ~0.5 Hill radius (Domingos et al. 2006) **(unverified)**;
  fluid Roche limit d ≈ 2.44 R_p (ρ_p/ρ_m)^(1/3) — [Roche limit](https://en.wikipedia.org/wiki/Roche_limit) **(not
  fetched)**; S-type and P-type binary stability limits (Holman & Wiegert 1999) **(unverified)**.
- Mass-luminosity L ∝ M^~3.5-4 on the main sequence, lifetime ~10 Gyr (M/M☉)^-2.5 **(unverified)**; rocky mass-radius
  R ∝ M^~0.27 (Zeng et al.), probabilistic "Forecaster" (Chen & Kipping 2017) **(unverified)**.
- Tidal despinning τ ∝ a⁶Q/k₂ — [arXiv:1204.3923](https://arxiv.org/abs/1204.3923) (from 00-scope notes)

### Inferences
- System formation is non-local by nature (snow line, spacing, Roche, Hill, binary limits); it is one cached
  whole-system stage, not per-body noise.
- SpaceEngine's classes are a good shape for a default classifier: a body kind is a rule over physical parameters,
  which is exactly a registry entry with a predicate.

### Gaps
- No primary description of Elite's epoch simulation (number of epochs, what accretes).
- Kopparapu coefficients not read from the paper here.

## 4. Climate, biomes and life

### Takeaway
SpaceEngine's published climate model starts from equilibrium temperature with a redistribution factor, adds
day/night and latitude/obliquity terms per planet type, combines stars as a fourth-power sum, then adds greenhouse
and internal heat; Star Citizen's Planet Tech v4 picks biomes from temperature and humidity maps. NMS could not
simulate erosion because it generates locally.

### Cited Findings
- SpaceEngine climate: equilibrium temperature with bond albedo A and redistribution factor f (f = 4 uniform, f = 2
  dayside-only such as tidally locked); terrestrial with atmosphere uses dT/dt = ΔF/(c_p ρ H), τ_rad =
  c_p ρ H/(σ T₀³), τ_adv = 2π R_p / v_wind; airless bodies peak at the subsolar point; gas giants use a sinusoid with
  wind-set phase, surface at 1 atm; tidally locked use a Lambert cosine with night side ~1/3 of day high; latitude
  term with a minimum polar fraction from pressure, rotation and equator-pole difference; sunlit fraction of day
  arccos(−tan φ tan φ_ss)/π; multiple stars T = (Σ T_i⁴)^(1/4), then internal heating and greenhouse; altitude via
  exponential pressure and a scaled P-T profile — [SpaceEngine, The Climate Model](https://spaceengine.org/articles/the-climate-model/)
- Star Citizen Planet Tech v4 generates terrain and biome selection from temperature and humidity data; arrived from
  Alpha 3.8 with texture blending, scattering and biome transitions; v5 adds geology, soil type, soil depth,
  nutrients **(secondary, fan wiki)** — [The Impound, PT v4](https://theimpound.com/blogs/star-citizen-wiki/planet-tech-v4);
  [The Impound, PT v5](https://theimpound.com/blogs/star-citizen-wiki/planet-tech-v5);
  [Twinfinite](https://twinfinite.net/news/star-citizen-microtech-planet-tech-v4/); a player thread says v4 placement
  is uniform and lost v3 mini-biomes (opinion) —
  [RSI Spectrum](https://robertsspaceindustries.com/spectrum/community/SC/forum/50259/thread/planet-tech-v3-to-v4-in-2019-it-s-lost-biomes-un-1)
- NMS (McKendrick, GDC 2017, from notes): 1 m voxels at 6 bytes, 36 m chunks; simulate on a sphere, store on a cube;
  generation at runtime in the background; CPU on PC, compute on PS4; "because data had to be generated locally, the
  team couldn't simulate erosion and instead wrote PCG algorithms that approximated its results"; perlin worms for
  caves — [blather-rinse-repeat GDC 2017 notes](https://blather-rinse-repeat.blogspot.com/2018/11/gdc-2017-part-ii-main-conference-notes.html);
  talk: [GDC Vault](https://gdcvault.com/play/1024265/Continuous-World-Generation-in-No)
- NMS Origins kept terrain of existing planets, reset creatures, added new planets beside old ones; Worlds Part II
  added gas giants as a new kind — [Origins](https://nomanssky.com/origins-update/);
  [Xbox Wire](https://news.xbox.com/en-us/2025/01/29/no-mans-sky-worlds-part-2-out-today/) (from 00-scope notes)
- Whittaker biome diagram (temperature × precipitation) and Holdridge life zones **(unverified, textbook)**.

### Inferences
- Climate and biomes are whole-body coarse fields (SpaceEngine computes per planet; Star Citizen reads maps), cached
  per body, then sampled by chunk-local generation.
- NMS's erosion limitation is the counter-example for a purely local pipeline.

### Gaps
- No primary source on SpaceEngine's biome or life generation (the climate article says the fields aren't shown).
- No primary Star Citizen technical talk on v4 found in this pass.

## 5. Registries, hooks and edits

### Takeaway
NMS shipped new kinds (gas giants, new planets, binary stars) years after launch and had to keep old content stable;
Minecraft's per-feature salts show how extension can shift seeds by accident. Registries keyed by stable string ids,
with additive slots for new content, are the pattern.

### Cited Findings
- NMS Origins: "No existing planet will have its fundamental type or hazard changed"; "New planets have been created
  in most star systems" — [nomanssky.com](https://nomanssky.com/origins-update/)
- Minecraft salts from list positions (`index + 10000 * step`) —
  [Yarn docs](https://maven.fabricmc.net/docs/yarn-1.21.3+build.1/net/minecraft/util/math/random/ChunkRandom.html)
- Space Engineers persists voxel edits over generated planets —
  [Rosa](https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/) (from 00-scope notes)

### Inferences
- A mod adding a body kind must not reclassify existing bodies silently; additions claim new slots (reserved index
  ranges) or bump a stage version (Q11).
- Registration order must not affect output: iterate kinds sorted by id, never load order.

### Gaps
- No public detail on how NMS stores which generator version a planet uses.

## 6. Cost and testability

### Takeaway
Elite does the physics per planet on the CPU and only the noise on the GPU; NMS generates terrain on the CPU on PC
and compute on PS4. Record stages are cheap; chunk-level generation is the budget.

### Cited Findings
- Elite: per-planet averages to a GPU buffer; compute shaders spherify patches and run noise graphs; 64-bit or
  emulated dual-float functions on the GPU; "a very efficient pipeline for requesting and generating these uniform
  patches" — [80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)
- NMS: CPU on PC, compute shaders on PS4 — [GDC 2017 notes](https://blather-rinse-repeat.blogspot.com/2018/11/gdc-2017-part-ii-main-conference-notes.html)

### Inferences
- Per-record stage cost is not published by anyone; it must be measured in a spike on the M5 Max.
- Content must never depend on quality tier: M1 vs M3 Pro may change field resolution and star-count limits only
  for visual-only outputs.

### Gaps
- No published per-system or per-body generation times from any precedent.
- No measured M1 or M3 Pro numbers for anything here; only the M5 Max can be measured.
