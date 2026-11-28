// veach: canonical
// The CPU batch interpreter of field programs (promoted from spike SP-06). Canonical code (ADR 0043): only + - * /
// sqrt, floor, fma, float/int conversions and integer hashing, evaluated in a fixed order, so a batch gives the same
// bits as one sample at a time and the same bits on every platform (ADR 0071 rule 3). Each op runs over a whole batch
// before the next op starts; the noise is force-inlined into the batch loops so they vectorize (SP-06: 8.7 M samples/s
// without, 20 M with).

#include <veach/procgen/field_graph.hpp>

#include <veach/core/assert.hpp>
#include <veach/core/jobs.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>

#if defined(_MSC_VER) && !defined(__clang__)
#define VEACH_PROCGEN_INLINE __forceinline
#else
#define VEACH_PROCGEN_INLINE [[gnu::always_inline]] inline
#endif

namespace veach::procgen {

namespace {

// ---- veach:fbm@1's gradient noise (mirrored by tests/sp06/shaders/sp06_field_noise.slang) ---------------------------

// Lattice hash (ADR 0071 rule 2, frozen with veach:fbm@1): per-axis odd multipliers, xor-combined with the octave's
// seed, then Wellons' lowbias32 finalizer. 32-bit integer only: bit-exact on every CPU and GPU.
constexpr std::uint32_t prime_x = 0x8DA6B343u;
constexpr std::uint32_t prime_y = 0xD8163841u;
constexpr std::uint32_t prime_z = 0xCB1AB31Fu;

VEACH_PROCGEN_INLINE std::uint32_t finalize(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

/// Perlin's improved-noise gradients (the 12 cube edges, 4 repeated), chosen by the hash's low 4 bits. Selects and
/// one add: exact on every evaluator.
VEACH_PROCGEN_INLINE float grad(std::uint32_t h, float x, float y, float z) {
    const std::uint32_t k = h & 15u;
    const float u = k < 8u ? x : y;
    const float v = k < 4u ? y : (k == 12u || k == 14u ? x : z);
    return ((k & 1u) != 0u ? -u : u) + ((k & 2u) != 0u ? -v : v);
}

VEACH_PROCGEN_INLINE float lerp(float a, float b, float t) {
    return a + t * (b - a);
}

/// a * b + c rounded once (ADR 0071 rule 1): the two places where a GPU's fast math fuses anyway and the unfused
/// rounding is large, the lattice fraction p * f - floor and the fade polynomial, which cancels from 10 to 1 near
/// t = 1. With hardware FMA this is std::fma; without (x64 SSE2, MSVC's default) the product is exact in double
/// (24 + 24 bits) and so is the sum for these operands (at most 48 significant bits), so rounding the double to float
/// gives the same bits.
VEACH_PROCGEN_INLINE float fused(float a, float b, float c) {
#if defined(__ARM_FEATURE_FMA) || defined(__FMA__)
    return std::fma(a, b, c);
#else
    return static_cast<float>(static_cast<double>(a) * static_cast<double>(b) + static_cast<double>(c));
#endif
}

VEACH_PROCGEN_INLINE float fade(float t) {
    return t * t * t * fused(t, fused(t, 6.0f, -15.0f), 10.0f);
}

/// The noise inside one lattice cell: integer cell coordinates and the fraction within it.
VEACH_PROCGEN_INLINE float cell_noise(std::uint32_t ix, std::uint32_t iy, std::uint32_t iz, float dx, float dy,
                                      float dz, std::uint32_t seed) {
    const std::uint32_t x0 = ix * prime_x;
    const std::uint32_t x1 = (ix + 1u) * prime_x;
    const std::uint32_t y0 = iy * prime_y;
    const std::uint32_t y1 = (iy + 1u) * prime_y;
    const std::uint32_t z0 = (iz * prime_z) ^ seed;
    const std::uint32_t z1 = ((iz + 1u) * prime_z) ^ seed;
    const float n000 = grad(finalize(x0 ^ y0 ^ z0), dx, dy, dz);
    const float n100 = grad(finalize(x1 ^ y0 ^ z0), dx - 1.0f, dy, dz);
    const float n010 = grad(finalize(x0 ^ y1 ^ z0), dx, dy - 1.0f, dz);
    const float n110 = grad(finalize(x1 ^ y1 ^ z0), dx - 1.0f, dy - 1.0f, dz);
    const float n001 = grad(finalize(x0 ^ y0 ^ z1), dx, dy, dz - 1.0f);
    const float n101 = grad(finalize(x1 ^ y0 ^ z1), dx - 1.0f, dy, dz - 1.0f);
    const float n011 = grad(finalize(x0 ^ y1 ^ z1), dx, dy - 1.0f, dz - 1.0f);
    const float n111 = grad(finalize(x1 ^ y1 ^ z1), dx - 1.0f, dy - 1.0f, dz - 1.0f);
    const float ux = fade(dx);
    const float uy = fade(dy);
    const float uz = fade(dz);
    const float nx00 = lerp(n000, n100, ux);
    const float nx10 = lerp(n010, n110, ux);
    const float nx01 = lerp(n001, n101, ux);
    const float nx11 = lerp(n011, n111, ux);
    return lerp(lerp(nx00, nx10, uy), lerp(nx01, nx11, uy), uz);
}

/// One octave at a float position times f (the cube-face domain, SP-06's arithmetic): the cell is floor(p * f) and
/// the fraction p * f - cell, fused.
VEACH_PROCGEN_INLINE float noise(float px, float py, float pz, float f, std::uint32_t seed) {
    const float fx = std::floor(px * f);
    const float fy = std::floor(py * f);
    const float fz = std::floor(pz * f);
    const float dx = fused(px, f, -fx);
    const float dy = fused(py, f, -fy);
    const float dz = fused(pz, f, -fz);
    return cell_noise(static_cast<std::uint32_t>(static_cast<std::int32_t>(fx)),
                      static_cast<std::uint32_t>(static_cast<std::int32_t>(fy)),
                      static_cast<std::uint32_t>(static_cast<std::int32_t>(fz)), dx, dy, dz, seed);
}

/// One octave at a double position times f (the lattice domain): q = p * f in double, the cell floor(q) wrapped to 32
/// bits, the fraction q - cell (exact in double) rounded to float. Every input is a function of the position alone.
VEACH_PROCGEN_INLINE float noise(double px, double py, double pz, double f, std::uint32_t seed) {
    const double qx = px * f;
    const double qy = py * f;
    const double qz = pz * f;
    const double cx = std::floor(qx);
    const double cy = std::floor(qy);
    const double cz = std::floor(qz);
    return cell_noise(static_cast<std::uint32_t>(static_cast<std::int64_t>(cx)),
                      static_cast<std::uint32_t>(static_cast<std::int64_t>(cy)),
                      static_cast<std::uint32_t>(static_cast<std::int64_t>(cz)), static_cast<float>(qx - cx),
                      static_cast<float>(qy - cy), static_cast<float>(qz - cz), seed);
}

inline float as_float(std::uint32_t bits) {
    return std::bit_cast<float>(bits);
}

inline double as_double(const std::uint32_t* words) {
    return std::bit_cast<double>(std::uint64_t{words[0]} | (std::uint64_t{words[1]} << 32));
}

// ---- batches and kernels
// ---------------------------------------------------------------------------------------------

/// One batch of samples. A domain fills either the float directions (cube faces) or the double positions (lattice).
/// Nothing else reaches a node: no level, no step, no chunk.
struct Batch {
    alignas(64) float x[max_batch];
    alignas(64) float y[max_batch];
    alignas(64) float z[max_batch];
    alignas(64) double px[max_batch];
    alignas(64) double py[max_batch];
    alignas(64) double pz[max_batch];
    alignas(64) float reg[max_registers][max_batch];
};

struct FaceDomain {
    static void octave(const Batch& b, float* __restrict out, std::size_t n, float f, double /*lattice_f*/, float a,
                       std::uint32_t seed) {
        const float* __restrict px = b.x;
        const float* __restrict py = b.y;
        const float* __restrict pz = b.z;
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = out[i] + a * noise(px[i], py[i], pz[i], f, seed);
        }
    }
};

struct LatticeDomain {
    static void octave(const Batch& b, float* __restrict out, std::size_t n, float /*face_f*/, double f, float a,
                       std::uint32_t seed) {
        const double* __restrict px = b.px;
        const double* __restrict py = b.py;
        const double* __restrict pz = b.pz;
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = out[i] + a * noise(px[i], py[i], pz[i], f, seed);
        }
    }
};

template <class Domain>
void fbm_kernel(const Batch& b, float* __restrict out, std::size_t n, const std::uint32_t* payload) {
    const std::uint32_t octaves = payload[0];
    float frequency = as_float(payload[1]);
    const float lacunarity = as_float(payload[2]);
    const float gain = as_float(payload[3]);
    float amplitude = as_float(payload[4]);
    const std::uint32_t* lattice_f = payload + 5 + octaves;
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = 0.0f;
    }
    for (std::uint32_t o = 0; o < octaves; ++o) {
        Domain::octave(b, out, n, frequency, as_double(lattice_f + 2 * o), amplitude, payload[5 + o]);
        frequency = frequency * lacunarity;
        amplitude = amplitude * gain;
    }
}

