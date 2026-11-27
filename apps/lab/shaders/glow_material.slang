// The lab's proof of the material model (docs/adr/0035-material-model.md): GlowMaterial overrides only
// evalEmission. Its emission is the standard one in diagonal stripes, so the stripes show which code ran.
import veach.material;

struct GlowMaterial : IMaterial {
    override float3 evalEmission(MaterialContext c) {
        const float stripes = step(0.5, frac((c.surface.uv.x + c.surface.uv.y) * 3.0));
        return standardEmission(c) * stripes;
    }
};
