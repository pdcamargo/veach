// Spike SP-06 (VCH-082): the field graph's shared GPU code, used by both the interpreter and the hand-written kernel.
// Mirrors the CPU interpreter in tests/sp06/field_graph.cpp operation for operation; the GPU keeps fast math, so the
// results agree within rounding, not bit for bit (ADR 0043: GPU output is visual-only).

import veach.bindless;

/// Mirrors sp06::FieldParams (tests/sp06/field_gpu.cpp).
struct FieldParams {
    DescriptorHandle<StructuredBuffer<uint>> program;  ///< the op list (unused by the hand-written kernel)
    DescriptorHandle<RWStructuredBuffer<float>> output;
    float4 center;  ///< xyz: the face's center; w: 2 / resolution
    float4 right;
    float4 up;
    uint resolution;
    uint words;   ///< op-list length
    uint result;  ///< register holding the output
    uint unused;
};

/// Correctly rounded sqrt and division. Metal's fast math approximates both by a few ulp, and the direction's
/// error, multiplied by the noise frequency, is most of the CPU/GPU difference. D3D's sqrt and division are already
/// IEEE-exact on WARP.
float exact_sqrt(float x) {
    __target_switch {
    case metal:
        __intrinsic_asm "metal::precise::sqrt($0)";
    default:
        return sqrt(x);
    }
}

float exact_divide(float a, float b) {
    __target_switch {
    case metal:
        __intrinsic_asm "metal::precise::divide($0, $1)";
    default:
        return a / b;
    }
}

/// The unit-sphere direction of sample `id` (mirrors face_positions in field_graph.cpp).
float3 face_direction(FieldParams f, uint2 id) {
    const float step = f.center.w;
    const float u = (float(id.x) + 0.5) * step - 1.0;
    const float v = 1.0 - (float(id.y) + 0.5) * step;
    const float3 c = f.center.xyz + u * f.right.xyz + v * f.up.xyz;
    const float len = exact_sqrt(fma(c.x, c.x, fma(c.y, c.y, c.z * c.z)));
    return float3(exact_divide(c.x, len), exact_divide(c.y, len), exact_divide(c.z, len));
}

static const uint sp06_prime_x = 0x8DA6B343u;
static const uint sp06_prime_y = 0xD8163841u;
static const uint sp06_prime_z = 0xCB1AB31Fu;

uint sp06_finalize(uint h) {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

float sp06_grad(uint h, float x, float y, float z) {
    const uint k = h & 15u;
    const float u = k < 8u ? x : y;
    const float v = k < 4u ? y : ((k == 12u || k == 14u) ? x : z);
    return ((k & 1u) != 0u ? -u : u) + ((k & 2u) != 0u ? -v : v);
}


float sp06_lerp(float a, float b, float t) {
    return a + t * (b - a);
}

/// a * b + c rounded once, as the CPU's fused() in field_graph.cpp, used where fusing or not changes the result
/// most (the lattice fraction and the fade polynomial). Metal: fma, correctly rounded. HLSL has no float fma, and
/// `mad` may or may not fuse, so the product goes through double, where it is exact (24 + 24 bits), as is the sum for
/// these operands: rounding to float gives the same bits.
float3 fused(float3 a, float3 b, float3 c) {
    __target_switch {
    case metal:
        return fma(a, b, c);
    default:
        return float3(double3(a) * double3(b) + double3(c));
    }
}

float3 lattice_fraction(float3 p, float f, float3 cell) {
    return fused(p, float3(f), -cell);
}

float3 sp06_fade(float3 t) {
    return t * t * t * fused(t, fused(t, float3(6.0), float3(-15.0)), float3(10.0));
}

/// One octave of 3D gradient noise at p * f, roughly in [-1, 1].
float gradient_noise(float3 p, float f, uint seed) {
    const float3 fl = floor(p * f);
    const float3 d = lattice_fraction(p, f, fl);
    const uint ix = uint(int(fl.x));
    const uint iy = uint(int(fl.y));
    const uint iz = uint(int(fl.z));
    const uint x0 = ix * sp06_prime_x;
    const uint x1 = (ix + 1u) * sp06_prime_x;
    const uint y0 = iy * sp06_prime_y;
    const uint y1 = (iy + 1u) * sp06_prime_y;
    const uint z0 = (iz * sp06_prime_z) ^ seed;
    const uint z1 = ((iz + 1u) * sp06_prime_z) ^ seed;
    const float n000 = sp06_grad(sp06_finalize(x0 ^ y0 ^ z0), d.x, d.y, d.z);
    const float n100 = sp06_grad(sp06_finalize(x1 ^ y0 ^ z0), d.x - 1.0, d.y, d.z);
    const float n010 = sp06_grad(sp06_finalize(x0 ^ y1 ^ z0), d.x, d.y - 1.0, d.z);
    const float n110 = sp06_grad(sp06_finalize(x1 ^ y1 ^ z0), d.x - 1.0, d.y - 1.0, d.z);
    const float n001 = sp06_grad(sp06_finalize(x0 ^ y0 ^ z1), d.x, d.y, d.z - 1.0);
    const float n101 = sp06_grad(sp06_finalize(x1 ^ y0 ^ z1), d.x - 1.0, d.y, d.z - 1.0);
    const float n011 = sp06_grad(sp06_finalize(x0 ^ y1 ^ z1), d.x, d.y - 1.0, d.z - 1.0);
    const float n111 = sp06_grad(sp06_finalize(x1 ^ y1 ^ z1), d.x - 1.0, d.y - 1.0, d.z - 1.0);
    const float3 u = sp06_fade(d);
    const float ux = u.x;
    const float uy = u.y;
    const float uz = u.z;
    const float nx00 = sp06_lerp(n000, n100, ux);
    const float nx10 = sp06_lerp(n010, n110, ux);
    const float nx01 = sp06_lerp(n001, n101, ux);
    const float nx11 = sp06_lerp(n011, n111, ux);
    return sp06_lerp(sp06_lerp(nx00, nx10, uy), sp06_lerp(nx01, nx11, uy), uz);
}
