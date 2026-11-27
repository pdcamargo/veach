# 00: Scope gap analysis

What the owner's scope for the space toolbox leaves out, measured against eight shipped games or tools (No Man's Sky,
Elite Dangerous, Star Citizen, Space Engineers, Kerbal Space Program, Outer Wilds, Astroneer, SpaceEngine) and against
the astronomy and planetary science a physically coherent pipeline needs. Every research question after this one must
respect the classification below. Raw notes and every source: [notes/00-scope-notes.md](notes/00-scope-notes.md).

**Classes.**

- **(a) Founding:** it constrains a decision we make now. It is folded into the named question, which must answer it.
- **(b) Later, but must be possible:** we don't build it now, but the architecture must leave the named extension
  point open. "Luau" means a creator adds it with scripts and data; "module" means a C++ Veach module (a Flecs
  module, [ADR 0019](../adr/0019-veach-modules-and-services.md)); "core" means only a toolbox change can add it;
  "engine" means it is generic and moves down into `engine/` ([ADR 0042](../adr/0042-space-toolbox-layering.md)).
- **(c) Out of scope for now,** with the reason.

**Questions** (the nine from the brief, plus three this analysis adds; see [Questions added or split](#questions-added-or-split)):
Q1 coordinates and frames, Q2 celestial mechanics and time, Q3 generation pipeline, Q4 terrain, Q5 procgen graph,
Q6 networking, Q7 simulation LOD, Q8 renderer, Q9 physics, **Q10 gravity, flight and travel**, **Q11 persistence and
generator versioning**, **Q12 numeric determinism**.

**Scale notation.** s_p scales planet-scale lengths (radii, atmosphere height, moon orbits, large terrain features);
s_sys scales system lengths (planet orbits, star radii); whether they are one factor is Q1/Q2's call. "Invariant"
means the quantity does not change under a single uniform s.

## The five findings that matter most

1. **Nobody owns the travel model, and it sets the streaming budget.** Elite (normal flight ~300 m/s, supercruise,
   hyperspace) and X4 (engine tiers, gates, time acceleration) make scale playable with speed tiers tied to gravity
   wells. The speed you may approach a surface at is the rate Q4 must stream and mesh terrain, and the reason "no
   loading screen" is possible at all. It gets its own question (Q10).
2. **The pipeline cannot be pure point-evaluated noise.** Rings (Roche limit), moon placement (Hill sphere), giant
   planets (snow line), system stability (Hill spacing), rivers and drainage, and plate boundaries are non-local. Q3 and
   Q5 must have whole-system and whole-body stages, computed once and cached, before chunk-local generation. Elite's
   Stellar Forge works this way (galaxy → star → disc → proto-planets → surface parameter ranges → GPU noise).
3. **The universe's identity is seed + generator versions + mods, not the seed.** NMS Origins kept every existing
   planet's terrain, reset creatures, and added new planets beside the old ones; Elite says changing generation now
   would upset the whole galaxy. With Luau procgen hooks, two clients with different mods generate different universes
   from one seed. Generator versioning is founding (Q11), and every generator must be versioned per layer.
4. **Determinism across ARM/x64 and CPU/GPU is a contract we have to write, not a property we have.** IEEE basic
   operations are reproducible; libm transcendentals, FMA contraction, fast-math shader compilation and Luau's libm
   calls are not. Anything that gameplay or the server depends on (collision, resource placement, what exists where)
   must come from a deterministic path; GPU-only output must be visual-only. Q12.
5. **Several Veach types are scale-blind today and must change before anything else.** Luau's `vector` is float32
   ([ADR 0013](../adr/0013-conventions-and-math.md)), `Transform` is float32, JSON numbers lose int64 above 2^53, the
   sky assumes Earth at Y = altitude with one sun, and exposure is a fixed EV100. Q1, Q8 and Q11 must fix the types,
   not work around them.

## 1. Universe structure and deep-space objects

| Item | Class | Folds into / extension point / reason | Precedent | Scale note |
|---|---|---|---|---|
| Galaxy model: a density/age distribution (arms, bulge, disc) that star positions are drawn from | a | Q3: the top stage of the seed hierarchy; Q1: galaxy frame | Elite: mass and age distribution functions plus one hand-made map ([80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)) | Interstellar distances should not follow s_p or s_sys: travel between stars is FTL anyway. Recommend a third factor, default 1. |
| Spatial index of the galaxy (octree sectors) with a 64-bit id that is both address and seed path | a | Q3 (seed hierarchy), Q1 (galaxy coordinates), Q6/Q11 (ids on the wire and in saves) | Elite: 8-layer octree, one 64-bit integer encodes sector, layer, system and body ([80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)) | Ids must not encode scaled positions. |
| Query API over the galaxy ("stars within r of p", "next system along a ray") | a | Q3: the galaxy stage must be queryable without generating systems; feeds the starfield (Q8), the map (b) and FTL targeting (Q10) | Elite galaxy map; SpaceEngine (unverified detail) | Invariant in light-years. |
| Starfield and skybox generated from the actual galaxy, coherent with where you are | a | Q8, from Q3's query API: no static cubemap. Near stars are points with true color and magnitude, far ones aggregated | SpaceEngine renders its catalog plus procedural stars ([Wikipedia](https://en.wikipedia.org/wiki/SpaceEngine)) | Invariant. |
| Galactic-center supermassive black hole | a (data) / b (render) | Q3: a fixed object in the galaxy model. Render (lensing, accretion disk) is a deep-sky render feature: module | Elite has Sagittarius A* (detail unverified) | Not scaled. |
| Quasars | b | Deep-sky object registry (data + generator: Luau; render: module). Astronomy note: a quasar is an active galactic nucleus in another galaxy; inside our galaxy they are distant point sources, or the creator makes the home galaxy's nucleus active | Astronomy | Not scaled. |
| Nebulae (emission, reflection, dark), visible from outside and inside | a (data) / b (render) | Q3: a diffuse-object representation (low-res density fields at galaxy scale) so the skybox seen from far and the fog seen from inside are the same object. Render technique: Q8 module, needs 3D textures (VCH-072) | SpaceEngine (catalog nebulae) | Not scaled; volumes at light-year size. |
| Star clusters (open, globular) | b | Galaxy distribution plugin: Luau data and generator | SpaceEngine catalogs clusters | Not scaled. |
| Other galaxies as background | b | Same deep-sky registry; one skybox layer | SpaceEngine (10 billion parsec universe) | Not scaled. |
| Galaxy rotation, stellar proper motion, stellar evolution during play | c | Timescales of millions of years; the galaxy is static during play. Age is still a generation input (a, Q3) | SpaceEngine doesn't simulate them either ([Wikipedia](https://en.wikipedia.org/wiki/SpaceEngine)) | — |
| Real star catalogs (Hipparcos, Gaia) mixed in | c | A product choice for a real-Milky-Way game; the pipeline must not prevent a "catalog source" stage, which the galaxy query API already allows | Elite seeds with Hipparcos and Gliese ([80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)) | — |

## 2. Stars and star systems

| Item | Class | Folds into / extension point / reason | Precedent | Scale note |
|---|---|---|---|---|
| Stellar class (O B A F G K M, brown dwarfs, white dwarfs, neutron stars, black holes) from mass and age, giving temperature, luminosity, radius | a | Q3: first physical stage of a system. Light color from temperature (blackbody), intensity from luminosity, into photometric lights ([ADR 0036](../adr/0036-lights-in-photometric-units.md)) | Elite generates black holes, neutron stars and brown dwarfs from first principles ([Giant Bomb](https://giantbomb.com/wiki/Games/Elite_Dangerous)); [stellar classification](https://en.wikipedia.org/wiki/Stellar_classification) | Luminosity is physical. Star radius follows s_sys. |
| Star kinds beyond the main sequence (giants, Wolf-Rayet, pulsars) | b | Star-kind registry: Luau generator + data; render variants as material/feature: module | Elite | — |
| Illuminance at a body from its star(s) | a | Q8 + Q2: computed from **physical** distance and luminosity, never scaled distance | ADR 0038 uses 128,000 lux at the top of Earth's atmosphere | If computed from scaled orbits, E grows by 1/s_sys². Must use physical. |
| Binary and multiple star systems (hierarchical: pairs orbiting barycenters; S-type and P-type planets) | a | Q2: orbits are a tree of barycenters, not "planets around the star". Q3: stability limits for S/P orbits (Holman & Wiegert 1999, unverified). Q8: several suns | NMS Origins added binary and rare ternary stars ([nomanssky.com](https://nomanssky.com/origins-update/)) | Invariant under one s_sys. |
| Multiple suns lighting one scene | a | Q8: today one directional sun and one Hillaire sky-view LUT per sun direction ([ADR 0038](../adr/0038-physical-sky.md)). N suns means N directional lights with angular size and a sky that sums them (N LUT passes or a different structure). Budget per extra sun on M1 vs M3 Pro | NMS binaries | Angular size invariant under one factor. |
| Habitable zone from stellar flux | a | Q3: Kopparapu et al. 2013 parametric fit on incident flux (not temperature, so no albedo dependence); sum flux in multi-star systems | [Kopparapu 2013](https://ui.adsabs.harvard.edu/abs/2013ApJ...765..131K/abstract); mass-dependent [2014](https://dx.doi.org/10.1088/2041-8205/787/2/l29) | Computed physically; invariant by construction. |
| Snow line deciding where giants and icy bodies form | a | Q3: a whole-system stage before body placement | Hayashi 1981 (unverified); Elite tracks a protoplanetary disc ([80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)) | Physical. |
| Orbital spacing and stability of the generated system | a | Q3: on-rails orbits never perturb each other, so the generator must space them (≥ ~10 mutual Hill radii; Chambers et al. 1996, unverified) or the system looks wrong to anyone who knows | Astronomy | Ratio test, invariant. |
| Stellar variability, flares, space weather | b | Star behavior hook: Luau event generator driving light intensity and hazards | NMS has hazards; none modeled physically | — |

## 3. Planets, moons and small bodies

| Item | Class | Folds into / extension point / reason | Precedent | Scale note |
|---|---|---|---|---|
| Body kinds as a registry (rocky, ocean, gas giant, ice giant, dwarf, asteroid, comet, artificial) rather than an enum | a | Q3/Q5: `BodyKind` = data + registered generator stages + render features. Luau for generators and data, module for new render or terrain representations | NMS added gas giants as a new kind years after launch ([Worlds Part II](https://news.xbox.com/en-us/2025/01/29/no-mans-sky-worlds-part-2-out-today/)) | — |
| Gas and ice giants: no landable surface | a | Q4 (a body with no solid surface: no terrain chunks, or a deep floor), Q8 (banded atmosphere rendered as a volume/shell), Q10 (what flying in does: crush depth, ejection) | NMS gas giants, ~10× the largest planets ([Push Square](https://www.pushsquare.com/news/2025/01/no-mans-sky-gets-another-huge-game-changing-update-with-worlds-part-ii)) | Real giants are 10× Earth's radius: 70,000 km at s = 1 sets the worst case for Q1 precision and Q8 depth. |
| Moons, and moons of moons | a | Q1 (nested frames), Q2 (orbit tree, patched conics by sphere of influence) | KSP (Mun, Minmus; patched conics, [KSP wiki](https://wiki.kerbalspaceprogram.com/wiki/Kraken)) | Moon orbits should follow s_p, not s_sys, so the Roche/ring relationship and angular sizes stay right. With gameplay masses, SOI scales as s^0.8, so moons must be revalidated against it. |
| Rings, with the Roche limit deciding rings vs moons | b (render) / a (rule) | Q3: rings only inside the Roche limit, moons outside. Render: ring feature (module) with planet shadow on rings and ring shadow on the planet | [Roche limit](https://en.wikipedia.org/wiki/Roche_limit); Elite rings also affect surface ice ([80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)) | Invariant under one factor; broken if ring radius and moon orbits use different factors. |
| Asteroid belts (statistical) | b | Belt as a distribution on an orbit: Luau data; the field you fly into is generated locally on demand | Elite asteroid rings (mining) | Follows s_sys. |
| Asteroids as small editable bodies | a | Q4 (already in the brief): same terrain representation as planets with weak or no gravity | Space Engineers voxel asteroids ([Rosa](https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/)) | Meters; never scaled. |
| Comets (very eccentric orbits, tails pointing away from the star) | b | Q2 must pick a Kepler solver that handles e → 1 and hyperbolic orbits (a); tails are a render feature (module) | Astronomy | — |
| Super-earths, mini-Neptunes | b | Just parameters of the pipeline; they stress surface-gravity gameplay (2-3 g), which is the per-quantity creator setting Q2 defines | Exoplanet population | g is a gameplay setting, not scaled. |
| Tidally locked bodies (day side, night side, terminator) | a | Q2: rotation period equals orbital period, no solar day, fixed sun position per location. Q3: locking decided from a despinning timescale vs system age (τ ∝ a⁶Q/k₂) | Elite tracks tidal locking per proto-planet ([80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)); [Gladman et al. via arXiv:1204.3923](https://arxiv.org/abs/1204.3923) | Decided on physical values; holds at any s. |
| Rogue planets (no star) | b | The pipeline must not assume a host star: temperature from internal heat, light from the starfield only. Body-in-galaxy placement hook | Astronomy | — |
| Volcanism and tidal heating (Io-like) as a pipeline output | a | Q3: internal heat from mass, age and tidal heating drives volcanism, which Q7's eruptions and Q4's terrain use | Elite: crust/mantle/core temperature, tidal heating, volcanic parameters; NMS volcanoes on "disrupted core" planets ([Origins](https://nomanssky.com/origins-update/)) | Physical. |
| Magnetic field (aurorae, atmosphere stripping) | b | A physical parameter slot in the body stage; consumers (aurora render feature, escape refinement) are modules | Astronomy | Physical. |
| Lagrange points, trojans, resonances | b | Orbit-tree node kinds (Luau data) | Astronomy | Invariant. |
| Artificial bodies (stations, megastructures) on orbits | b | Body kind with authored content instead of terrain; same frames as moons | Elite stations; Star Citizen | Human scale inside, never scaled. |

## 4. Physically coherent generation pipeline

| Item | Class | Folds into / extension point / reason | Precedent | Scale note |
|---|---|---|---|---|
| Staged order: galaxy → system (star mass, age, metallicity, disc) → body (mass, radius, composition, orbit, spin, obliquity) → derived → climate → biomes → life → objects | a | Q3, Q5. Stage outputs are typed records; later stages read earlier ones only | Elite Stellar Forge ([80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)) | The whole pipeline runs on physical (unscaled) values; s is applied when placing geometry. |
| Derived physics: surface gravity, escape velocity, scale height, equilibrium temperature T_eq = T★√(R★/2a)(1−A)^¼ | a | Q3 | Textbook ([equilibrium temperature](https://en.wikipedia.org/wiki/Planetary_equilibrium_temperature)) | Computed physical, then the gameplay values are chosen per quantity (Q2). |
| Atmosphere retention | a | Q3: the cosmic-shoreline test (escape velocity vs instellation) as default, per-gas Jeans escape as a refinement hook | [Zahnle & Catling 2017](https://arxiv.org/abs/1702.03386); [Jeans escape](https://en.wikipedia.org/wiki/Atmospheric_escape) | Physical v_esc, not the scaled one (scaled is √s smaller and would strip every atmosphere). |
| Greenhouse warming from composition and pressure | a | Q3: a small model (optical depth from CO2/H2O/CH4) over T_eq; Earth needs +33 K | Textbook | Physical. |
| Climate: temperature and precipitation by latitude and season | a | Q3 (static seasonal fields per body from a cheap energy-balance model) and Q7 (weather as noise and events on top) | Budyko-Sellers style models (no source checked) | Fields are on the sphere's surface, so they follow s_p geometrically; values are physical. |
| Biomes from climate (temperature, precipitation, altitude, solvent) | a | Q3/Q5: biome = data + rules in a registry (Luau). No hardcoded biome enum | Whittaker diagram (unverified); NMS biome types | Biome area follows s_p: at s = 0.01 a continent-sized biome shrinks to a few km, so biome patch size needs a floor in meters. |
| Life and biochemistry (water, ammonia or methane solvent; Titan-like worlds) | b | Life stage hooks: Luau. The stage receives temperature, pressure, solvent, light | Astronomy | Physical. |
| Non-local planet-wide stages: plate boundaries, drainage basins, rivers, erosion | a | Q3/Q4/Q5: a coarse whole-body stage, computed once per body (CPU or compute), cached and versioned, before chunk-local generation. Rivers cannot come from point noise | [Génevaux et al. 2013, hydrology terrain](https://doi.org/10.1145/2461912.2461996) (unverified); Elite infers tectonic and volcanic ranges per planet | Coarse stage follows s_p; river widths and banks are meter-scale detail that never scales. |
| Split between planet-scale features (follow s_p) and detail (meters, never scaled) | a | Q3/Q4/Q5, as the brief says | KSP (1/10 scale planets, human-scale parts) | This is the scale design itself. |
| Who may hook which stage, from where | a | Q5: every stage has a Luau hook and a C++ hook; the graph knows its stages' inputs, so a hook can't read data from a later stage | NMS shipped new phenomena as data years later ([Origins](https://nomanssky.com/origins-update/)) | — |

## 5. Time

| Item | Class | Folds into / extension point / reason | Precedent | Scale note |
|---|---|---|---|---|
| A universal simulation clock, integer ticks (int64 at 60 Hz lasts ~4.9 billion years) | a | Q2. Orbital phase must come from `tick mod period`, not `n·t` in doubles, which loses precision as t grows | Derivation; [ADR 0018](../adr/0018-engine-lifecycle-and-phases.md) fixed 60 Hz | Independent of s. |
| Per-body rotation, sidereal vs solar day, year, local solar time, day of year, seasons from obliquity and eccentricity | a | Q2. Replaces the Earth-only `time_of_day`, `latitude`, `day_of_year` inputs of today's `Sky` | Already in the brief; [solar time](https://en.wikipedia.org/wiki/Solar_time) (unverified) | Rotation period is radius-independent: day length can stay physical. Year length depends on whether s_sys rescales orbital periods (Q2's on-rails decoupling). |
| Calendars and time display per body | b | Luau formatter over the clock and body record | — | — |
| Eclipses (body occluding a star, planet shadow on moons and rings) | a (data) / b (render) | Q2 must answer "which bodies occlude star S from point P at tick t" cheaply; Q8 consumes it as an analytic occluder list | Astronomy | Total vs annular depends on angular sizes: invariant under one factor, changed by separate s_p and s_sys. |
| Time compression / warp | a | Q2 + Q6: a product decision. Recommend none in multiplayer (the server owns one clock), optional in single player, and a "time passes while away" catch-up instead (Q7) | KSP has warp and on-rails switching ([Krakensbane](https://www.kerbalspaceprogram.com/ksp/api/class_krakensbane.html)); X4 time acceleration ([Space.com](https://www.space.com/entertainment/space-games/why-space-games-still-struggle-with-the-scale-of-the-universe)) | — |
| Time as content (scheduled events, loops) | b | Event scheduler on the universal clock: Luau | Outer Wilds' 22-minute loop ([GDC 2020](https://gdconf.com/news/see-4d-level-design-outer-wilds-deconstructed-gdc-2020)) | — |
| Light travel time, relativity | c | No gameplay value for a NMS-style game; breaks a shared "now" in multiplayer | — | — |
| Tides on oceans | c | Moving sea level fights base building and static ocean surfaces. Revisit as a render-only effect | — | — |

## 6. Reference frames and coordinates

| Item | Class | Folds into / extension point / reason | Precedent | Scale note |
|---|---|---|---|---|
| Frame tree: galaxy → system barycenter → star → planet (orbiting, rotating) → moon → surface/base → ship interior | a | Q1, Q2, Q9 | KSP (on rails + floating origin + Krakensbane, [API](https://www.kerbalspaceprogram.com/ksp/api/class_krakensbane.html)); Star Citizen 64-bit positions ([Gamers Nexus](https://gamersnexus.net/gg/2622-star-citizen-sean-tracy-64bit-engine-tech-edge-blending)) | Real scale: a planet orbit is ~1.5×10¹¹ m and a galaxy ~10²¹ m; doubles resolve ~30 µm at 1 AU but ~100 km at 10²¹ m, so the galaxy needs a sector + local position, not one double. |
| Bases riding on a rotating planet (body-fixed frame) | a | Q1/Q9: physics on a surface runs in the body-fixed frame; recommend ignoring Coriolis and centrifugal terms by default | Space Engineers planets do not rotate (unverified); NMS bases are static | Surface speed 2πR/T ∝ s. |
| Moving local frames: walking inside a flying ship, ships docked in ships | a | Q1/Q9/Q6: needed for ship building and multiplayer. Physics of the interior in the ship's frame | Space Engineers grids; Star Citizen physics grids (unverified) | Human scale; never scaled. |
| Positions in Luau, JSON and MCP | a | Q1: Luau `vector` is float32 ([ADR 0013](../adr/0013-conventions-and-math.md)), so scripts need a position type that can't silently lose precision; JSON/MCP must encode int64 as strings (JavaScript clients lose integers above 2^53) | Veach-specific | Must hold real scale. |
| Camera-relative rendering, extraction in float32 relative to the camera | a | Q1/Q8; also photo mode's TLAS ([ADR 0041](../adr/0041-photo-mode.md)) must take camera-relative transforms | KSP floating origin ([API](https://kspmoddinglibs.github.io/KSPDocsSite/class_floating_origin.html)); Elite doubles and emulated dual-float ([80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)) | — |
| `world_up` = +Y and sky altitude = world Y | a | Q1/Q8: "up" becomes a function of position (gradient of gravity or body normal) | Known Veach flat-world assumption | — |
| Audio listener and spatial audio in the local frame | b | VCH-029 must take camera-relative positions from Q1 | — | — |

## 7. Gravity, flight and travel (new Q10)

| Item | Class | Folds into / extension point / reason | Precedent | Scale note |
|---|---|---|---|---|
| Spherical gravity per body, player orientation (up from gravity), walking around a planet | a | Q10 (+ Q9 for the force) | Space Engineers: gravity scales with size and falls off with distance ([Petrzilka](https://blog.marekrosa.org/2015/04/guest-post-by-ondrej-petrzilka-space_17/)) | Surface g is a creator setting; KSP keeps 9.81 at 1/10 radius. With g fixed, gameplay mass ∝ s². |
| Transitions between gravity wells (sphere of influence handoff) | a | Q10/Q2 | KSP patched conics | SOI ∝ a·s^0.8 with gameplay masses. |
| Ship flight model: Newtonian, assisted or arcade; a swappable module | a (seam) / b (models) | Q10 decides the seam (forces in which frame, at what step); the models are modules | Elite flight assist; KSP pure Newtonian; NMS arcade (unverified) | Orbital speed √(gR) ∝ √s: at s = 1 orbital speeds are ~7.9 km/s, which no arcade flight model wants. |
| Travel tiers and speed limits near bodies (supercruise, pulse, FTL) | a | Q10, and the budget Q4's streaming must meet | Elite: ~300 m/s, supercruise, hyperspace ([Space.com](https://www.space.com/entertainment/space-games/why-space-games-still-struggle-with-the-scale-of-the-universe)); X4 tiers | The approach speed has to grow with s and R for travel times to stay playable, so Q4's streaming rate is a function of s. |
| Atmospheric entry: drag and lift from the same density profile the renderer uses, heating VFX | a (one density model) / b (VFX) | Q10/Q8/Q3: one atmosphere record is the source for sky, drag and audio | KSP reentry heating (unverified) | If the atmosphere scales by s_p with density rescaled to keep the look, drag at a given altitude rises 1/s. Decide per quantity. |
| Landing and docking on rotating bodies and moving stations | a | Q10/Q1 | Elite stations | — |
| FTL between systems (with or without a loading transition) | a | Q10/Q3/Q6: the brief says no loading screen planet-to-surface; between systems a transition can hide generation of the next system | Elite hyperspace tunnel; NMS warp | Independent of s. |
| Artificial gravity, gravity generators | b | Gravity source registry: module | Space Engineers gravity generator blocks ([ModDB](https://www.moddb.com/news/update-01044-spherical-gravity-generator-modding-for-prefab-and-respawn-ship)) | — |

## 8. Determinism, versioning and persistence (new Q11, Q12)

| Item | Class | Folds into / extension point / reason | Precedent | Scale note |
|---|---|---|---|---|
| Float determinism across ARM (Mac) and x64 (Windows): FMA contraction, libm transcendentals, compiler flags | a | Q12. Basic IEEE ops reproduce; `sin`, `exp`, `pow` from different libms don't. Recommend engine-provided math for anything authoritative, and a CI test that hashes stage outputs on macOS arm64 and Windows x64 | [Fiedler](https://gafferongames.com/post/floating_point_determinism/); [Unity thread](https://discussions.unity.com/t/floating-point-determinism-in-unity-intel-vs-amd/813566) | — |
| CPU vs GPU and across GPU vendors | a | Q12/Q5: Metal compiles with fast math by default (Apple docs, unverified; check our Slang build). Rule: GPU output is visual-only unless produced with integer/fixed math and verified bit-equal on Metal and WARP | Fiedler | — |
| Luau procgen determinism | a | Q12/Q5: Luau numbers are doubles and `math.sin` calls the platform libm. Generators get deterministic math from `@veach`, and `veach::Random` streams per stage | Veach-specific | — |
| Generator versioning: saves and servers surviving procgen changes | a | Q11. Every stage carries a version; a body records the versions it was generated with; old versions stay runnable for bodies that have edits on them | NMS Origins: terrain of existing planets kept, creatures reset, new planets added ([nomanssky.com](https://nomanssky.com/origins-update/)); Elite: changes "would upset the proc-gen of the entire Galaxy" (forum, unverified) | — |
| Universe identity = seed + generator manifest (versions and mod hashes) | a | Q11/Q6/Q5: a client with a different manifest is refused or told what's missing | Veach-specific: Luau hooks are the brief's own requirement | Scale is part of the manifest: same seed at s = 0.1 and s = 1 is "the same universe" in content but not in geometry; saves must say which. |
| Player edits and system-authored edits (eruptions, erosion events) stored as deltas over generated content | a | Q11/Q4: edits are keyed by body id, stage version and body-local position in meters; system edits go through the same path, so they are deterministic and replayable | Space Engineers persistent voxel edits ([Rosa](https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/)) | Edits in meters relative to the generated surface, so the same edit applies at any s; a crater "at lat/long" must define what happens when s changes after edits exist (recommend: s is fixed per universe once edited). |
| Save format | a | Q11: text-first per ADR principle 6, with binary blobs for voxel deltas; reflected, versioned, scale-free | — | Must not assume a scale. |
| Record/replay at universe scale | a | Q11/Q12: a recording must capture the seed, the manifest and s; replay on the other architecture is the cross-platform test | [record-replay spec](../specs/record-replay/spec.md) | — |
| Discovery and naming shared across players | b | A discovery service: module; ids from Q3 | NMS uploads discoveries (unverified) | — |

## 9. Simulation LOD

| Item | Class | Folds into / extension point / reason | Precedent | Scale note |
|---|---|---|---|---|
| Off-screen catch-up when a place is revisited | a | Q7: closed-form or event-sourced catch-up from the universal clock, not stepping | Star Citizen freezes unobserved containers ([OCS](https://starcitizen.tools/Object_Container_Streaming)) | — |
| Ecosystem population dynamics | a | Q7 (aggregate predator-prey per biome, instantiate individuals near players) | NMS creatures are generated, not simulated (unverified) | Density per m² must stay fixed; total population ∝ s_p². |
| Economy, trade, NPC ship traffic, faction territory over time | a | Q7, with Q6 for authority | Elite background simulation (unverified detail); X4 | Traffic along routes: route lengths follow s_sys. |
| Automated bases running while nobody is there | a | Q7: closed-form production over elapsed ticks | Space Engineers, NMS bases (unverified) | — |
| Interest management, players in different systems | a | Q6 (already in brief) | Star Citizen server meshing ([OCS](https://starcitizen.tools/Object_Container_Streaming)) | Interest radius in meters near the player, in frames beyond. |
| Weather and geology at planet scale vs local | a | Q7 | NMS storms, tornadoes, firestorms ([Origins](https://nomanssky.com/origins-update/)) | Storm sizes follow s_p, raindrop scale does not. |
| NPC navigation on spherical, editable, streamed terrain | b | Q4 must expose a "chunk ready" event for derived data (colliders, navmesh, foliage, cover), so nav is a consumer module | — | Meters. |

## 10. Rendering at astronomical scale

| Item | Class | Folds into / extension point / reason | Precedent | Scale note |
|---|---|---|---|---|
| Depth precision from cockpit (0.1 m) to a gas giant (10⁸ m) | a | Q8/Q1: reverse-Z float with infinite far ([ADR 0013](../adr/0013-conventions-and-math.md)) already gives near-uniform relative precision; the risk is float32 vertex positions, not depth. Impostors or a separate far pass for bodies | KSP ScaledSpace layer for distant planets ([forum](https://forum.kerbalspaceprogram.com/topic/33402-planets-and-scaledspace/page/2)) | Worst case at s = 1. |
| Impostors for distant bodies, smooth handoff to the terrain renderer | a | Q8/Q4 | KSP ScaledSpace; Elite per-patch lighting for distant views ([80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)) | Handoff distance follows s_p. |
| Atmosphere from orbit and from any body, driven by composition | a | Q8: today's Hillaire LUTs break above the atmosphere top and assume Earth. Per-body parameters from Q3 (Rayleigh from composition, aerosols, absorbers), view from space | [ADR 0038](../adr/0038-physical-sky.md) | Invariant if all lengths scale by s_p and coefficients by 1/s_p, except human-scale aerial perspective, which becomes 1/s hazier. Needs a separate fog control. |
| Auto-exposure from deep space (stars ~0.002 lux) to sunlit (~100 klux) to caves | a | Q8: today a fixed EV100 ([ADR 0037](../adr/0037-exposure-and-agx.md)) | Eye adaptation is standard in the genre (unverified) | Invariant. |
| Eclipses, ring shadows, planet shadows, cloud shadows | b | An analytic occluder list (spheres, rings) given to shading by Q2/Q8: module | Astronomy | Angular sizes: see section 5. |
| Gas giant atmospheres | b | Render feature: module | NMS Worlds Part II | — |
| Underwater, and the camera's medium (air, water, vacuum, cave) | b | A "camera medium" input to the frame from Q8, so underwater and vacuum are features, not special cases | NMS deeper oceans in Worlds Part II | Meters. |
| Volumetric clouds and nebulae | a (RHI) | Q8: needs 3D textures (VCH-072) and probably async compute (VCH-074) | — | Cloud layer thickness follows s_p (or not: a creator setting). |
| Vegetation and object instancing at planet scale | a | Q8: GPU-driven, needs indirect draw (VCH-071) | NMS | Density per m² fixed. |
| Black hole lensing, accretion disks | b | Deep-sky render feature: module | Elite (unverified) | — |
| Planetshine, moonlight | b | A dim directional light per lit body: module | Astronomy | — |
| Photo mode on planet scenes | b | Uses Q1's camera-relative transforms; sky from Q8 | [ADR 0041](../adr/0041-photo-mode.md) | — |

## 11. Terrain, water and surface

| Item | Class | Folds into / extension point / reason | Precedent | Scale note |
|---|---|---|---|---|
| Oceans as a sea-level sphere; lakes with their own levels; rivers following drainage | a | Q4 (water bodies in the representation), Q3 (drainage stage) | NMS deeper oceans ([Worlds Part II](https://www.shacknews.com/article/142889/no-mans-sky-update-55-patch-notes)) | Sea level follows s_p; water depth near shore is meters. |
| Dynamic water simulation (flowing, filling holes) | c | Large scope, fights deterministic edits and networking; static levels first | Most precedents have static water (unverified) | — |
| Caves to the core | a | Q4 (already) | Astroneer caves to the core ([UE spotlight](https://www.unrealengine.com/en-US/spotlights/how-system-era-softworks-leveraged-ue4-to-create-astroneer-s-wonderful-universe)) | Cave size never scales; at s = 1 "to the core" is 6,371 km of depth, so core access is a creator choice. |
| Building on a sphere (no global grid axis) | a | Q4/Q9/Q10: local tangent frames for building grids | Space Engineers grids can't be axis-aligned on planets ([Rosa](https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/)) | Human scale. |
| Ship building (block grids with mass, physics, interiors) | b | Moving-frame physics (a, Q9) + grid module | Space Engineers | Human scale. |

## 12. Later content

| Item | Class | Folds into / extension point / reason | Precedent | Scale note |
|---|---|---|---|---|
| Procedural creatures and their animation | b | Creature stage in the life step (Luau data and generator), and a generic procedural animation/IK runtime in the engine | NMS creatures (reset in Origins, [nomanssky.com](https://nomanssky.com/origins-update/)) | Never scaled. |
| Procedural audio, audio at all | b | VCH-029 is a one-line backlog item; Veach has no audio area. Requirements from this analysis: listener in the camera-relative frame, propagation from the atmosphere record (no sound in vacuum), procedural sources as modules | [The Sound of No Man's Sky, GDC 2017](https://www.gdcvault.com/play/1024067/The-Sound-of-No-Man) | — |
| Ruins, structures, settlements | b | Placement stage (Luau) after biomes, on the object level of the seed hierarchy | NMS abandoned and ancient buildings ([Origins](https://nomanssky.com/origins-update/)) | Meters. |
| Names, languages and lore | b | Seeded Luau text generators per id | NMS | — |
| Nature editor | b | Q5's graph must be editable data with a UI later | Owner's scope | — |
| Galaxy map, system map, navigation UI | b | Reads Q3's query API; a module | Elite galaxy map | — |
| Creator-authored named systems inside a procedural galaxy | b | Override stage: data replaces generated records for one id | Elite's catalog stars ([80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)) | Authored values are physical. |
| Localization, accessibility, VR, consoles | c | Out of the product's current platform and scope ([VISION](../VISION.md) non-goals) | — | — |
| Anti-cheat | c | Depends on Q6's multiplayer scope; server authority covers the basics | — | — |

## What each question must now answer

The (a) items above, per question:

- **Q1 coordinates and frames:** the frame tree including ship interiors; a galaxy position that is sector + local;
  positions in Luau, JSON and MCP; camera-relative extraction for both renderers; "up" as a function of position.
- **Q2 mechanics and time:** int64 tick clock with phase from `tick mod period`; orbit tree of barycenters (multiple
  stars, moons of moons); a Kepler solver for e → 1 and hyperbolic orbits; per-body solar and sidereal time,
  seasons, tidal locking; occlusion queries for eclipses; time warp policy.
- **Q3 generation pipeline:** galaxy distribution and octree ids; queryable galaxy; star class → luminosity and color;
  snow line, Hill spacing, Roche limit, S/P orbit limits; derived physics, retention, greenhouse, climate, biomes;
  non-local whole-system and whole-body stages; body-kind registry; diffuse objects for nebulae.
- **Q4 terrain:** bodies without surfaces, asteroids, water bodies, caves, building frames on a sphere, a chunk-ready
  hook for derived data, streaming rate as a function of Q10's approach speed.
- **Q5 procgen graph:** stage hooks in Luau and C++; deterministic math for hooks; GPU output visual-only by default;
  coarse cached stages next to chunk-local ones.
- **Q6 networking:** universe manifest handshake; server-owned clock; interest management across frames; ids as
  strings on JSON wires.
- **Q7 simulation LOD:** catch-up from the clock; aggregate ecosystems; economy, traffic, factions; automated bases;
  weather and eruptions as system edits.
- **Q8 renderer:** N suns; atmosphere per body and from orbit; auto-exposure; impostors and the far-body handoff; 3D
  textures and indirect draw as prerequisites; separate human-scale fog.
- **Q9 physics:** body-fixed frames without fictitious forces by default; moving ship frames; per-body gravity.
- **Q10 gravity, flight and travel:** see below.
- **Q11 persistence and versioning:** see below.
- **Q12 numeric determinism:** see below.

## Questions added or split

**Q10: Gravity, flight and travel (added).** The brief lists gravity and flight in the gap analysis but no question
owns them: Q1 owns frames, Q2 orbits, Q9 the physics engine. Yet the first slice is "fly down to the surface with no
loading screen and land", and the approach speed is the number Q4's streaming, Q1's frame switches and Q8's
impostor handoff are all sized against. Scope: spherical gravity and orientation, sphere-of-influence handoff, the
flight-model seam (Newtonian, assisted, arcade as modules), travel tiers and speed limits near bodies, atmospheric
entry from the shared atmosphere record, landing on rotating bodies and moving stations, FTL between systems. It
depends on Q1 and Q2 and constrains Q4, so it should be answered right after them.

**Q11: Persistence and generator versioning (split out of Q3 and Q6).** Q3 carries generator versioning and edit
storage and Q6 carries save formats; they are one problem (what is stored, keyed by what, and how it survives a
generator change) and it is the most expensive thing to get wrong: a bad choice invalidates every save and every
server world. NMS Origins and Elite show it shapes what a studio can ship for years. Scope: stage versions per body,
the universe manifest (seed, generator versions, mod hashes, scale), player and system edits as deltas, the save
format, recordings at universe scale. Q3 keeps the pipeline itself.

**Q12: Numeric determinism across platforms and CPU/GPU (split out of Q3).** It is a contract the whole engine signs,
not a procgen detail: Q2 (orbit evaluation), Q5 (GPU and Luau generators), Q6 (what is regenerated instead of sent),
Q9 (physics replay) and record/replay all depend on it. It must be decided before Q5 and Q6 commit to "regenerate
instead of sync". Scope: which code paths are authoritative; math library and compiler flags on Clang and MSVC; GPU
fast-math settings in the Slang to Metal and DXIL builds; Luau's math; a CI test comparing stage hashes between the
macOS arm64 gate and Windows x64 CI.

**Not added:**

- **Time** stays in Q2: the brief's Q2 already covers the clock, calendars and warp. Q2 gains eclipse occlusion
  queries.
- **Audio** gets no question. Its only founding constraints (listener in the camera-relative frame, propagation from
  the atmosphere record) are folded into Q1 and Q3; VCH-029 should be expanded with them.
- **Galaxy map and UI** need only Q3's query API.

## Sources

Games and tools:

- Innes McKendrick, "Continuous World Generation in No Man's Sky", GDC 2017: https://gdcvault.com/play/1024265/Continuous-World-Generation-in-No
  and the write-up https://gamedeveloper.com/programming/video-how-continuous-world-generation-works-in-i-no-man-s-sky-i-
- Sean Murray, "Building Worlds Using Math(s)", GDC 2017: https://gdcvault.com/play/1024514/Building-Worlds-Using
- "The Sound of No Man's Sky", GDC 2017: https://www.gdcvault.com/play/1024067/The-Sound-of-No-Man
- No Man's Sky Origins update notes: https://nomanssky.com/origins-update/
- No Man's Sky Worlds Part II: https://news.xbox.com/en-us/2025/01/29/no-mans-sky-worlds-part-2-out-today/,
  https://www.shacknews.com/article/142889/no-mans-sky-update-55-patch-notes,
  https://www.pushsquare.com/news/2025/01/no-mans-sky-gets-another-huge-game-changing-update-with-worlds-part-ii
- Sean Murray on NMS physics (Inverse): https://www.inverse.com/article/19754-no-man-s-sky-physics-explained
- Doc Ross interview on Stellar Forge (80.lv, 2018): https://80.lv/articles/generating-the-universe-in-elite-dangerous/
- Elite Dangerous explore page: https://www.elitedangerous.com/explore
- Braben on Stellar Forge (Space.com): https://www.space.com/31366-elite-dangerous-stellar-forge-interview.html
- Space games and scale (Space.com; Frontier, Egosoft): https://www.space.com/entertainment/space-games/why-space-games-still-struggle-with-the-scale-of-the-universe
- Frontier forum posts: https://forums.frontier.co.uk/goto/post?id=5935112, https://forums.frontier.co.uk/goto/post?id=8957363
- Elite Odyssey planets (headline only): https://www.pcgamer.com/heres-how-frontier-rebuilt-a-galaxys-worth-of-planets-for-elite-dangerous-odyssey
- Sean Tracy on Star Citizen 64-bit: https://gamersnexus.net/gg/2622-star-citizen-sean-tracy-64bit-engine-tech-edge-blending
- Object Container Streaming (community wiki): https://starcitizen.tools/Object_Container_Streaming
- KSP API, Krakensbane: https://www.kerbalspaceprogram.com/ksp/api/class_krakensbane.html; FloatingOrigin:
  https://kspmoddinglibs.github.io/KSPDocsSite/class_floating_origin.html; Kraken: https://wiki.kerbalspaceprogram.com/wiki/Kraken
- KSP scale and ScaledSpace (forums): https://forum.kerbalspaceprogram.com/topic/140724-the-size-of-spores-planets,
  https://forum.kerbalspaceprogram.com/topic/33402-planets-and-scaledspace/page/2
- Space Engineers planets: https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/,
  https://blog.marekrosa.org/2015/04/guest-post-by-ondrej-petrzilka-space_17/,
  https://www.moddb.com/news/update-01044-spherical-gravity-generator-modding-for-prefab-and-respawn-ship
- Outer Wilds: https://gdconf.com/news/see-4d-level-design-outer-wilds-deconstructed-gdc-2020,
  https://media.gdcvault.com/GDC+2021/Andrew+Woodward_OuterWildsPoster.pdf,
  https://www.vice.com/en/article/pkpde9/how-outer-wilds-developers-mobius-digital-made-space-physics-fun
- Astroneer: https://www.unrealengine.com/en-US/spotlights/how-system-era-softworks-leveraged-ue4-to-create-astroneer-s-wonderful-universe
- SpaceEngine: https://en.wikipedia.org/wiki/SpaceEngine

Science:

- Kopparapu et al. 2013, Habitable zones around main-sequence stars: https://ui.adsabs.harvard.edu/abs/2013ApJ...765..131K/abstract;
  2014 mass dependence: https://dx.doi.org/10.1088/2041-8205/787/2/l29
- Zahnle & Catling 2017, The Cosmic Shoreline: https://arxiv.org/abs/1702.03386
- Tidal despinning (Gladman et al. 1996, as used in): https://arxiv.org/abs/1204.3923, https://arxiv.org/pdf/2002.11388
- Reference formulas (not fetched, marked unverified in the notes): https://en.wikipedia.org/wiki/Planetary_equilibrium_temperature,
  https://en.wikipedia.org/wiki/Atmospheric_escape, https://en.wikipedia.org/wiki/Roche_limit,
  https://en.wikipedia.org/wiki/Hill_sphere, https://en.wikipedia.org/wiki/Sphere_of_influence_(astrodynamics),
  https://en.wikipedia.org/wiki/Stellar_classification, https://en.wikipedia.org/wiki/Mass%E2%80%93luminosity_relation,
  https://nssdc.gsfc.nasa.gov/planetary/factsheet/earthfact.html
- Génevaux et al. 2013, hydrology-based terrain (unverified): https://doi.org/10.1145/2461912.2461996

Determinism:

- Glenn Fiedler, Floating Point Determinism: https://gafferongames.com/post/floating_point_determinism/
- Unity discussion, Intel vs AMD determinism: https://discussions.unity.com/t/floating-point-determinism-in-unity-intel-vs-amd/813566
- Metal fast math default (unverified): https://developer.apple.com/documentation/metal/mtlcompileoptions/fastmathenabled