template <class Domain>
void run(const FieldProgram& program, Batch& b, std::size_t n) {
    const std::uint32_t* w = program.words.data();
    const std::size_t end = program.words.size();
    std::size_t pc = 0;
    while (pc < end) {
        const auto op = static_cast<Op>(w[pc]);
        float* __restrict d = b.reg[w[pc + 1]];
        const float* __restrict ra = b.reg[w[pc + 2]];
        const float* __restrict rb = b.reg[w[pc + 3]];
        const float* __restrict rc = b.reg[w[pc + 4]];
        const std::uint32_t* payload = w + pc + op_header_words;
        switch (op) {
            case Op::fbm: fbm_kernel<Domain>(b, d, n, payload); break;
            case Op::ridge: {
                const float offset = as_float(payload[0]);
                for (std::size_t i = 0; i < n; ++i) {
                    const float r = offset - std::fabs(ra[i]);
                    d[i] = r * r;
                }
                break;
            }
            case Op::remap: {
                const float m = as_float(payload[0]);
                const float k = as_float(payload[1]);
                for (std::size_t i = 0; i < n; ++i) {
                    d[i] = ra[i] * m + k;
                }
                break;
            }
            case Op::clamp: {
                const float lo = as_float(payload[0]);
                const float hi = as_float(payload[1]);
                for (std::size_t i = 0; i < n; ++i) {
                    d[i] = std::min(std::max(ra[i], lo), hi);
                }
                break;
            }
            case Op::blend:
                for (std::size_t i = 0; i < n; ++i) {
                    d[i] = ra[i] + rc[i] * (rb[i] - ra[i]);
                }
                break;
            case Op::add:
                for (std::size_t i = 0; i < n; ++i) {
                    d[i] = ra[i] + rb[i];
                }
                break;
            case Op::mul:
                for (std::size_t i = 0; i < n; ++i) {
                    d[i] = ra[i] * rb[i];
                }
                break;
        }
        pc += op_header_words + w[pc + 5];
    }
}

