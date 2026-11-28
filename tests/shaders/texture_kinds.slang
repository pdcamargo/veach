// 3D textures, 2D arrays and cube maps written and sampled through bindless handles (tests/rhi_texture_tests.cpp,
// docs/specs/rhi-textures/spec.md). Layout must match TextureKindsParams there (ADR 0027).

import veach.bindless;

struct TextureKindsParams {
    DescriptorHandle<RWTexture3D<float4>> volume_out;
    DescriptorHandle<Texture3D<float4>> volume;
    DescriptorHandle<Texture2DArray<float4>> layers;
    DescriptorHandle<TextureCube<float4>> cube;
    DescriptorHandle<RWTexture2DArray<float4>> faces_out;
    DescriptorHandle<SamplerState> sampler;
    DescriptorHandle<StructuredBuffer<float4>> queries;
    DescriptorHandle<RWStructuredBuffer<float4>> results;
    uint size;   // texels per side of what write_volume and write_faces write
    uint count;  // queries
    uint unused0;
    uint unused1;
};
[[vk::push_constant]] ConstantBuffer<TextureKindsParams> params;

/// Texel (x, y, z) of the volume holds (x, y, z, x + size * (y + size * z)): exact in half floats for size <= 8.
[shader("compute")]
[numthreads(4, 4, 4)]
void write_volume(uint3 id: SV_DispatchThreadID) {
    if (any(id >= params.size)) {
        return;
    }
    params.volume_out[id] = float4(float3(id), float(id.x + params.size * (id.y + params.size * id.z)));
}

/// Texel (x, y) of face f holds (x, y, f, 1).
[shader("compute")]
[numthreads(4, 4, 1)]
void write_faces(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.size)) {
        return;
    }
    params.faces_out[id] = float4(float(id.x), float(id.y), float(id.z), 1.0);
}

/// Query: (u, v, w, lod).
[shader("compute")]
[numthreads(64, 1, 1)]
void sample_volume(uint3 id: SV_DispatchThreadID) {
    if (id.x >= params.count) {
        return;
    }
    const float4 q = params.queries[id.x];
    params.results[id.x] = params.volume.SampleLevel(params.sampler, q.xyz, q.w);
}

/// Query: (u, v, layer, lod).
[shader("compute")]
[numthreads(64, 1, 1)]
void sample_layers(uint3 id: SV_DispatchThreadID) {
    if (id.x >= params.count) {
        return;
    }
    const float4 q = params.queries[id.x];
    params.results[id.x] = params.layers.SampleLevel(params.sampler, q.xyz, q.w);
}

/// Query: (direction, lod).
[shader("compute")]
[numthreads(64, 1, 1)]
void sample_cube(uint3 id: SV_DispatchThreadID) {
    if (id.x >= params.count) {
        return;
    }
    const float4 q = params.queries[id.x];
    params.results[id.x] = params.cube.SampleLevel(params.sampler, q.xyz, q.w);
}
