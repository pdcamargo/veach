# 0050: Universe addresses and hashed seeds

Date: 2026-11-27

## Status

Accepted.

## Context

Every generated thing (system, body, ring, surface region, chunk, tree, spawn point) needs a stable id. The id names it in saves, on the wire, in discoveries and over MCP, and it seeds its generation. A
procedural universe can't store ids for everything it could generate, so ids must be computed from where a thing is
in the hierarchy. The layout ends up in every save, packet and delta: changing it later means migrating every stored
reference ([0055](0055-universe-identity-and-versioning.md) keeps addressing outside versioning for that reason).

Precedents: No Man's Sky's fixed 48-bit address (16 bodies per system at most); Elite's 64-bit layered-octree id, where
massive systems live in coarse cells so a sky query visits a bounded set of cells per layer; Minecraft's hashed chunk
seeds, salted by list position, so inserting a feature reseeds the rest. Research: [03-generation-pipeline](../research/03-generation-pipeline.md)
§2.1, §2.2, §7.1, §7.2.

## Decision

**`SystemAddress` is 64 bits, a layered octree** (toolbox, `space/`). Cell edge at layer L is c₀·2^L with
c₀ = 10 ly (10 ly at layer 0 to 1,280 ly at layer 7):

| Field | Bits at layer 0 | Bits at layer L | Meaning |
|---|---|---|---|
| layer | 3 | 3 | 8 layers; brighter (more massive) stars live in coarser layers |
| cell x, z | 14 + 14 | 14 − L each | 163,840 ly span |
| cell y | 11 | 11 − L | 20,480 ly thick: disk, bulge, part of the halo |
| system index | 13 | 13 + 3L | systems within the cell (8,192 per 10 ly cell at layer 0) |
| body index | 9 | 9 | 0 = primary; 1-255 generated bodies, moons, rings and belts; 256-511 reserved for mods |

The fields always sum to 64 bits. The maximum galaxy is therefore 163,840 × 163,840 × 20,480 ly. The default galaxy
is Milky Way-sized (100,000 ly), a creator setting within that bound (pending the owner's review packet). A system's
`GalaxyPosition` (int64 km, [0045](0045-large-world-positions-and-reference-frames.md)) is its cell origin plus its
generated offset in the cell; addresses never encode meters or anything scaled.

**Typed sub-addresses compose below the system:**

- `BodyAddress`: a `SystemAddress` with its body index set.
- `SurfaceKey` (64 bits): cube face (3), level (5), quadtree u and v (28 each), for coarse body fields and surface
  regions; ~4 cm cells on an Earth-size face at s = 1.
- `ChunkKey`: `{ BodyAddress body; std::uint8_t level; std::int32_t x, y, z; }` in the body-local terrain octree
  ([0053](0053-terrain-density-field.md)). A struct: 22 levels need more than 64 bits.
- `ObjectAddress`: `{ ChunkKey chunk; std::uint32_t layer; std::uint32_t index; }` (the chunk names the body), with `layer`
  the low 32 bits of `hash_id` of the placement layer's stable id (`"layer:trees"`) and `index` stable within the
  chunk per layer version.

**Text forms.** Every address has one canonical string, e.g. `"S:3f2a91c07e5b0004/B:4/C:12:-4:7:301/L:trees/I:117"`.
JSON and MCP carry that string; Luau sees an opaque tagged `Address` userdata with `tostring`, equality and accessors, never a number
([0046](0046-64-bit-values-across-luau-json-mcp.md)). A short checksummed share code is a later UI feature.

**Seeds are hashed from addresses** with the counter-based hash of [0044](0044-counter-based-hash-randomness.md):

```text
stage_seed = veach::hash(universe_seed, address_word, veach::hash_id(stage_id), stage_version)
```

where `address_word` is the 64-bit system address, or the hash of the composed sub-address. Inside a stage, each named
sub-decision draws from its own `HashStream` keyed by `hash_id` of its name (`"planet_masses"`, `"orbits"`,
`"spins"`). Field-graph node seeds are `hash(body_seed, hash_id(node_id))` ([0052](0052-procgen-runtime.md)). **Salts
are hashes of stable string ids, never list positions or load order.** The stage version is in the key, so a version
bump reseeds only that stage's output.

**The universe seed** is a `uint64` in the genesis ([0055](0055-universe-identity-and-versioning.md)), written there as
a decimal string. A creator may enter any text: a decimal `uint64` is used as is, anything else becomes
`hash_id(text)`. It is not `--seed`, which keeps seeding `veach::Random` for live gameplay; the lab may default one
from the other.

**Addressing is version 1 forever.** New content claims new address space (body indices 256-511, new placement layers,
new deep-sky catalog entries). Anything that would renumber existing addresses is not allowed; reclassifying content
at an existing address is a stage version bump handled by pinning.

**Where it lives.** Addresses, their layout and text forms are toolbox (`space/`). The hash, `hash_id`, the tagged-id
userdata mechanism and the string marshalling are engine ([0042](0042-space-toolbox-layering.md)).

## Consequences

- Any system, body, chunk or object can be named, seeded and generated alone, on any peer, in any order; a sky query
  visits a bounded number of cells per layer ([0051](0051-staged-generation-pipeline.md)).
- The galaxy can't exceed the address span; a bigger galaxy needs a new address layout, which means a new universe.
- Mods add bodies and objects next to existing ones instead of reshuffling them.
- The layout, the hash and the seed formula are fixed for every universe's life; test vectors come with the code.

**Validated by** VCH-081 (100,000 systems headless: no address overflow, stage hashes equal across s and thread
counts) and VCH-094 (addresses as save keys: an anchored body is unchanged after a stage update; save → load → save is
byte-identical). **Reopen if** VCH-081 finds a layer-0 cell in the bulge needing more than 8,192 systems at the
default density, or more than 255 generated bodies in a system: widen the field before any universe exists, never
after.

## Alternatives considered

- **Random UUIDs stored in a database:** needs storage for everything that could be generated.
- **NMS-style fixed fields:** simple, but a 16-body cap and no headroom for mods.
- **128-bit system addresses:** unneeded headroom paid by every key and wire id.
- **Seeds walked down a sequential RNG:** order-dependent and O(n) random access.
