// Reprojection and disocclusion, shared by every temporal pass (ADR 0040). The surface pass decides, once per
// pixel, which of the four bilinear taps around its previous position hold the same surface last frame (bits in
// motion.w), and which of the 3x3 pixels around it do (bits in motion.z); ReSTIR, the denoiser and the upscaler
// read that decision here. When no bilinear tap matches (an edge whose samples moved with the jitter), the closest
// matching pixel of the 3x3 is used alone.

/// Bilinear taps around the previous position, in the order of the validity bits.
static const int2 history_taps[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };

struct History {
    int2 base;       ///< the top-left tap, previous-frame pixels
    float4 weights;  ///< per tap, zero where invalid, summing to 1 when valid
    bool valid;      ///< some tap holds the same surface
};

/// The pixel containing the previous position of `pixel`'s center: the center of the 3x3 neighborhood.
int2 previous_pixel(uint2 pixel, float2 motion) {
    return int2(floor(float2(pixel) + 0.5 + motion));
}

/// Where the pixel's surface was last frame: `motion` is the surface pass's output (xy previous minus current
/// pixel position, z = valid 3x3 bits, w = valid bilinear-tap bits).
History history_of(uint2 pixel, float4 motion) {
    History h;
    const float2 previous = float2(pixel) + motion.xy;  // previous position of the pixel center, minus 0.5
    h.base = int2(floor(previous));
    const float2 f = previous - float2(h.base);
    const float4 bilinear = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    const uint bits = uint(motion.w + 0.5);
    const float4 valid = float4(float(bits & 1u), float((bits >> 1) & 1u), float((bits >> 2) & 1u), float((bits >> 3) & 1u));
    const float sum = dot(bilinear, valid);
    h.valid = sum > 1e-3;
    h.weights = h.valid ? bilinear * valid / sum : float4(0.0, 0.0, 0.0, 0.0);
    if (!h.valid) {
        const uint neighbors = uint(motion.z + 0.5);
        const int2 center = previous_pixel(pixel, motion.xy);
        float closest = 1e30;
        for (uint k = 0; k < 9; ++k) {
            const int2 q = center + int2(int(k % 3) - 1, int(k / 3) - 1);
            const float2 d = float2(q) - previous;
            if ((neighbors & (1u << k)) != 0 && dot(d, d) < closest) {
                closest = dot(d, d);
                h.base = q;
                h.valid = true;
            }
        }
        h.weights = h.valid ? float4(1.0, 0.0, 0.0, 0.0) : float4(0.0, 0.0, 0.0, 0.0);
    }
    return h;
}

/// Whether last frame's surface at a tap is this one: the same plane (distance under 1% of the view depth) and a
/// geometric normal within 25 degrees. `previous_point` and `previous_geometric` are this surface's position and
/// normal moved by last frame's transforms.
bool same_surface(float4 stored_position, float3 stored_geometric, float3 previous_point, float3 previous_geometric,
                  float depth) {
    if (stored_position.w <= 0.0) {
        return false;
    }
    const float plane = abs(dot(previous_geometric, stored_position.xyz - previous_point));
    return plane < 0.01 * depth && dot(previous_geometric, stored_geometric) > 0.906;
}
