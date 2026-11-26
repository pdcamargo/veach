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
};

static const uint cache_probes = 8;
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

CacheKey cache_key(RadianceCache c, float3 p, float3 n, uint level) {
    const float size = c.cell_size * float(1u << level);
    const int3 q = int3(floor(p / size));
    const uint tag = level * 8u + normal_bucket(n);
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
/// or a coarser one. False if no cell there has a value yet.
bool cache_lookup(RadianceCache c, float3 p, float3 n, out float3 radiance) {
    radiance = float3(0.0, 0.0, 0.0);
    const uint level = cache_level(c, p);
    for (uint l = level; l <= min(level + cache_coarser_levels, cache_max_level); ++l) {
        const CacheKey k = cache_key(c, p, n, l);
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

/// Adds one estimate of the outgoing diffuse radiance at p (normal n facing the side seen) to its cell, claiming a
/// slot for it if it has none. Dropped when every probed slot belongs to another cell.
void cache_add(RadianceCache c, float3 p, float3 n, float3 radiance) {
    const CacheKey k = cache_key(c, p, n, cache_level(c, p));
    const uint3 units = uint3(min(max(radiance, 0.0) * c.scale, 4.0e6));
    for (uint i = 0; i < cache_probes; ++i) {
        const uint slot = (k.slot + i) & (c.capacity - 1u);
        uint previous;
        InterlockedCompareExchange(c.keys[slot], 0u, k.checksum, previous);
        if (previous == 0u || previous == k.checksum) {
            InterlockedAdd(c.accum[slot * 4u + 0u], units.x);
            InterlockedAdd(c.accum[slot * 4u + 1u], units.y);
            InterlockedAdd(c.accum[slot * 4u + 2u], units.z);
            InterlockedAdd(c.accum[slot * 4u + 3u], 1u);
            return;
        }
    }
}

/// End of frame for one slot: this frame's mean blended into the resolved value (an average over up to max_frames
/// frames), the sums cleared; a cell nobody added to for `evict_after` frames is freed.
void resolve_cell(RadianceCache c, uint slot, uint evict_after) {
    if (c.keys[slot] == 0u) {
        return;
    }
    const uint count = c.accum[slot * 4u + 3u];
    const uint2 v = c.radiance[slot];
    uint frames = v.y >> 24;
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
        return;
    }
    value = min(value, 60000.0);  // half's range
    c.radiance[slot] = uint2(f32tof16(value.x) | (f32tof16(value.y) << 16),
                             f32tof16(value.z) | (min(age, 255u) << 16) | (frames << 24));
}
