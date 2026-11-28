// The real-time renderer's culling (docs/adr/0093-gpu-driven-core.md, docs/specs/gpu-driven-renderer/spec.md): every
// instance, then every meshlet of the survivors, against the frustum, its normal cone and the HiZ pyramid, in two
// phases. Phase 1 tests last frame's pyramid and draws what passes; the pyramid is rebuilt from that depth; phase 2
// re-tests what phase 1 found occluded against the new pyramid and draws what became visible, so a moving camera never
// shows a hole. Survivors go to compacted draw lists whose DrawIndirectArgs::instance_count the GPU counts (ADR 0089).
// Its parameter layout matches render::CullParams (realtime_renderer.cpp).

import veach.bindless;
import veach.material;  // MaterialRecord flags
import veach.scene;     // InstanceRecord, to_world

/// Mirrors render::CullView: the view culling tests against this frame. Every position is in render space.
struct CullView {
    float4x4 view_projection;
    float4x4 occlusion_view_projection;  ///< phase 1: the view last frame's pyramid was built from, in this frame's space
    float4 planes[5];                    ///< left, right, bottom, top, near: inside where dot(xyz, p) + w >= 0
    float4 camera;                       ///< position; w = near plane distance
    uint2 screen;                        ///< the depth buffer's size
    uint2 hiz_size;                      ///< mip 0 of the pyramid: half the depth buffer, rounded down
    uint hiz_levels;
    uint flags;  ///< cull_previous_hiz, cull_shadow_view
    uint instance_count;
    uint phase_stride;      ///< entries from a phase-1 draw list to the same phase-2 list
    uint4 list_offsets[2];  ///< first entry of each of the 8 draw lists (phase 1)
    uint task_capacity;     ///< meshlet tasks per phase
    uint pad0;
    uint pad1;
    uint pad2;
    /// cull_shadow_view: the camera's frustum slice the cascade's shadows fall in (inside where dot(xyz, p) + w >= 0)...
    float4 receiver_planes[6];
    float4 sweep;  ///< ...the way shadows go (xyz, away from the light), w: the smallest sphere radius that casts
};

struct CullParams {
    DescriptorHandle<StructuredBuffer<CullView>> view;
    DescriptorHandle<StructuredBuffer<InstanceRecord>> instances;
    DescriptorHandle<StructuredBuffer<MaterialRecord>> materials;
    DescriptorHandle<Texture2D<float>> hiz;  ///< every mip: the farthest depth (reverse-Z) of each texel's pixels
    DescriptorHandle<RWStructuredBuffer<uint>> counters;
    DescriptorHandle<RWStructuredBuffer<uint>> draw_args;      ///< DrawIndirectArgs: phase x list
    DescriptorHandle<RWStructuredBuffer<uint>> dispatch_args;  ///< 16-byte DispatchIndirectArgs records
    DescriptorHandle<RWStructuredBuffer<uint2>> tasks;         ///< instance, first meshlet; task_capacity per phase
    DescriptorHandle<RWStructuredBuffer<uint>> recheck_instances;
    DescriptorHandle<RWStructuredBuffer<uint2>> recheck_meshlets;  ///< instance, meshlet
    DescriptorHandle<RWStructuredBuffer<uint2>> visible;           ///< the draw lists: instance, meshlet
    DescriptorHandle<RWStructuredBuffer<uint>> bin_tiles;          ///< the shading bins' counts (reset only)
    uint phase;                                                    ///< 0 or 1
    uint bin_count;
    uint pad0;
    uint pad1;
};
[[vk::push_constant]] ConstantBuffer<CullParams> params;

static const uint cull_previous_hiz = 1u;  ///< CullView::flags: last frame's pyramid is valid for phase 1
/// CullView::flags: a shadow cascade (shadows.cpp). No normal cones (every meshlet casts, whichever way it faces);
/// what passes must also be big enough to cast (sweep.w) and its shadow must fall in the receivers' slice.
static const uint cull_shadow_view = 2u;
static const uint meshlet_task_size = 64;  ///< meshlets per task (a thread group of cull_meshlets)
static const uint bucket_count = 4;        ///< draw lists by triangle count: <= 16, <= 32, <= 64, <= 124
static const uint list_count = 8;          ///< opaque buckets, then alpha-tested buckets (render::draw_list_of)
static const uint groups_per_row = 32768;  ///< indirect dispatches wider than this use rows (D3D12 caps a row at 65535)