/// Evaluates samples [begin, end) of `samples`: fill(batch, first, n) writes the positions of samples
/// [first, first + n) into the batch, and each output lands at out[o * samples + i].
template <class Domain, class Fill>
void drive(const FieldProgram& program, std::size_t samples, std::size_t begin, std::size_t end, std::span<float> out,
           std::uint32_t width, Fill&& fill) {
    VEACH_ASSERT(width >= 1 && width <= max_batch, "field graph: EvalOptions::batch must be 1 to 256");
    auto batch = std::make_unique_for_overwrite<Batch>();
    for (std::size_t s = begin; s < end; s += width) {
        const std::size_t n = std::min<std::size_t>(width, end - s);
        fill(*batch, s, n);
        run<Domain>(program, *batch, n);
        for (std::size_t o = 0; o < program.outputs.size(); ++o) {
            std::memcpy(out.data() + o * samples + s, batch->reg[program.outputs[o].reg], n * sizeof(float));
        }
    }
}

void check_out(const FieldProgram& program, std::size_t samples, std::span<float> out) {
    VEACH_ASSERT(out.size() == program.outputs.size() * samples,
                 "field graph: `out` must hold program.outputs.size() * samples floats (planar)");
}

/// Lattice point to body-fixed meters: origin + i * spacing, in double.
void lattice_position(const Lattice& lattice, const LatticePoint& p, Batch& b, std::size_t k) {
    b.px[k] = lattice.origin_m[0] + static_cast<double>(p[0]) * lattice.spacing_m;
    b.py[k] = lattice.origin_m[1] + static_cast<double>(p[1]) * lattice.spacing_m;
    b.pz[k] = lattice.origin_m[2] + static_cast<double>(p[2]) * lattice.spacing_m;
}

