// sky.AC2: the GPU's transmittance integrator (the one that fills the transmittance LUT) at chosen points, read back
// and compared with render::atmosphere_transmittance (tests/sky_tests.cpp). Mirrors AtmosphereProbeParams there.

import veach.atmosphere;
import veach.bindless;

struct AtmosphereProbeParams {
    DescriptorHandle<StructuredBuffer<AtmosphereParams>> medium;
    DescriptorHandle<StructuredBuffer<float4>> queries;  ///< x = radius (km), y = zenith cosine
    DescriptorHandle<RWStructuredBuffer<float4>> results;
    uint count;
};
[[vk::push_constant]] ConstantBuffer<AtmosphereProbeParams> probe;

[shader("compute")]
[numthreads(64, 1, 1)]
void transmittance_probe(uint3 id: SV_DispatchThreadID) {
    if (id.x >= probe.count) {
        return;
    }
    const float4 q = probe.queries[id.x];
    probe.results[id.x] = float4(integrate_transmittance(probe.medium[0], q.x, q.y), 1.0);
}
