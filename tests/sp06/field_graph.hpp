#pragma once

// Spike SP-06 (VCH-082), throwaway: one field graph evaluated three ways (docs/research/spikes/sp-06-field-graph.md).
//
// A field graph is JSON (ADR 0052): stable node ids, versioned node types, unit-typed lengths. compile() checks it,
// folds the body's constants (seed, radius) into it and lowers it to a flat op list, which is both the CPU batch
// interpreter's program and the GPU interpreter's storage buffer (tests/sp06/shaders/sp06_field_interpreter.slang).
//
// Op list layout, in 32-bit words: [opcode, dst, a, b, c, n, payload[n]] per op, in dependency order. Registers are
// scalar floats; the only vec3 is the input position. Floats in the payload are their IEEE bits.

#include <veach/core/error.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace veach::sp06 {

enum class Op : std::uint32_t {
    fbm = 1,    ///< veach:fbm@1     payload: octaves, frequency, lacunarity, gain, amplitude, seed[octaves]
    ridge = 2,  ///< veach:ridge@1   payload: offset            (offset - |a|)^2
    remap = 3,  ///< veach:remap@1   payload: mul, add          a * mul + add
    clamp = 4,  ///< veach:clamp@1   payload: lo, hi
    blend = 5,  ///< veach:blend@1   a + c * (b - a)
    add = 6,    ///< veach:add@1     a + b
    mul = 7,    ///< veach:mul@1     a * b
};

inline constexpr std::uint32_t max_registers = 8;
inline constexpr std::uint32_t max_octaves = 8;
inline constexpr std::uint32_t op_header_words = 6;

/// What the body supplies at compile time (ADR 0052: a node's seed is hash(body_seed, hash_id(node_id))).
struct BodyContext {
    std::uint64_t seed = 0;
    double radius_planet_m = 6.0e6;  ///< p_planet = unit direction * radius
};

struct Program {
    std::vector<std::uint32_t> words;
    std::uint32_t registers = 0;
    std::uint32_t output = 0;         ///< register holding the graph's output
    std::uint32_t noise_octaves = 0;  ///< gradient-noise evaluations per sample (for the cost tables)
};

/// Parses and compiles a field graph. Errors name the node and the fix.
[[nodiscard]] Result<Program> compile(std::string_view json, const BodyContext& body);

/// The spike's graph, tests/sp06/terrain.field.json, as text.
[[nodiscard]] Result<std::string> terrain_graph_json();

/// A square region of a cube face: sample (i, j) of `resolution`^2 lies at center + u * right + v * up with
/// u = (i + 0.5) * 2 / resolution - 1 and v = 1 - (j + 0.5) * 2 / resolution, projected onto the unit sphere.
struct FaceRegion {
    float center[3] = {0.0f, 0.0f, 1.0f};
    float right[3] = {1.0f, 0.0f, 0.0f};
    float up[3] = {0.0f, 1.0f, 0.0f};
    std::uint32_t resolution = 256;
};

/// Samples per batch of the CPU interpreter: every op runs over a whole batch before the next op starts.
inline constexpr std::size_t batch_size = 256;

/// CPU batch interpreter over `region`, rows top to bottom. Canonical: only + - * / sqrt, floor and integer hashing.
void evaluate_batch(const Program& program, const FaceRegion& region, std::span<float> out);
/// The same interpreter one sample at a time (batch of 1): the scalar reference.
void evaluate_scalar(const Program& program, const FaceRegion& region, std::span<float> out);

/// The scalar interpreter on one point of the unit sphere (CPU only).
[[nodiscard]] float evaluate_point(const Program& program, float x, float y, float z);

/// The 3D gradient noise every evaluator shares: one octave at (x, y, z) * frequency, roughly in [-1, 1].
[[nodiscard]] float gradient_noise(float x, float y, float z, float frequency, std::uint32_t seed);

}  // namespace veach::sp06
