// The world-space radiance cache (ADR 0051): a spatial hash of cells, each holding the outgoing diffuse radiance of
// the surfaces in it (nits, without their emission). Candela feeds it from sparse paths and reads it where its gather
// rays land; reflections (VCH-018) can do both through the same functions.
//
// A cell is a cube whose size grows with its distance from the camera (finer close up, against leaks), times one of
// six normal directions, so the two sides of a thin wall never share one. Cells live in an open-addressed table:
// `keys` holds each slot's checksum (0: empty), `accum` this frame's sums (fixed point, 4 uints per slot: r, g, b,
// count), `radiance` the resolved cell (CellValue, packed in a uint4). Mirrors render::RadianceCache's buffers
// (realtime_renderer.cpp).
//
// A frame's samples for a cell are summed first, and their mean blends into the cell's history weighted by how many
// there were, over at most `max_samples` samples. A short average of the frame means watches for change: when it
// leaves the history by more than the frame means' noise allows (a light switched off), the history restarts from
// this frame.

import veach.bindless;
import veach.brdf;  // luminance
import veach.random;

struct RadianceCache {
    DescriptorHandle<RWStructuredBuffer<uint>> keys;
    DescriptorHandle<RWStructuredBuffer<uint>> accum;
    DescriptorHandle<RWStructuredBuffer<uint4>> radiance;
    float3 camera;
    uint capacity;     ///< slots, a power of two
    float scale;       ///< fixed-point units per nit in `accum`
    float cell_size;   ///< meters at level 0
    float cell_scale;  ///< a cell is about this fraction of its distance to the camera
    uint max_samples;  ///< the history a cell averages over, in samples
    uint smooth;       ///< 1: lookups blend the neighboring cells (trilinear across cell centers, same normal)
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
static const float min_trusted_samples = 16.0;  ///< a history this long can tell a change from noise
static const float history_decay = 0.9;  ///< a cell's sample count fades by this much a frame: its history is recent

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

float cell_size_at(RadianceCache c, uint level) {
    return c.cell_size * float(1u << level);
}

/// The key of cell q (integer coordinates at its level) for one level, normal direction and kind.
CacheKey cache_key_cell(RadianceCache c, int3 q, uint level, uint bucket, uint kind) {
    const uint tag = (kind * 16u + level) * 8u + bucket;
    const uint h = pcg(uint(q.z) ^ pcg(uint(q.y) ^ pcg(uint(q.x) ^ pcg(tag))));
    CacheKey k;
    k.slot = h & (c.capacity - 1u);
    k.checksum = pcg(h ^ 0x9e3779b9u ^ pcg(uint(q.x) + tag)) | 1u;
    return k;
}

CacheKey cache_key(RadianceCache c, float3 p, float3 n, uint level, uint kind = cache_kind_outgoing) {
    return cache_key_cell(c, int3(floor(p / cell_size_at(c, level))), level, normal_bucket(n), kind);
}

/// A resolved cell.
struct CellValue {
    float3 value;      ///< the history's average
    float short_mean;  ///< a short average of the frame means' luminance: what changed recently
    float variance;    ///< of the frame means' luminance
    float samples;     ///< in the history, up to max_samples; 0: no value yet
    uint age;          ///< frames since a sample last arrived
};

CellValue unpack_cell(uint4 v) {
    CellValue cell;
    cell.value = float3(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y));
    cell.short_mean = f16tof32(v.y >> 16);
    cell.variance = f16tof32(v.z);
    cell.samples = f16tof32(v.z >> 16);
    cell.age = v.w & 0xffu;
    return cell;
}

uint4 pack_cell(CellValue cell) {
    const float3 value = min(cell.value, 60000.0);  // half's range
    return uint4(f32tof16(value.x) | (f32tof16(value.y) << 16), f32tof16(value.z) | (f32tof16(min(cell.short_mean, 60000.0)) << 16),
                 f32tof16(min(cell.variance, 60000.0)) | (f32tof16(cell.samples) << 16), min(cell.age, 255u));
}

static const float confident_samples = 8.0;  ///< a cell this well sampled answers alone; fewer, its parent shares

/// The value of the cell with key k, and how far to trust it (0: no value, 1: confident_samples or more).
float cell_value(RadianceCache c, CacheKey k, out float3 value) {
    value = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < cache_probes; ++i) {
        const uint slot = (k.slot + i) & (c.capacity - 1u);
        if (c.keys[slot] == k.checksum) {
            const CellValue cell = unpack_cell(c.radiance[slot]);
            value = cell.value;
            return saturate(cell.samples / confident_samples);
        }
    }
    return 0.0;
}