// counters[]
static const uint counter_tasks = 0;  ///< + phase
static const uint counter_recheck_instances = 2;
static const uint counter_recheck_meshlets = 3;
static const uint counter_instances_visible = 4;  ///< + phase: instances that passed
static const uint counter_meshlets_culled = 6;    ///< by the frustum or the normal cone
static const uint counter_meshlets_occluded = 7;  ///< found occluded in phase 2 (they stay hidden)
static const uint counter_count = 64;

// dispatch_args records (16 bytes each)
static const uint dispatch_tasks_1 = 0;
static const uint dispatch_recheck_instances = 1;
static const uint dispatch_recheck_meshlets = 2;
static const uint dispatch_tasks_2 = 3;

uint bucket_of(uint triangles) {
    return triangles <= 16 ? 0 : (triangles <= 32 ? 1 : (triangles <= 64 ? 2 : 3));
}

struct Sphere {
    float3 center;
    float radius;
};

/// An object-space sphere in render space: the transform's largest axis scales the radius.
Sphere world_sphere(InstanceRecord inst, float4 s) {
    Sphere out;
    out.center = to_world(inst.object_to_world, s.xyz, 1.0);
    const float3 x = float3(inst.object_to_world[0].x, inst.object_to_world[1].x, inst.object_to_world[2].x);
    const float3 y = float3(inst.object_to_world[0].y, inst.object_to_world[1].y, inst.object_to_world[2].y);
    const float3 z = float3(inst.object_to_world[0].z, inst.object_to_world[1].z, inst.object_to_world[2].z);
    out.radius = s.w * sqrt(max(max(dot(x, x), dot(y, y)), dot(z, z)));
    return out;
}

/// For a shadow cascade, whether the sphere can shadow anything in the receivers' slice: the sphere swept away from
/// the light stays outside none of the slice's planes. A plane the sweep heads into is never a reason to cull
/// (conservative: a sweep past a corner of the slice is kept).
bool casts_into_view(CullView v, Sphere s) {
    if (s.radius < v.sweep.w) {
        return false;  // smaller than the cascade can show
    }
    for (uint i = 0; i < 6; ++i) {
        const float4 p = v.receiver_planes[i];
        if (dot(p.xyz, s.center) + p.w < -s.radius && dot(p.xyz, v.sweep.xyz) <= 0.0) {
            return false;
        }
    }
    return true;
}

bool in_frustum(CullView v, Sphere s) {
    for (uint i = 0; i < 5; ++i) {
        if (dot(v.planes[i].xyz, s.center) + v.planes[i].w < -s.radius) {
            return false;
        }
    }
    return (v.flags & cull_shadow_view) == 0 || casts_into_view(v, s);
}

