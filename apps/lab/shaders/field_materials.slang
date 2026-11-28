// The SP-16 vegetation field through the engine's renderer (VEACH_SP16=engine, sp16_spike.cpp): alpha-tested cards
// whose coverage is drawn from their UV instead of a texture, so the field needs no runtime textures. FieldGrass: a
// row of blades on each card; FieldLeaves: a jittered grid of leaf discs. Both tint the record's base color.
import veach.material;

float field_hash(float2 p) {
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

/// Cards are drawn with v = 1 at the bottom (sp16_spike.cpp's MeshBuilder::card).
struct FieldGrass : IMaterial {
    override float4 evalBaseColor(MaterialContext c) {
        const float x = c.surface.uv.x * 7.0;
        const float cell = floor(x);
        const float h = 1.0 - c.surface.uv.y;  // 0 at the root, 1 at the card's top
        const float tall = 0.55 + 0.45 * field_hash(float2(cell, 3.0));
        const float lean = (field_hash(float2(cell, 7.0)) - 0.5) * 0.6;
        const float across = frac(x) - 0.5 - lean * h;
        const float width = 0.38 * saturate(1.0 - h / tall);
        const float alpha = abs(across) < width ? 1.0 : 0.0;
        const float4 base = standardBaseColor(c);
        return float4(base.rgb * (0.6 + 0.4 * h + 0.2 * field_hash(float2(cell, 11.0))), alpha);
    }
};

struct FieldLeaves : IMaterial {
    override float4 evalBaseColor(MaterialContext c) {
        const float2 grid = c.surface.uv * 4.0;
        const float2 cell = floor(grid);
        const float2 jitter = float2(field_hash(cell), field_hash(cell + 17.0)) - 0.5;
        const float radius = 0.28 + 0.16 * field_hash(cell + 5.0);
        const float alpha = length(frac(grid) - 0.5 - jitter * 0.3) < radius ? 1.0 : 0.0;
        const float4 base = standardBaseColor(c);
        return float4(base.rgb * (0.75 + 0.5 * field_hash(cell + 9.0)), alpha);
    }
};