void evaluate_box(const FieldProgram& program, const Lattice& lattice, const LatticeBox& box, std::size_t begin,
                  std::size_t end, std::span<float> out, std::uint32_t width) {
    drive<LatticeDomain>(program, box.size(), begin, end, out, width, [&](Batch& b, std::size_t first, std::size_t n) {
        for (std::size_t k = 0; k < n; ++k) {
            lattice_position(lattice, box.point(first + k), b, k);
        }
    });
}

}  // namespace

LatticePoint LatticeBox::point(std::size_t index) const {
    const std::size_t x = index % count[0];
    const std::size_t y = (index / count[0]) % count[1];
    const std::size_t z = index / (std::size_t{count[0]} * count[1]);
    return {first[0] + step * static_cast<std::int64_t>(x), first[1] + step * static_cast<std::int64_t>(y),
            first[2] + step * static_cast<std::int64_t>(z)};
}

void evaluate(const FieldProgram& program, const Lattice& lattice, std::span<const LatticePoint> points,
              std::span<float> out, EvalOptions options) {
    check_out(program, points.size(), out);
    drive<LatticeDomain>(program, points.size(), 0, points.size(), out, options.batch,
                         [&](Batch& b, std::size_t first, std::size_t n) {
                             for (std::size_t k = 0; k < n; ++k) {
                                 lattice_position(lattice, points[first + k], b, k);
                             }
                         });
}

void evaluate(const FieldProgram& program, const Lattice& lattice, const LatticeBox& box, std::span<float> out,
              EvalOptions options) {
    check_out(program, box.size(), out);
    evaluate_box(program, lattice, box, 0, box.size(), out, options.batch);
}

void evaluate(JobSystem& jobs, const FieldProgram& program, const Lattice& lattice, const LatticeBox& box,
              std::span<float> out) {
    check_out(program, box.size(), out);
    jobs.parallel_for("procgen: field", box.size(), parallel_grain, [&](std::size_t begin, std::size_t end) {
        evaluate_box(program, lattice, box, begin, end, out, max_batch);
    });
}

void evaluate(const FieldProgram& program, const FaceRegion& r, std::span<float> out, EvalOptions options) {
    check_out(program, r.size(), out);
    const std::uint32_t res = r.resolution;
    const float step = 2.0f / static_cast<float>(res);
    drive<FaceDomain>(program, r.size(), 0, r.size(), out, options.batch,
                      [&](Batch& b, std::size_t first, std::size_t n) {
                          for (std::size_t k = 0; k < n; ++k) {
                              const auto i = static_cast<std::uint32_t>((first + k) % res);
                              const auto j = static_cast<std::uint32_t>((first + k) / res);
                              const float u = (static_cast<float>(i) + 0.5f) * step - 1.0f;
                              const float v = 1.0f - (static_cast<float>(j) + 0.5f) * step;
                              const float cx = r.center[0] + u * r.right[0] + v * r.up[0];
                              const float cy = r.center[1] + u * r.right[1] + v * r.up[1];
                              const float cz = r.center[2] + u * r.right[2] + v * r.up[2];
                              const float len = std::sqrt(std::fma(cx, cx, std::fma(cy, cy, cz * cz)));
                              b.x[k] = cx / len;
                              b.y[k] = cy / len;
                              b.z[k] = cz / len;
                          }
                      });
}

void evaluate(const FieldProgram& program, std::span<const std::array<float, 3>> unit_directions, std::span<float> out,
              EvalOptions options) {
    check_out(program, unit_directions.size(), out);
    drive<FaceDomain>(program, unit_directions.size(), 0, unit_directions.size(), out, options.batch,
                      [&](Batch& b, std::size_t first, std::size_t n) {
                          for (std::size_t k = 0; k < n; ++k) {
                              b.x[k] = unit_directions[first + k][0];
                              b.y[k] = unit_directions[first + k][1];
                              b.z[k] = unit_directions[first + k][2];
                          }
                      });
}

float gradient_noise(float x, float y, float z, float frequency, std::uint32_t seed) {
    return noise(x, y, z, frequency, seed);
}

}  // namespace veach::procgen
