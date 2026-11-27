// The world-space radiance cache (ADR 0051): a spatial hash of cells, each holding the outgoing diffuse radiance of
// the surfaces in it (nits, without their emission). Candela feeds it from sparse paths and reads it where its gather
// rays land; reflections (VCH-018) can do both through the same functions.
//
// A cell is a cube whose size grows with its distance from the camera (finer close up, against leaks), times one of
// six normal directions, so the two sides of a thin wall never share one. Cells live in an open-addressed table:
// `keys` holds each slot's checksum (0: empty), `accum` this frame's sums (fixed point, 4 uints per slot: r, g, b,
// count), `radiance` the resolved value (half r, g, b and frames: rgb in x and y, frames and age in the top of y).
// Mirrors render::RadianceCache's buffers (realtime_renderer.cpp).

import veach.bindless;
import veach.random;

struct RadianceCache {
    DescriptorHandle<RWStructuredBuffer<uint>> keys;
    DescriptorHandle<RWStructuredBuffer<uint>> accum;
    DescriptorHandle<RWStructuredBuffer<uint2>> radiance;
    float3 camera;
    uint capacity;     ///< slots, a power of two
    float scale;       ///< fixed-point units per nit in `accum`
    float cell_size;   ///< meters at level 0
    float cell_scale;  ///< a cell is about this fraction of its distance to the camera
    uint max_frames;   ///< the resolved value blends at least 1 / max_frames of each new frame's mean
    DescriptorHandle<RWStructuredBuffer<uint>> stats;  ///< cache_stat_* counters, never reset (logged at shutdown)
};

// Counters in RadianceCache::stats (render::RealtimeRenderer logs them at shutdown).
static const uint cache_stat_adds = 3;          ///< estimates added (one per vertex and level)
static const uint cache_stat_new_cells = 4;     ///< empty slots claimed
static const uint cache_stat_failed_adds = 5;   ///< estimates dropped: every probed slot held another cell
static const uint cache_stat_evictions = 6;     ///< cells freed after evict_after idle frames
static const uint cache_stat_occupied = 7;      ///< slots holding a cell after each resolve, summed over frames
static const uint cache_stat_hit_own = 8;       ///< lookups answered by their own level
static const uint cache_stat_hit_parent = 9;    ///< answered by a coarser level
static const uint cache_stat_miss = 10;         ///< no level had a value: the hit was shaded and added

void count(RadianceCache c, uint index, bool event) {
    const uint n = WaveActiveCountBits(event);
    if (WaveIsFirstLane() && n > 0) {
        InterlockedAdd(c.stats[index], n);
    }
}

static const uint cache_probes = 8;
// What a cell holds: the outgoing diffuse radiance of the surfaces in it, or (at visible surfaces only) the diffuse
// light arriving there from bounces and the sky: the cosine-weighted mean of the incoming radiance, without
// emitters (direct light). Both kinds share the table.
static const uint cache_kind_outgoing = 0;
static const uint cache_kind_incoming = 1;
static const uint cache_coarser_levels = 2;  ///< a lookup that misses tries this many coarser levels
static const uint cache_max_level = 15;

/// One of six directions: the normal's dominant axis and its sign.
uint normal_bucket(float3 n) {
    const float3 a = abs(n);
    if (a.x >= a.y && a.x >= a.z) {
        return n.x >= 0.0 ? 0u : 1u;
    }
    if (a.y >= a.z) {
        return n.y >= 0.0 ? 2u : 3u;
    }
    return n.z >= 0.0 ? 4u : 5u;
}

uint cache_level(RadianceCache c, float3 p) {
    const float size = length(p - c.camera) * c.cell_scale;
    return min(uint(max(floor(log2(max(size / c.cell_size, 1.0))), 0.0)), cache_max_level);
}

struct CacheKey {
    uint slot;
    uint checksum;  ///< never 0
};

CacheKey cache_key(RadianceCache c, float3 p, float3 n, uint level, uint kind = cache_kind_outgoing) {
    const float size = c.cell_size * float(1u << level);
    const int3 q = int3(floor(p / size));
    const uint tag = (kind * 16u + level) * 8u + normal_bucket(n);
    const uint h = pcg(uint(q.z) ^ pcg(uint(q.y) ^ pcg(uint(q.x) ^ pcg(tag))));
    CacheKey k;
    k.slot = h & (c.capacity - 1u);
    k.checksum = pcg(h ^ 0x9e3779b9u ^ pcg(uint(q.x) + tag)) | 1u;
    return k;
}

float3 unpack_cache_radiance(uint2 v) {
    return float3(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y));
}

