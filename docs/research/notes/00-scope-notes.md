# Scope gap analysis: raw notes

Raw research notes behind [00-scope.md](../00-scope.md). Collected 2026-11-27. Every claim carries a link; claims
taken from memory of standard references without fetching the page are marked **(unverified)**.

## Q1: What do shipped space games have that the owner's scope lacks, and how did they solve it?

### Takeaway
Every shipped game in the genre had to make explicit choices the scope is silent on: a travel model with speed tiers
(Elite, NMS), a hierarchical object id that doubles as the seed path (Elite), 64-bit or rebased positions (Star
Citizen, KSP, Elite), on-rails orbits with a physics bubble around the player (KSP), a policy for changing procgen
without destroying saves (NMS Origins), and moving local frames for ships and bases (Space Engineers, Star Citizen).
None of these is in the scope as written; most of them constrain founding decisions.

### Cited Findings

**No Man's Sky (Hello Games)**
- GDC 2017 "Continuous World Generation in No Man's Sky" (Innes McKendrick) covers the planet generation
  architecture and the systems that run it continuously in real time, from voxel generation through polygonization —
  [GDC Vault](https://gdcvault.com/play/1024265/Continuous-World-Generation-in-No);
  [Game Developer write-up](https://gamedeveloper.com/programming/video-how-continuous-world-generation-works-in-i-no-man-s-sky-i-)
- GDC 2017 "Building Worlds Using Math(s)" (Sean Murray) — [GDC Vault](https://gdcvault.com/play/1024514/Building-Worlds-Using)
- GDC 2017 "The Sound of No Man's Sky" (procedural audio) — [GDC Vault](https://www.gdcvault.com/play/1024067/The-Sound-of-No-Man)
  (session content not fetched; that NMS uses procedural creature vocals and ship audio is **unverified** here)
- Origins (3.0, 2020) patch notes: "Terrain generation for existing planets has not been affected, and player bases
  have not been reset." "Creature generation has been reset", with old discoveries moved to an "Extinct" section.
  "No existing planet will have its fundamental type or hazard changed." "New planets have been created in most star
  systems." Binary stars and rare ternary stars added; volcanoes on planets with disrupted cores; tornadoes,
  lightning, meteor showers, firestorms, "strange gravitational events", sandworms, abandoned and ancient buildings —
  [nomanssky.com Origins](https://nomanssky.com/origins-update/)
- Sean Murray on Origins: "We didn't want to change the terrain of existing planets... So the idea that we came up
  with was to introduce new planets" — quoted in search summary of Origins coverage
  ([TechRadar](https://www.techradar.com/news/no-mans-sky-origins-30-update), quote via search, page not fetched)
- One player reported "all the flora has gone on their home planet" after Origins (anecdote) —
  [GOG forum](https://gog.com/forum/no_mans_sky/origins_next_week/page2)
- Worlds Part II (5.5, 29 Jan 2025): billions of new star systems added while explored and customized planets stay;
  gas giants about ten times the size of the largest planets; deeper oceans, new terrain, lighting overhaul —
  [Xbox Wire](https://news.xbox.com/en-us/2025/01/29/no-mans-sky-worlds-part-2-out-today/);
  [Shacknews patch notes](https://www.shacknews.com/article/142889/no-mans-sky-update-55-patch-notes);
  [Push Square](https://www.pushsquare.com/news/2025/01/no-mans-sky-gets-another-huge-game-changing-update-with-worlds-part-ii);
  [Neowin](https://www.neowin.net/news/no-mans-sky-worlds-part-ii-expands-the-universe-with-gas-giants-water-worlds-and-more/)
- Murray (2016) described the day-night cycle as "the planet is rotating on its axis as it spins around the sun" —
  [Inverse](https://www.inverse.com/article/19754-no-man-s-sky-physics-explained). In the shipped game planets do not
  visibly orbit (common player observation; **unverified**, no developer statement found).

**Elite Dangerous (Frontier, Stellar Forge)**
- About 400 billion star systems in a 1:1 Milky Way — [elitedangerous.com](https://www.elitedangerous.com/explore);
  Frontier says nobody has counted them exactly ([forum](https://forums.frontier.co.uk/goto/post?id=700183))
- Doc Ross (Lead Render Programmer) interview, 80.lv, 5 Apr 2018 — [80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/):
  - top-down: galaxy mass and age distribution functions plus a hand-made distribution map (arms, bulge), "one of the
    only non-programmatic resources";
  - sectors in an eight-layer octree; each primary star gets type and age consistent with the distributions;
  - leftover material forms a protoplanetary disc whose element distributions accumulate over simulated epochs;
    proto-planets are tracked for solar wind, catastrophic events, **tidal locking**, gravitational heating;
  - crust/mantle/core temperature, tidal and stellar heating, trapped gases, composition and volcanic parameters;
  - planet parameters set ranges for tectonics, volcanism, basins, mountains, fissures, craters, ice (also influenced
    by nearby rings); averaged properties modulate GPU noise; "All of these values are simulated according to
    calculations of their plausible ranges";
  - **a 64-bit integer encodes sector coordinates, octree layer, system id and body id**, so client and server agree
    on the same object; noise takes the surface point and a unique planet id;
  - **64-bit doubles and emulated dual-float** math give millimeter accuracy across planet-sized coordinates;
  - Hipparcos and Gliese catalogs "seed our generated Milky Way with real stars".
- 160,000 systems from star catalogs, the rest generated — [Space.com interview with Braben](https://www.space.com/31366-elite-dangerous-stellar-forge-interview.html)
- Travel tiers: normal space (~300 m/s), supercruise in-system, hyperspace between systems, later Supercruise
  Overcharge; systems generated once on discovery and regenerated the same way on later visits (Gauthier Verquerre) —
  [Space.com](https://www.space.com/entertainment/space-games/why-space-games-still-struggle-with-the-scale-of-the-universe)
- A Frontier forum answer: making alterations now "would upset the proc-gen of the entire Galaxy" (search summary;
  exact post **unverified**) — [Frontier forums](https://forums.frontier.co.uk/goto/post?id=5935112)
- Odyssey rebuilt the planet terrain generation for the whole galaxy —
  [PC Gamer headline](https://www.pcgamer.com/heres-how-frontier-rebuilt-a-galaxys-worth-of-planets-for-elite-dangerous-odyssey)
  (article body not retrievable; details **unverified**)
- Kay Ross livestream summary: raw inputs (mass distribution, sector ages, stellar lifetime equations) generate the
  galaxy procedurally and deterministically — [Frontier forum summary](https://forums.frontier.co.uk/goto/post?id=8957363)

**Star Citizen (Cloud Imperium)**
- The 64-bit change targeted positioning and physics, not the whole engine; no direct speed gain (Sean Tracy) —
  [Gamers Nexus](https://gamersnexus.net/gg/2622-star-citizen-sean-tracy-64bit-engine-tech-edge-blending)
- Object Container Streaming: unobserved objects' state can be frozen; server-side streaming leads to static, then
  dynamic, server meshing — [Star Citizen wiki (community)](https://starcitizen.tools/Object_Container_Streaming)
- 32-bit coordinates at those distances give errors of meters, seen as jitter of the player's arms —
  [RSI Spectrum thread (community)](https://robertsspaceindustries.com/spectrum/community/SC/forum/50259/thread/understanding-64-bit-coordinate-system-and-possibl)
- "Zone system and local grids" for physics inside moving ships: mentioned in forums only, **unverified**.

**Kerbal Space Program (Squad)**
- Krakensbane shifts the frame origin and velocity so the active vessel stays near the origin and moves slowly
  relative to the physics frame; engaged when a vessel comes off rails —
  [KSP API: Krakensbane](https://www.kerbalspaceprogram.com/ksp/api/class_krakensbane.html)
- FloatingOrigin shifts the origin to a given position; modders report it kicks in after ~2.5 km —
  [KSP API: FloatingOrigin](https://kspmoddinglibs.github.io/KSPDocsSite/class_floating_origin.html);
  [forum](https://forum.kerbalspaceprogram.com/topic/41665-wip-mass-driver/page/4)
- Bodies move on rails, unaffected by physical interaction; vessels follow patched conics —
  [KSP wiki: Kraken](https://wiki.kerbalspaceprogram.com/wiki/Kraken)
- Stock KSP is about 1/10 real scale; very small rescales "break too much" —
  [KSP forum](https://forum.kerbalspaceprogram.com/topic/140724-the-size-of-spores-planets). Kerbin's radius is
  600 km with 9.81 m/s² surface gravity (**unverified** here, standard KSP wiki values).
- Planets use a separate "ScaledSpace" layer for distant rendering — [KSP forum](https://forum.kerbalspaceprogram.com/topic/33402-planets-and-scaledspace/page/2)
  (the scale ratio and camera stacking are **unverified**)
- n-body gravity exists only as a mod (Principia) — [forum](https://forum.kerbalspaceprogram.com/topic/162200-wip181-191-1101-1110%E2%80%932-1122%E2%80%935-principia%E2%80%94releases-every-new-moon%E2%80%94n-body-and-extended-body-gravitation/page/6)

**Space Engineers (Keen)**
- Planets (Nov 2015) are volumetric, fully destructible and persistent; gravity works like real gravity —
  [Marek Rosa's blog](https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/)
- Planet gravity scales with planet size and falls off with distance; applied to ships too —
  [Ondrej Petrzilka guest post](https://blog.marekrosa.org/2015/04/guest-post-by-ondrej-petrzilka-space_17/);
  [Dusan Andras guest post](https://blog.marekrosa.org/2015/06/guest-post-by-dusan-andras-space/)
- Block grids cannot be axis-aligned on a sphere; a "station voxel" option keeps a station static while touching the
  planet — same Rosa post; artificial and spherical gravity generator blocks —
  [ModDB update 01.044](https://www.moddb.com/news/update-01044-spherical-gravity-generator-modding-for-prefab-and-respawn-ship)

**Outer Wilds (Mobius)**
- The ship and the bodies follow exaggerated Newtonian physics; planets orbit quickly and have their own gravity
  fields — [Vice](https://www.vice.com/en/article/pkpde9/how-outer-wilds-developers-mobius-digital-made-space-physics-fun);
  [Giant Bomb blog](https://www.giantbomb.com/profile/gamer_152/blog/push-and-pull-physics-in-outer-wilds/264418/)
- "4D level design": levels that change over a 22-minute loop (time as content) —
  [GDC 2020 announcement](https://gdconf.com/news/see-4d-level-design-outer-wilds-deconstructed-gdc-2020);
  "truth in simulation" — [GDC 2021 poster](https://media.gdcvault.com/GDC+2021/Andrew+Woodward_OuterWildsPoster.pdf)
- Moving the whole scene around the player: Reddit claim only, **unverified**.

**Astroneer (System Era)**
- Voxel planets with layered noise, reshaped by the Terrain Tool; caves to the core in 1.0; UE4 —
  [Unreal Engine spotlight](https://www.unrealengine.com/en-US/spotlights/how-system-era-softworks-leveraged-ue4-to-create-astroneer-s-wonderful-universe)
- Gravity on its small planets: sources conflict (fan wikis) — [astroneer.wiki.gg](https://astroneer.wiki.gg/wiki/Planets)

**SpaceEngine**
- Real catalogs (Hipparcos, NGC/IC, known exoplanets, clusters, nebulae) plus procedural fill of a universe over 10
  billion parsecs; galaxies fixed and non-rotating, no proper motion, no intrinsic variable stars; gravity simulated
  only for bodies in a system and the player's craft — [Wikipedia: SpaceEngine](https://en.wikipedia.org/wiki/SpaceEngine)

**X4 (Egosoft), for travel design**
- Tiered engines, gates, time acceleration and "highways" to compress travel; realism gives way when it fights fun
  (Bernd Lehahn) — [Space.com](https://www.space.com/entertainment/space-games/why-space-games-still-struggle-with-the-scale-of-the-universe)

### Inferences
- A **travel model** (speed tiers tied to gravity wells and streaming budgets) is a founding decision nobody in the
  scope owns. Elite and X4 show it is the main tool for making scale playable; it also sets the terrain streaming
  rate Q4 must sustain.
- Elite's **64-bit body id = path in the octree** is the cleanest precedent for the seed hierarchy: an id that is
  both an address and a seed. Q3 should adopt something like it.
- NMS shows the **generator versioning policy** that survives: freeze the terrain function of existing bodies, add
  new content as new bodies, allow resetting layers nobody builds on (creatures). That is a policy, and the
  architecture must make it possible (versioned generators per layer).
- Space Engineers and Star Citizen show **moving local frames** (ship grids, interiors) are part of ship building and
  multiplayer, and that "up" on a sphere breaks grid-aligned building.
- Every precedent keeps bodies **on rails** and simulates real physics only near the player (KSP, Elite,
  SpaceEngine). Outer Wilds is the exception, with one tiny system.

### Gaps
- No primary technical source on Outer Wilds' floating origin or its physics integrator.
- No primary source for Star Citizen's physics grids / zones.
- Elite Odyssey's terrain rebuild details (article body not retrievable).
- NMS rivers: whether shipped NMS has rivers was not checked.

## Q2: What does astronomy and planetary science say a physically coherent staged pipeline needs?

### Takeaway
A coherent pipeline needs a small set of physical inputs (stellar mass and age; body mass, radius, composition,
orbit, spin, obliquity) and a fixed order of derived quantities: luminosity and temperature of the star; instellation
and equilibrium temperature; escape velocity and atmosphere retention; greenhouse; climate; biomes. Several items are
non-local (orbital stability of the whole system, Roche limits for rings, snow line for giant planets, drainage for
rivers) and force stages that see a whole system or a whole planet before chunks are generated.

### Cited Findings
- Kopparapu et al. 2013, ApJ 765:131: 1D cloud-free climate model, HZ for stars of 2600-7200 K, given in parametric
  form; Solar System inner (water-loss) limit ~0.99 AU, outer (maximum greenhouse) ~1.70 AU; recommends **incident
  flux, not equilibrium temperature**, to drop the albedo dependence; omits clouds, so real limits may be wider —
  [ADS](https://ui.adsabs.harvard.edu/abs/2013ApJ...765..131K/abstract); erratum ApJ 770:82
  ([HAL](https://hal.archives-ouvertes.fr/hal-01016707)); mass dependence 0.1-5 Earth masses in Kopparapu et al.
  2014 ApJL 787:L29 ([IOP](https://dx.doi.org/10.1088/2041-8205/787/2/l29))
- Zahnle & Catling 2017 "The Cosmic Shoreline", ApJ 843:122: bodies with and without atmospheres separate along a line
  in (escape velocity, insolation/XUV) space; empirically I ∝ v_esc⁴; energy-limited escape predicts I ∝ v_esc³√ρ;
  impact erosion also fits v_esc⁴ — [arXiv:1702.03386](https://arxiv.org/abs/1702.03386)
- A 2025 revisit adds a hydrodynamic-escape-based retention metric and notes volatile content variations —
  [arXiv:2504.19872](https://arxiv.org/html/2504.19872v2)
- Tidal despinning timescale (Gladman et al. 1996, as used in later papers): τ ∝ ω·a⁶·I·Q / (G·M²·k₂·R⁵); a rigid
  icy body has Q/k₂ ≈ 10⁶; exact prefactors vary between sources —
  [arXiv:1204.3923](https://arxiv.org/abs/1204.3923); [arXiv:2002.11388](https://arxiv.org/pdf/2002.11388);
  [Centauri Dreams](https://www.centauri-dreams.org/2020/01/20/a-deep-dive-into-tidal-lock/)
- Standard relations used below, from textbook references (**unverified** here, not fetched):
  - equilibrium temperature T_eq = T★·√(R★/2a)·(1−A)^¼, i.e. 255 K for Earth (288 K observed: the ~33 K greenhouse)
    — [Wikipedia: Planetary equilibrium temperature](https://en.wikipedia.org/wiki/Planetary_equilibrium_temperature)
  - Jeans escape: a gas is retained over geological time if v_esc is several times (rule of thumb ~6×) its mean
    thermal speed √(8kT/πm) at the exobase — [Wikipedia: Atmospheric escape](https://en.wikipedia.org/wiki/Atmospheric_escape)
  - scale height H = kT/(m̄g), ~8.5 km for Earth — [NASA Earth fact sheet](https://nssdc.gsfc.nasa.gov/planetary/factsheet/earthfact.html)
  - main-sequence mass-luminosity L ∝ M^~3.5 and lifetime ∝ M/L —
    [Wikipedia: Mass–luminosity relation](https://en.wikipedia.org/wiki/Mass%E2%80%93luminosity_relation)
  - spectral classes O B A F G K M (plus L T Y brown dwarfs) with surface temperatures from >30,000 K to <3,700 K —
    [Wikipedia: Stellar classification](https://en.wikipedia.org/wiki/Stellar_classification)
  - Roche limit (fluid) d ≈ 2.44·R_p·(ρ_p/ρ_m)^⅓; rings lie inside it, moons outside —
    [Wikipedia: Roche limit](https://en.wikipedia.org/wiki/Roche_limit)
  - Hill sphere r_H ≈ a·(m/3M)^⅓; prograde moons stable to about half of it; Laplace sphere of influence
    r_SOI = a·(m/M)^⅖ (what patched conics use) — [Hill sphere](https://en.wikipedia.org/wiki/Hill_sphere);
    [Sphere of influence](https://en.wikipedia.org/wiki/Sphere_of_influence_(astrodynamics))
  - multi-planet stability needs spacing of roughly ≥10 mutual Hill radii for long lifetimes (Chambers, Wetherill &
    Boss 1996, Icarus 119:261) — **unverified**
  - circumstellar (S-type) and circumbinary (P-type) stable orbit limits in binaries (Holman & Wiegert 1999, AJ
    117:621) — **unverified**
  - the snow (frost) line at ~2.7 AU for the Sun, scaling with √L (Hayashi 1981) — **unverified**
  - solar day = sidereal day / (1 − P_sid/P_orb); a tidally locked body has no solar day —
    [Wikipedia: Solar time](https://en.wikipedia.org/wiki/Solar_time) **unverified**
  - illuminance: sunlight ~100-130 klux at Earth, full moon ~0.1-0.3 lux, moonless starlight ~0.002 lux —
    [Wikipedia: Lux](https://en.wikipedia.org/wiki/Lux) **unverified**; ADR 0038 already uses 128,000 lux at the top
    of the atmosphere.
  - biome from annual temperature and precipitation (Whittaker diagram) —
    [Wikipedia: Biome](https://en.wikipedia.org/wiki/Biome) **unverified**
  - rivers and drainage generated from a hydrology graph (Génevaux et al. 2013, "Terrain generation using procedural
    models based on hydrology", SIGGRAPH) — [ACM DL](https://doi.org/10.1145/2461912.2461996) **unverified**

### Inferences
- Elite's Stellar Forge already follows this order (galaxy distributions → star type and age → disc → proto-planets
  with tidal locking and heating → surface parameter ranges → GPU noise). It is the proof that the staged pipeline is
  shippable at galaxy scale.
- Several stages are **non-local**: orbital spacing and stability (whole system), Roche limit and ring placement
  (planet and its moons), snow line (whole disc), drainage and rivers (whole planet), plate boundaries (whole planet).
  A pure point-evaluated f(seed, position) cannot express them. The pipeline needs whole-system and whole-body coarse
  stages, cached, before chunk-local generation.
- Kopparapu's flux-based HZ fits a game well: it is a parametric fit, cheap, deterministic, and independent of
  albedo; a two-star system sums fluxes.
- Atmosphere retention can be a two-parameter test (v_esc, instellation) after Zahnle & Catling, cheaper and more
  game-friendly than full Jeans escape per gas, with per-gas Jeans as an optional refinement.

### Gaps
- Exact prefactors of the tidal locking timescale (source disagreement); the pipeline only needs an order-of-magnitude
  test against system age.
- No source checked for a cheap climate model (energy balance with latitude, Budyko-Sellers); flagged for Q3.

## Q3: How do the gaps interact with the creator scale factor s?

### Takeaway
Most physically derived quantities depend on ratios and survive one uniform length scale unchanged (Roche limit,
angular sizes, eclipse geometry, atmosphere optical depth if coefficients are rescaled); the trouble comes from what
is deliberately held fixed (surface gravity, human-scale fog distances, illuminance) and from using separate planet and
system factors.

### Cited Findings
- KSP keeps Earth surface gravity on a ~1/10-radius planet — [KSP forum](https://forum.kerbalspaceprogram.com/topic/140724-the-size-of-spores-planets)
- Space Engineers scales gravity with planet size and distance instead — [Petrzilka](https://blog.marekrosa.org/2015/04/guest-post-by-ondrej-petrzilka-space_17/)

### Inferences (derivations, standard physics)
- g fixed and R → sR implies gameplay mass GM → s²·GM. Then v_esc = √(2gR) ∝ √s, low-orbit speed √(gR) ∝ √s,
  low-orbit period 2π√(R/g) ∝ √s. Earth's ~88 min low orbit becomes ~28 min at s = 0.1 (KSP's ~30 min low Kerbin
  orbit is consistent).
- Sphere of influence with gameplay masses: r_SOI ∝ a·(s²)^⅖ = a·s^0.8 relative to unscaled; if moon orbits scale
  with s, SOI/orbit ratio changes by s^-0.2 — small, but moons must be re-validated against the scaled SOI.
- Atmosphere: optical depth τ = ∫σ dl is dimensionless. Scaling all lengths (R, atmosphere top, H) by s and all
  scattering coefficients by 1/s leaves every planet-scale view identical (zenith, horizon, from orbit). But aerial
  perspective over human-scale distances (1 km to a hill) is then 1/s times hazier, so fog at human scale needs its
  own control.
- Illuminance: if orbit distances are scaled, E ∝ 1/(s_sys·a)² rises by 1/s_sys². Lighting must use the physical
  distance, as generation does, or the climate and the look disagree.
- Angular sizes of stars and moons are invariant under one factor but change with separate planet and system factors,
  which changes total vs annular eclipses.
- Horizon distance √(2Rh) ∝ √s; curvature becomes visible at lower altitude.
- Rotation period is radius-independent, so day length can stay physical; surface rotation speed 2πR/T ∝ s.

### Gaps
- Nobody has published how KSP handles fog and aerial perspective at its scale; the "1/s hazier" effect is derived,
  not measured.

## Q4: Missing or split questions

### Takeaway
The nine questions leave four orphans: the travel and flight model (gravity, orientation, approach speed, entry,
landing), persistence (save format, generator versioning, edits, mod identity), numeric determinism across platforms
and CPU/GPU, and (smaller) audio. The first three are founding decisions that cut across several questions.

### Cited Findings
- Floating-point results are reproducible across machines only with the same compiler, architecture and
  platform-specific care; not even between debug and release builds by default —
  [Fiedler, Floating Point Determinism](https://gafferongames.com/post/floating_point_determinism/);
  [Fiedler, Deterministic Lockstep](https://gafferongames.com/post/deterministic_lockstep/)
- IEEE-754 basic operations are deterministic on modern consumer hardware, but transcendental functions are not and
  need software implementations — [Unity forum thread](https://discussions.unity.com/t/floating-point-determinism-in-unity-intel-vs-amd/813566)
- Cross-platform RTS sync argues for fixed point —
  [Game Developer](https://gamedeveloper.com/programming/cross-platform-rts-synchronization-and-floating-point-indeterminism)
- Metal compiles shaders with fast math enabled by default (`MTLCompileOptions.fastMathEnabled`, superseded by
  `mathMode`) — [Apple docs](https://developer.apple.com/documentation/metal/mtlcompileoptions/fastmathenabled)
  **unverified** (not fetched); whether Veach's offline Slang → metallib build sets it must be checked in the repo.
- Veach facts from the repo: Luau `vector` is float32 vec3 (ADR 0013); positions are float32 glm (ADR 0013); audio is
  only a backlog line (VCH-029); there is no save system; the job system is VCH-015.

### Inferences
- See 00-scope.md "Questions added or split".

### Gaps
- No measured data on Apple GPU vs DX12/WARP float differences for procgen kernels; that is spike material.
