// Spike SP-06 (VCH-082): tests/sp06/terrain.field.json written by hand, for body seed 42 and radius 6,000 km, with
// every constant folded in: what a graph compiled to Slang (ADR 0052's D3) would produce. Same noise as the
// interpreter (sp06_field_noise.slang). The octave seeds are veach::hash(hash(42, hash_id(node)), octave) folded to
// 32 bits; the unit test checks them against sp06::compile.

import veach.bindless;
import sp06_field_noise;

[[vk::push_constant]] ConstantBuffer<FieldParams> field;

static const uint continents_seeds[6] = { 0x0D1C6AB3u, 0x2CA4B71Cu, 0x95510E19u, 0x376320F1u, 0x9BE8AC0Eu, 0x8413A8EDu };
static const uint mountains_seeds[5] = { 0x561C8B05u, 0x43BE9322u, 0x67CC6CCAu, 0x1E39FB4Bu, 0xC3058AD7u };

[shader("compute")]
[numthreads(8, 8, 1)]
void field_handwritten(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= uint2(field.resolution, field.resolution))) {
        return;
    }
    const float3 p = face_direction(field, id.xy);

    // continents: fbm, wavelength 3,000 km on a 6,000 km planet (frequency 2), 6 octaves.
    float continents = 0.0;
    float frequency = 2.0;
    float amplitude = 1.0;
    [unroll]
    for (uint o = 0; o < 6; ++o) {
        continents = continents + amplitude * gradient_noise(p, frequency, continents_seeds[o]);
        frequency = frequency * 2.0;
        amplitude = amplitude * 0.5;
    }

    // mountains: fbm, wavelength 600 km (frequency 10), 5 octaves.
    float mountains = 0.0;
    frequency = 10.0;
    amplitude = 1.0;
    [unroll]
    for (uint o = 0; o < 5; ++o) {
        mountains = mountains + amplitude * gradient_noise(p, frequency, mountains_seeds[o]);
        frequency = frequency * 2.0;
        amplitude = amplitude * 0.5;
    }

    const float ridge = 1.0 - abs(mountains);
    const float peaks = (ridge * ridge) * 0.65 + 0.35;
    const float land = continents * 0.3 + 0.3;
    const float mask = min(max(continents * 4.0 + -0.2, 0.0), 1.0);
    field.output[id.y * field.resolution + id.x] = land + mask * (peaks - land);
}