/// The cached outgoing diffuse radiance at surface point p with normal n (facing the side seen), from p's own level
/// or, where that cell is empty, its parent's and grandparent's (every estimate goes into all three). False if no
/// level has a value yet; `level_offset` says which answered (0: p's own).
bool cache_lookup(RadianceCache c, float3 p, float3 n, out float3 radiance, out uint level_offset,
                  uint kind = cache_kind_outgoing) {
    radiance = float3(0.0, 0.0, 0.0);
    level_offset = 0;
    const uint level = cache_level(c, p);
    for (uint l = level; l <= min(level + cache_coarser_levels, cache_max_level); ++l) {
        level_offset = l - level;
        const CacheKey k = cache_key(c, p, n, l, kind);
        for (uint i = 0; i < cache_probes; ++i) {
            const uint slot = (k.slot + i) & (c.capacity - 1u);
            if (c.keys[slot] == k.checksum) {
                const uint2 v = c.radiance[slot];
                if ((v.y >> 24) > 0) {  // frames with a value
                    radiance = unpack_cache_radiance(v);
                    return true;
                }
                break;
            }
        }
    }
    return false;
}

bool cache_lookup(RadianceCache c, float3 p, float3 n, out float3 radiance) {
    uint level_offset;
    return cache_lookup(c, p, n, radiance, level_offset);
}

/// Adds one estimate to a cell, claiming a slot for it if it has none. Dropped when every probed slot belongs to
/// another cell.
void cache_add_at(RadianceCache c, CacheKey k, uint3 units) {
    bool added = false;
    bool claimed = false;
    for (uint i = 0; i < cache_probes && !added; ++i) {
        const uint slot = (k.slot + i) & (c.capacity - 1u);
        uint previous;
        InterlockedCompareExchange(c.keys[slot], 0u, k.checksum, previous);
        if (previous == 0u || previous == k.checksum) {
            InterlockedAdd(c.accum[slot * 4u + 0u], units.x);
            InterlockedAdd(c.accum[slot * 4u + 1u], units.y);
            InterlockedAdd(c.accum[slot * 4u + 2u], units.z);
            InterlockedAdd(c.accum[slot * 4u + 3u], 1u);
            added = true;
            claimed = previous == 0u;
        }
    }
    count(c, cache_stat_adds, true);
    count(c, cache_stat_new_cells, claimed);
    count(c, cache_stat_failed_adds, !added);
}

/// Adds one estimate of the outgoing diffuse radiance at p (normal n facing the side seen) to its cell and to the
/// two coarser cells containing it, so a lookup that finds its own cell empty can read a parent.
void cache_add(RadianceCache c, float3 p, float3 n, float3 radiance, uint kind = cache_kind_outgoing) {
    const uint3 units = uint3(min(max(radiance, 0.0) * c.scale, 4.0e6));
    const uint level = cache_level(c, p);
    for (uint l = level; l <= min(level + cache_coarser_levels, cache_max_level); ++l) {
        cache_add_at(c, cache_key(c, p, n, l, kind), units);
    }
}

/// End of frame for one slot: this frame's mean blended into the resolved value (an average over up to max_frames
/// frames), the sums cleared; a cell nobody added to for `evict_after` frames is freed. With `restart` (the lighting
/// changed this frame) the old value is dropped: the cell holds this frame's mean, or no value until a sample arrives.
void resolve_cell(RadianceCache c, uint slot, uint evict_after, bool restart) {
    const bool occupied = c.keys[slot] != 0u;
    count(c, cache_stat_occupied, occupied);
    if (!occupied) {
        return;
    }
    const uint count = c.accum[slot * 4u + 3u];
    const uint2 v = c.radiance[slot];
    uint frames = restart ? 0u : v.y >> 24;
    uint age = (v.y >> 16) & 0xffu;
    float3 value = unpack_cache_radiance(v);
    if (count > 0u) {
        const float3 mean = float3(c.accum[slot * 4u + 0u], c.accum[slot * 4u + 1u], c.accum[slot * 4u + 2u]) /
                            (float(count) * c.scale);
        frames = min(frames + 1u, c.max_frames);
        value = lerp(value, mean, 1.0 / float(frames));
        age = 0u;
        c.accum[slot * 4u + 0u] = 0u;
        c.accum[slot * 4u + 1u] = 0u;
        c.accum[slot * 4u + 2u] = 0u;
        c.accum[slot * 4u + 3u] = 0u;
    } else if (++age > evict_after) {
        c.keys[slot] = 0u;
        c.radiance[slot] = uint2(0u, 0u);
        InterlockedAdd(c.stats[cache_stat_evictions], 1u);
        return;
    }
    value = min(value, 60000.0);  // half's range
    c.radiance[slot] = uint2(f32tof16(value.x) | (f32tof16(value.y) << 16),
                             f32tof16(value.z) | (min(age, 255u) << 16) | (frames << 24));
}