/// True if the sphere is hidden behind the pyramid built from `view_projection`'s depth: its nearest point is farther
/// than the farthest depth of every pyramid texel its screen rectangle touches. Pyramid texel t of mip L covers
/// pixels [t, t + 1) * 2^(L + 1), and the last texel of a row or column reaches the edge (hiz.slang).
bool occluded(CullView v, float4x4 view_projection, Sphere s) {
    float2 lo = float2(1e30, 1e30);
    float2 hi = float2(-1e30, -1e30);
    float nearest = 0.0;
    [unroll]
    for (uint i = 0; i < 8; ++i) {
        const float3 corner = s.center + s.radius * float3((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0,
                                                           (i & 4) != 0 ? 1.0 : -1.0);
        const float4 clip = mul(view_projection, float4(corner, 1.0));
        if (clip.w < v.camera.w) {
            return false;  // reaches in front of the near plane: visible
        }
        const float3 ndc = clip.xyz / clip.w;
        const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        lo = min(lo, uv);
        hi = max(hi, uv);
        nearest = max(nearest, ndc.z);  // reverse-Z: nearer is larger
    }
    if (any(hi < float2(0.0, 0.0)) || any(lo > float2(1.0, 1.0))) {
        return false;  // outside that view: its depth says nothing
    }
    const float2 screen = float2(v.screen);
    const float2 p_lo = saturate(lo) * screen;
    const float2 p_hi = saturate(hi) * screen;
    const float extent = max(p_hi.x - p_lo.x, p_hi.y - p_lo.y);
    const uint level = min(extent <= 2.0 ? 0u : uint(ceil(log2(extent))) - 1u, v.hiz_levels - 1u);
    const uint2 size = max(v.hiz_size >> level, uint2(1, 1));
    const uint shift = level + 1;
    const uint2 a = min(uint2(p_lo) >> shift, size - 1);
    const uint2 b = min(uint2(p_hi) >> shift, size - 1);
    float farthest = 1.0;
    for (uint y = a.y; y <= b.y; ++y) {
        for (uint x = a.x; x <= b.x; ++x) {
            farthest = min(farthest, params.hiz.Load(int3(int(x), int(y), int(level))));
        }
    }
    return nearest < farthest;
}

/// Whether every triangle of the meshlet faces away from the camera (meshoptimizer's cone test, on the sphere).
bool backfacing(InstanceRecord inst, Sphere s, float4 cone, float3 camera) {
    if ((inst.flags & instance_cone_culling) == 0 || cone.w >= 1.0) {
        return false;
    }
    const float3 axis = normalize(to_world(inst.object_to_world, cone.xyz, 0.0));
    const float3 to_center = s.center - camera;
    return dot(to_center, axis) >= cone.w * length(to_center) + s.radius;
}

uint list_offset(CullView v, uint list) {
    return v.list_offsets[list / 4][list % 4];
}

void draw_meshlet(CullView v, uint phase, uint instance, uint meshlet, uint triangles, bool masked) {
    const uint list = (masked ? bucket_count : 0) + bucket_of(triangles);
    uint slot;
    InterlockedAdd(params.draw_args[(phase * list_count + list) * 4 + 1], 1u, slot);
    params.visible[list_offset(v, list) + phase * v.phase_stride + slot] = uint2(instance, meshlet);
}

/// Frustum, normal cone (single-sided materials only), then occlusion: drawn, rechecked in phase 2, or dropped.
void test_meshlet(CullView v, uint phase, InstanceRecord inst, uint instance, uint meshlet) {
    const uint triangles = nonuniform(inst.meshlets)[meshlet * 8 + 3];
    const uint slot = nonuniform(inst.meshlets)[meshlet * 8 + 4];
    const Sphere s = world_sphere(inst, nonuniform(inst.meshlet_bounds)[meshlet * 3]);
    const MaterialRecord material = params.materials[inst.material_base + slot];
    const bool masked = (material.flags & (material_alpha_mask | material_alpha_blend)) != 0;
    if (!in_frustum(v, s) ||
        ((v.flags & cull_shadow_view) == 0 && (material.flags & material_double_sided) == 0 &&
         backfacing(inst, s, nonuniform(inst.meshlet_bounds)[meshlet * 3 + 1], v.camera.xyz))) {
        InterlockedAdd(params.counters[counter_meshlets_culled], 1u);
        return;
    }
    if (phase == 0) {
        if ((v.flags & cull_previous_hiz) != 0 && occluded(v, v.occlusion_view_projection, s)) {
            uint index;
            InterlockedAdd(params.counters[counter_recheck_meshlets], 1u, index);
            params.recheck_meshlets[index] = uint2(instance, meshlet);
            return;
        }
    } else if (occluded(v, v.view_projection, s)) {
        InterlockedAdd(params.counters[counter_meshlets_occluded], 1u);
        return;
    }
    draw_meshlet(v, phase, instance, meshlet, triangles, masked);
}

/// The linear index of a group of an indirect dispatch made by dispatch_args_for.
uint group_index(uint3 group) {
    return group.y * groups_per_row + group.x;
}

void dispatch_args_for(uint record, uint groups) {
    params.dispatch_args[record * 4 + 0] = min(groups, groups_per_row);
    params.dispatch_args[record * 4 + 1] = max((groups + groups_per_row - 1) / groups_per_row, 1u);
    params.dispatch_args[record * 4 + 2] = 1;
    params.dispatch_args[record * 4 + 3] = 0;
}

/// Start of the frame: counters to zero, draw records to empty, the shading bins' tile counts (classify.slang) to zero.
[shader("compute")]
[numthreads(64, 1, 1)]
void cull_reset(uint3 id: SV_DispatchThreadID) {
    params.counters[id.x] = 0;
    if (id.x < 2 * list_count) {
        const uint triangles[bucket_count] = { 16, 32, 64, 124 };
        params.draw_args[id.x * 4 + 0] = triangles[(id.x % list_count) % bucket_count] * 3;
        params.draw_args[id.x * 4 + 1] = 0;
        params.draw_args[id.x * 4 + 2] = 0;
        params.draw_args[id.x * 4 + 3] = 0;
    }
    if (id.x < params.bin_count) {
        params.bin_tiles[id.x] = 0;
    }
}

/// Phase 1: every instance (a thread each). Phase 2: those phase 1 found occluded. Survivors with one meshlet are
/// tested right here; larger ones become tasks of 64 meshlets for cull_meshlets.
[shader("compute")]
[numthreads(64, 1, 1)]
void cull_instances(uint3 group: SV_GroupID, uint3 thread: SV_GroupThreadID) {
    const CullView v = params.view[0];
    const uint phase = params.phase;
    const uint i = group_index(group) * 64 + thread.x;
    uint index;
    if (phase == 0) {
        if (i >= v.instance_count) {
            return;
        }
        index = i;
    } else {
        if (i >= params.counters[counter_recheck_instances]) {
            return;
        }
        index = params.recheck_instances[i];
    }
    const InstanceRecord inst = params.instances[index];
    if (inst.meshlet_count == 0) {
        return;
    }
    const Sphere s = world_sphere(inst, inst.sphere);
    if (phase == 0) {
        if (!in_frustum(v, s)) {
            return;
        }
        if ((v.flags & cull_previous_hiz) != 0 && occluded(v, v.occlusion_view_projection, s)) {
            uint slot;
            InterlockedAdd(params.counters[counter_recheck_instances], 1u, slot);
            params.recheck_instances[slot] = index;
            return;
        }
    } else if (occluded(v, v.view_projection, s)) {
        return;
    }
    InterlockedAdd(params.counters[counter_instances_visible + phase], 1u);
    if (inst.meshlet_count == 1) {
        test_meshlet(v, phase, inst, index, 0);
        return;
    }
    const uint count = (inst.meshlet_count + meshlet_task_size - 1) / meshlet_task_size;
    uint first;
    InterlockedAdd(params.counters[counter_tasks + phase], count, first);
    for (uint t = 0; t < count; ++t) {
        params.tasks[phase * v.task_capacity + first + t] = uint2(index, t * meshlet_task_size);
    }
}

/// A task of up to 64 meshlets of one instance, a thread each.
[shader("compute")]
[numthreads(64, 1, 1)]
void cull_meshlets(uint3 group: SV_GroupID, uint3 thread: SV_GroupThreadID) {
    const CullView v = params.view[0];
    const uint phase = params.phase;
    const uint t = group_index(group);
    if (t >= params.counters[counter_tasks + phase]) {
        return;
    }
    const uint2 task = params.tasks[phase * v.task_capacity + t];
    const InstanceRecord inst = params.instances[task.x];
    const uint meshlet = task.y + thread.x;
    if (meshlet >= inst.meshlet_count) {
        return;
    }
    test_meshlet(v, phase, inst, task.x, meshlet);
}

/// Phase 2: the meshlets phase 1 found occluded (they passed the frustum and the cone), against the new pyramid.
[shader("compute")]
[numthreads(64, 1, 1)]
void cull_recheck_meshlets(uint3 group: SV_GroupID, uint3 thread: SV_GroupThreadID) {
    const CullView v = params.view[0];
    const uint i = group_index(group) * 64 + thread.x;
    if (i >= params.counters[counter_recheck_meshlets]) {
        return;
    }
    const uint2 entry = params.recheck_meshlets[i];
    const InstanceRecord inst = params.instances[entry.x];
    const Sphere s = world_sphere(inst, nonuniform(inst.meshlet_bounds)[entry.y * 3]);
    if (occluded(v, v.view_projection, s)) {
        InterlockedAdd(params.counters[counter_meshlets_occluded], 1u);
        return;
    }
    const uint slot = nonuniform(inst.meshlets)[entry.y * 8 + 4];
    const MaterialRecord material = params.materials[inst.material_base + slot];
    draw_meshlet(v, 1, entry.x, entry.y, nonuniform(inst.meshlets)[entry.y * 8 + 3],
                 (material.flags & (material_alpha_mask | material_alpha_blend)) != 0);
}

/// The indirect dispatches of the next culling passes, from the counts so far (one thread).
[shader("compute")]
[numthreads(1, 1, 1)]
void cull_args(uint3 id: SV_DispatchThreadID) {
    dispatch_args_for(dispatch_tasks_1, params.counters[counter_tasks]);
    dispatch_args_for(dispatch_recheck_instances, (params.counters[counter_recheck_instances] + 63) / 64);
    dispatch_args_for(dispatch_recheck_meshlets, (params.counters[counter_recheck_meshlets] + 63) / 64);
    dispatch_args_for(dispatch_tasks_2, params.counters[counter_tasks + 1]);
}
