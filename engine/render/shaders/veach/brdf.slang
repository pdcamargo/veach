// The glTF metallic-roughness BRDF, shared by the path tracer and the real-time renderer (ADR 0037): Lambert
// diffuse and GGX specular (Smith height-correlated, Schlick Fresnel), sampled by visible normals.

import veach.material;
import veach.random;

static const float pi = 3.14159265358979;

float luminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

struct Frame {
    float3 t;
    float3 b;
    float3 n;
    float3 to_local(float3 v) { return float3(dot(v, t), dot(v, b), dot(v, n)); }
    float3 to_world(float3 v) { return v.x * t + v.y * b + v.z * n; }
};

Frame frame_of(float3 n) {  // Duff et al. 2017
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float b = n.x * n.y * a;
    Frame f;
    f.t = float3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    f.b = float3(b, s + n.y * n.y * a, -n.y);
    f.n = n;
    return f;
}

struct Brdf {
    float3 diffuse;  ///< base color x (1 - metallic)
    float3 f0;
    float alpha;  ///< roughness squared
    float specular_probability;
};

Brdf brdf_of(SurfaceSample s, float n_dot_v) {
    Brdf b;
    b.diffuse = s.base_color * (1.0 - s.metallic);
    b.f0 = lerp(float3(0.04, 0.04, 0.04), s.base_color, s.metallic);
    const float roughness = max(s.roughness, 0.02);
    b.alpha = roughness * roughness;
    const float spec = luminance(b.f0 + (1.0 - b.f0) * pow(1.0 - n_dot_v, 5.0));
    const float diff = luminance(b.diffuse);
    b.specular_probability = spec + diff > 0.0 ? clamp(spec / (spec + diff), 0.1, 0.9) : 0.5;
    return b;
}

float ggx_d(float n_dot_h, float a2) {
    const float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (pi * d * d);
}

float smith_g1(float n_dot_x, float a2) {
    return 2.0 * n_dot_x / (n_dot_x + sqrt(a2 + (1.0 - a2) * n_dot_x * n_dot_x));
}

/// f(l) * cos(l) for local unit vectors (z = shading normal), and its sampling pdf (solid angle).
float3 brdf_eval(Brdf b, float3 v, float3 l, out float pdf) {
    pdf = 0.0;
    if (l.z <= 0.0 || v.z <= 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 h = normalize(v + l);
    const float a2 = b.alpha * b.alpha;
    const float d = ggx_d(h.z, a2);
    const float vis = 0.5 / (l.z * sqrt(v.z * v.z * (1.0 - a2) + a2) + v.z * sqrt(l.z * l.z * (1.0 - a2) + a2));
    const float3 f = b.f0 + (1.0 - b.f0) * pow(1.0 - saturate(dot(v, h)), 5.0);
    const float3 specular = d * vis * f;
    const float3 diffuse = (1.0 - f) * b.diffuse / pi;
    const float pdf_specular = smith_g1(v.z, a2) * d / (4.0 * v.z);  // VNDF
    const float pdf_diffuse = l.z / pi;
    pdf = b.specular_probability * pdf_specular + (1.0 - b.specular_probability) * pdf_diffuse;
    return (specular + diffuse) * l.z;
}

/// Heitz 2018: a visible-normal sample of GGX, in local space.
float3 sample_vndf(float3 v, float alpha, float2 u) {
    const float3 vh = normalize(float3(alpha * v.x, alpha * v.y, v.z));
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lensq > 0.0 ? float3(-vh.y, vh.x, 0.0) / sqrt(lensq) : float3(1.0, 0.0, 0.0);
    const float3 t2 = cross(vh, t1);
    const float r = sqrt(u.x);
    const float phi = 2.0 * pi * u.y;
    const float p1 = r * cos(phi);
    const float s = 0.5 * (1.0 + vh.z);
    const float p2 = (1.0 - s) * sqrt(1.0 - p1 * p1) + s * r * sin(phi);
    const float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    return normalize(float3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
}

float3 brdf_sample(Brdf b, float3 v, inout Rng rng) {
    const float2 u = float2(rng.next(), rng.next());
    if (rng.next() < b.specular_probability) {
        return reflect(-v, sample_vndf(v, b.alpha, u));
    }
    const float r = sqrt(u.x);
    const float phi = 2.0 * pi * u.y;
    return float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u.x)));
}

float power_heuristic(float a, float b) {
    return a * a / max(a * a + b * b, 1e-20);
}
