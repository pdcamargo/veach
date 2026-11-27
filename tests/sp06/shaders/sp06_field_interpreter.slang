// Spike SP-06 (VCH-082): the GPU field-graph interpreter. Every thread walks the same op list (a storage buffer
// written by sp06::compile, layout in tests/sp06/field_graph.hpp), so control flow is uniform: no divergence, only the
// cost of reading the ops and of the register array.

import veach.bindless;
import sp06_field_noise;

[[vk::push_constant]] ConstantBuffer<FieldParams> field;

uint word(uint i) {
    return field.program[i];
}

float fbm_op(float3 p, uint at) {
    const uint octaves = word(at);
    float frequency = asfloat(word(at + 1));
    const float lacunarity = asfloat(word(at + 2));
    const float gain = asfloat(word(at + 3));
    float amplitude = asfloat(word(at + 4));
    float sum = 0.0;
    for (uint o = 0; o < octaves; ++o) {
        sum = sum + amplitude * gradient_noise(p, frequency, word(at + 5 + o));
        frequency = frequency * lacunarity;
        amplitude = amplitude * gain;
    }
    return sum;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void field_interpret(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= uint2(field.resolution, field.resolution))) {
        return;
    }
    const float3 p = face_direction(field, id.xy);
    float r[8] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    uint pc = 0;
    while (pc < field.words) {
        const uint op = word(pc);
        const float a = r[word(pc + 2) & 7u];
        const float b = r[word(pc + 3) & 7u];
        const float c = r[word(pc + 4) & 7u];
        const uint at = pc + 6;
        float v = 0.0;
        switch (op) {
        case 1:  // veach:fbm@1
            v = fbm_op(p, at);
            break;
        case 2: {  // veach:ridge@1
            const float t = asfloat(word(at)) - abs(a);
            v = t * t;
            break;
        }
        case 3:  // veach:remap@1
            v = a * asfloat(word(at)) + asfloat(word(at + 1));
            break;
        case 4:  // veach:clamp@1
            v = min(max(a, asfloat(word(at))), asfloat(word(at + 1)));
            break;
        case 5:  // veach:blend@1
            v = a + c * (b - a);
            break;
        case 6:  // veach:add@1
            v = a + b;
            break;
        case 7:  // veach:mul@1
            v = a * b;
            break;
        default:
            break;
        }
        r[word(pc + 1) & 7u] = v;
        pc = at + word(pc + 5);
    }
    field.output[id.y * field.resolution + id.x] = r[field.result & 7u];
}