/// p's value at one level and how far to trust it: its own cell's, or with `smooth` a trilinear blend of the eight
/// cells around it (their centers, the same normal direction and kind), each weighted by its trust. Deterministic:
/// the same point reads the same blend every frame.
float level_value(RadianceCache c, float3 p, uint bucket, uint level, uint kind, out float3 value) {
    const float size = cell_size_at(c, level);
    if (c.smooth == 0u) {
        return cell_value(c, cache_key_cell(c, int3(floor(p / size)), level, bucket, kind), value);
    }
    const float3 u = p / size - 0.5;
    const int3 base = int3(floor(u));
    const float3 t = u - float3(base);
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    for (uint corner = 0; corner < 8; ++corner) {
        const int3 o = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        const float3 w3 = select(o == int3(1, 1, 1), t, 1.0 - t);
        const float w = w3.x * w3.y * w3.z;
        float3 v = float3(0.0, 0.0, 0.0);
        const float trust = w > 0.0 ? cell_value(c, cache_key_cell(c, base + o, level, bucket, kind), v) : 0.0;
        sum += v * w * trust;
        weight += w * trust;
    }
    value = weight > 0.0 ? sum / weight : float3(0.0, 0.0, 0.0);
    return weight;
}

/// The cached outgoing diffuse radiance at surface point p with normal n (facing the side seen): p's own level as far
/// as it is trusted, the rest from its parent and grandparent (every estimate goes into all three), so a cell that is
/// new or thinly sampled fades in instead of switching. False if no level has a value yet; `level_offset` is the
/// finest level that contributed (0: p's own).
bool cache_lookup(RadianceCache c, float3 p, float3 n, out float3 radiance, out uint level_offset,
                  uint kind = cache_kind_outgoing) {
    radiance = float3(0.0, 0.0, 0.0);
    level_offset = 0;
    const uint level = cache_level(c, p);
    const uint bucket = normal_bucket(n);
    float3 sum = float3(0.0, 0.0, 0.0);
    float remaining = 1.0;
    bool found = false;
    for (uint l = level; l <= min(level + cache_coarser_levels, cache_max_level) && remaining > 0.01; ++l) {
        float3 v = float3(0.0, 0.0, 0.0);
        const float trust = level_value(c, p, bucket, l, kind, v);
        if (trust > 0.0 && !found) {
            level_offset = l - level;
            found = true;
        }
        sum += v * trust * remaining;
        remaining *= 1.0 - trust;
    }
    radiance = found ? sum / (1.0 - remaining) : float3(0.0, 0.0, 0.0);
    return found;
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

/// End of frame for one slot: this frame's samples (summed by cache_add) blend into the cell's history, weighted by
/// their count over a history of at most max_samples, and the sums clear. A frame whose mean leaves the history by
/// more than the noise of frame means allows restarts the history from that frame. A cell nobody added to for
/// `evict_after` frames is freed.
void resolve_cell(RadianceCache c, uint slot, uint evict_after) {
    const bool occupied = c.keys[slot] != 0u;
    count(c, cache_stat_occupied, occupied);
    if (!occupied) {
        return;
    }
    const uint count = c.accum[slot * 4u + 3u];
    CellValue cell = unpack_cell(c.radiance[slot]);
    // The history reaches back a few frames however often the cell is sampled: a thinly sampled cell keeps few
    // samples, is trusted little and lets its parent answer (cache_lookup), which is both steadier and quicker.
    cell.samples *= history_decay;
    if (count > 0u) {
        const float3 mean = float3(c.accum[slot * 4u + 0u], c.accum[slot * 4u + 1u], c.accum[slot * 4u + 2u]) /
                            (float(count) * c.scale);
        const float l = luminance(mean);
        if (cell.samples < 1e-3) {
            cell.value = mean;
            cell.short_mean = l;
            cell.samples = min(float(count), float(c.max_samples));
        } else {
            const float history = luminance(cell.value);
            const float d = l - cell.short_mean;  // noise around the recent level: a change itself does not count
            cell.short_mean = lerp(cell.short_mean, l, 0.5);
            // A change: once the history is long enough to trust, the short average (about two frames) is off it by
            // more than 2.5x the frame means' spread around the short average and by more than 10%. The spread is
            // the cell's noise, kept across restarts: a light switching off changes the cell's level, not how noisy
            // its samples are.
            const float spread = sqrt(max(cell.variance, 0.0));
            const float off = abs(cell.short_mean - history);
            if (cell.samples >= min_trusted_samples && off > 2.5 * spread && off > 0.1 * history) {
                cell.samples = 0.0;
                cell.short_mean = l;
            }
            const float samples = min(cell.samples + float(count), float(c.max_samples));
            const float w = float(count) / samples;
            cell.variance = lerp(cell.variance, d * d, max(w, 0.05));
            cell.value = lerp(cell.value, mean, w);
            cell.samples = samples;
        }
        cell.age = 0u;
        c.accum[slot * 4u + 0u] = 0u;
        c.accum[slot * 4u + 1u] = 0u;
        c.accum[slot * 4u + 2u] = 0u;
        c.accum[slot * 4u + 3u] = 0u;
    } else if (++cell.age > evict_after) {
        c.keys[slot] = 0u;
        c.radiance[slot] = uint4(0u, 0u, 0u, 0u);
        InterlockedAdd(c.stats[cache_stat_evictions], 1u);
        return;
    }
    c.radiance[slot] = pack_cell(cell);
}
