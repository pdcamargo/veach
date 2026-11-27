# 0044: Counter-based hash randomness for generation and simulation

Date: 2026-11-27

## Status

Accepted.

## Context

Veach has one source of randomness: `veach::Rng` (`engine/core/include/veach/core/rng.hpp`), an integer PCG32 stream
with plain, reflectable state, held by the `veach::Random` singleton and seeded from `--seed`. It is bit-exact on
every platform, but it is a stream: each result depends on how many draws came before it.

Generation and simulation LOD can't work that way. Generation is lazy and streamed: system 7 must be generated without
systems 0 to 6, chunks arrive in camera order, and stages run on worker jobs in any order. A mod that adds one draw
must not reshuffle everything generated after it. Simulation LOD ([0057](0057-simulation-lod.md)) must produce the same
result whether a region was stepped live or caught up later, which means a random decision at tick t must not depend
on whether ticks t−1, t−2... were simulated. Minecraft shows the trap in the other direction: its feature salt is a
list position (`index + 10000 * step`), so inserting a feature reseeds every feature after it.

Counter-based generators take a key and return bits with no state: Eiserloh's noise-based RNG (SquirrelNoise5),
Random123's Philox and Threefry, PCG-style hash finalizers. Integer-only, so bit-exact on CPUs and GPUs.
Research: [12-determinism](../research/12-determinism.md) §2 and rule 6,
[03-generation-pipeline](../research/03-generation-pipeline.md) §7.2,
[07-simulation-lod](../research/07-simulation-lod.md).

## Decision

**The hash** lives in `engine/core/include/veach/core/hash.hpp` (generic; [0042](0042-space-toolbox-layering.md)):

- `std::uint64_t veach::hash(std::uint64_t seed, std::uint64_t domain, std::uint64_t id, std::uint64_t index)`, plus
  overloads for 1 to 6 key words. Each key word is absorbed through the SplitMix64 finalizer (`z ^= z >> 30;
  z *= 0xBF58476D1CE4E5B9; z ^= z >> 27; z *= 0x94D049BB133111EB; z ^= z >> 31`) chained with the golden-ratio
  increment `0x9E3779B97F4A7C15`. The exact composition is fixed by test vectors committed in
  `tests/golden/determinism/hash.json` with VCH-077 and never changes afterwards without a determinism contract
  bump ([0055](0055-universe-identity-and-versioning.md)).
- `std::uint64_t veach::hash_id(std::string_view)`: 64-bit FNV-1a over the UTF-8 bytes of a stable string id
  (`"space:system"`, `"layer:trees"`). **Domains and salts are always hashes of stable names, never list positions,
  load order or enum values.**
- Conversions: `unit_double(h) = (h >> 11) * 0x1p-53` in [0, 1); `unit_float(h) = (h >> 40) * 0x1p-24`;
  `range(h, lo, hi)` by 128-bit multiply-high (bias below 2^-32 for spans under 2^32). Integer ops only, so these are
  canonical ([0043](0043-determinism-tiers-and-strict-floating-point.md)).

**Key layout.** The four words mean, in order:

| Word | Generation | Simulation LOD |
|---|---|---|
| `seed` | stage seed ([0050](0050-universe-addresses-and-seeds.md)) | universe seed |
| `domain` | `hash_id` of the sub-stream (`"orbits"`, `"spins"`, a field-graph node id) | `hash_id` of the sim domain (`"weather"`, `"population"`) |
| `id` | address of the record, chunk or object | sim cell or entity's persistent id |
| `index` | draw number or instance index | the tick, or tick / coarse period |

**`veach::HashStream { std::uint64_t key; std::uint64_t counter; }`** gives stage code a readable sequence
(`next()` returns `hash(key, counter++)`) while staying counter-based: the key is `hash(stage_seed,
hash_id(substream), address)`, the counter starts at 0, and each named sub-stream (`planet_masses`, `orbits`, `spins`)
has its own key, so a new draw in spin generation never moves orbits. Its state is two integers, reflected.

**Where each source is allowed:**

| Source | Allowed in | Banned in |
|---|---|---|
| `veach::hash`, `HashStream` | canonical and replay code, Luau generators (`veach.hash`, `ctx.stream(name)`), sim LOD | — |
| `veach::Rng` / `veach::Random` | live gameplay (replay tier): AI choices, weapon spread, particles' CPU side | canonical files (lint, [0043](0043-determinism-tiers-and-strict-floating-point.md)) |
| Luau `math.random` | gameplay scripts (replay tier) | the procgen sandbox ([0052](0052-procgen-runtime.md)) |
| `std::random_device`, `rand()`, `<random>` | nowhere in simulation | everywhere in simulation |

`veach::Rng` stays as it is and gains `next_double()` (53 bits) and an unbiased `range`. When live gameplay needs a
stream tied to generated content (a creature's behavior seed), it seeds an `Rng` from a `veach::hash` of that content's
id.

**GPU.** `veach_hash32` in `engine/render/shaders/veach/hash.slang` and its C++ twin `veach::hash32` take 32-bit words
(a PCG-family hash, Jarzynski & Olano 2020); 64-bit keys are split into two `uint`. 64-bit shader integers are not
used (optional on DX12). The existing path-tracer `Rng` in `random.slang` is unchanged.

**Luau.** Generators see `veach.hash(...)` and per-context streams; ids cross as tagged userdata or strings
([0046](0046-64-bit-values-across-luau-json-mcp.md)), never as numbers, so no key loses bits above 2^53.

## Consequences

- Any record, chunk or object can be generated alone, in any order, on any worker, with the same result.
- Catch-up and live simulation draw the same numbers for the same tick, which makes the CI equivalence test of
  [0057](0057-simulation-lod.md) possible.
- Mods add domains by name; they cannot perturb other content's draws.
- The hash and `hash_id` are part of the universe's identity. Pinning test vectors from day one is mandatory.
- Two random APIs exist; the lint keeps `Rng` out of canonical code.

**Validated by** VCH-077 (the committed hash vectors reproduced on the macOS gate and Windows CI), VCH-081 (100,000
systems: stage hashes equal across thread counts and scales; histograms show no artifacts) and VCH-093 (live and
caught-up hashes equal). **Reopen if** VCH-081's histograms or a PractRand run over `HashStream` show correlation
between neighboring keys, or `veach_hash32` is not bit-equal to `veach::hash32` on Metal and WARP.

## Alternatives considered

- **One `veach::Rng` stream walked down the generation tree:** order-dependent, O(n) random access, and any added draw
  shifts the rest.
- **Hash to seed a PCG32 stream per record** (Minecraft's population seeds): workable inside one record, but a stream
  can't be keyed by tick for catch-up, and two kinds of state for one job is one too many. `HashStream` reads the same.
- **Philox4x32 everywhere:** strong and GPU-friendly, but heavier than needed on the CPU; the 32-bit GPU hash already
  covers shaders.
