#pragma once

// Field graphs (docs/adr/0052-procgen-runtime.md, docs/specs/procgen/spec.md; guide: docs/guides/procgen.md).
//
// A field graph is JSON: stable node ids, versioned node types (`veach:fbm@1`), lengths with a unit class. It is
// parsed once into a FieldGraph (body-independent, cached by hash()), compiled per body into a FieldProgram (seeds and
// body constants folded into a flat op list), and evaluated by the canonical CPU interpreter in batches. The op list is
// also what a GPU evaluator uploads: its layout is [opcode, dst, a, b, c, n, payload[n]] per op, 32-bit words, in
// dependency order, floats as their IEEE bits.
//
// Positions. A program is evaluated on positions only, never on a level of detail
// (docs/adr/0074-amendments-from-sp-08.md rule 1): a sample is a pure function of its position, so two chunks of
// different levels read the same bits at the points they share. Two domains:
// - Lattice points: integer coordinates on a body-fixed lattice of spacing `Lattice::spacing_m` (the LOD0 lattice of
//   docs/adr/0053-terrain-density-field.md). Each node computes what it needs from the integer point in double, so
//   the result does not depend on which chunk or batch the point came in. This is the domain of terrain density.
// - Cube-face regions: unit directions on the sphere in float32, with the body's radius folded into the frequencies
//   (SP-06's domain), for coarse maps and previews.
//
// Canonical (docs/adr/0043-determinism-tiers-and-strict-floating-point.md): the same bits on every platform, in
// batches or one sample at a time (docs/adr/0071-field-graph-precision-rules.md rule 3).

#include <veach/core/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace veach {
class JobSystem;
}

namespace veach::procgen {

/// The `"veach_graph"` format this build reads.
inline constexpr std::uint32_t field_graph_format = 1;

/// Opcodes of the op list. Frozen: a GPU evaluator reads the same numbers; a changed node is a new opcode.
enum class Op : std::uint32_t {
    fbm = 1,    ///< veach:fbm@1    payload: octaves, frequency (sphere), lacunarity, gain, amplitude, seed[octaves],
                ///<                lattice frequency[octaves] (doubles, low word first)
    ridge = 2,  ///< veach:ridge@1  payload: offset            (offset - |a|)^2
    remap = 3,  ///< veach:remap@1  payload: mul, add          a * mul + add
    clamp = 4,  ///< veach:clamp@1  payload: lo, hi
    blend = 5,  ///< veach:blend@1  a + c * (b - a)
    add = 6,    ///< veach:add@1    a + b
    mul = 7,    ///< veach:mul@1    a * b
};

inline constexpr std::uint32_t op_header_words = 6;
inline constexpr std::uint32_t max_registers = 16;
inline constexpr std::uint32_t max_octaves = 24;
/// Samples per batch: every op runs over a whole batch before the next op starts.
inline constexpr std::uint32_t max_batch = 256;

/// What a position input of a graph is (docs/research/05-procgen-graph.md §5).
enum class PositionKind : std::uint8_t {
    p_planet,  ///< Unscaled body-fixed position, in planet_m: the same continents at every scale
    p_local,   ///< Realized body-fixed position, in meters (m): the same rocks at every scale
};

/// A parsed and validated field graph. Cheap to copy (shared, immutable).
class FieldGraph {
public:
    struct Impl;

    [[nodiscard]] const std::string& id() const;
    /// Hash of the graph's canonical JSON: the cache key of everything compiled from it.
    [[nodiscard]] std::uint64_t hash() const;
    [[nodiscard]] std::size_t node_count() const;
    /// Output names, sorted: the order of a program's outputs.
    [[nodiscard]] std::span<const std::string> outputs() const;

    [[nodiscard]] const Impl& impl() const { return *impl_; }
    explicit FieldGraph(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

private:
    std::shared_ptr<const Impl> impl_;
};

/// Parses and validates a graph. Errors name the node and the fix: procgen.graph_invalid, data.unknown_field (with
/// the field), data.parse_failed. `source_name` prefixes messages (a file name).
[[nodiscard]] Result<FieldGraph> parse_field_graph(std::string_view json, std::string_view source_name = {});

/// What a body supplies at compile time (docs/adr/0052-procgen-runtime.md: a node's seed is
/// hash(body_seed, hash_id(node_id))).
struct BodyConstants {
    std::uint64_t seed = 0;
    /// The body's radius in planet_m, for the cube-face domain (p_planet = direction * radius). Unused on lattices.
    double radius_planet_m = 0;
    /// Planet meters per realized meter: p_planet = p_local * planet_m_per_m. The toolbox sets 1 / s_p
    /// (docs/adr/0049-scale-model.md); a game without a scale model leaves 1.
    double planet_m_per_m = 1.0;
};

struct FieldOutput {
    std::string name;
    std::uint32_t reg = 0;  ///< Register holding it at the end of the op list
};

struct FieldProgram {
    std::vector<std::uint32_t> words;  ///< The op list
    std::uint32_t registers = 0;       ///< Registers used
    std::vector<FieldOutput> outputs;  ///< In FieldGraph::outputs() order
    std::uint32_t noise_octaves = 0;   ///< Gradient-noise evaluations per sample (cost)
    std::uint64_t graph_hash = 0;
};

/// Compiles a graph for one body: folds constants, derives seeds, allocates registers. Fails with
/// procgen.graph_invalid if the graph needs more than max_registers live values.
[[nodiscard]] Result<FieldProgram> compile(const FieldGraph& graph, const BodyConstants& body);

// ---- Domains --------------------------------------------------------------------------------------------------------

/// A body-fixed lattice: point (i, j, k) is at origin_m + (i, j, k) * spacing_m, realized meters (p_local).
struct Lattice {
    double spacing_m = 0.5;
    std::array<double, 3> origin_m{};
};

using LatticePoint = std::array<std::int64_t, 3>;

/// A box of lattice points: first + step * (x, y, z) for x < count[0], y < count[1], z < count[2], x fastest. The step
/// only picks which points are evaluated; no node ever sees it.
struct LatticeBox {
    LatticePoint first{};
    std::int64_t step = 1;
    std::array<std::uint32_t, 3> count{};

    [[nodiscard]] std::size_t size() const {
        return std::size_t{count[0]} * std::size_t{count[1]} * std::size_t{count[2]};
    }
    [[nodiscard]] LatticePoint point(std::size_t index) const;
};

/// A square region of a cube face: sample (i, j) of `resolution`^2 lies at center + u * right + v * up with
/// u = (i + 0.5) * 2 / resolution - 1 and v = 1 - (j + 0.5) * 2 / resolution, projected onto the unit sphere. Rows top
/// to bottom.
struct FaceRegion {
    std::array<float, 3> center{0.0f, 0.0f, 1.0f};
    std::array<float, 3> right{1.0f, 0.0f, 0.0f};
    std::array<float, 3> up{0.0f, 1.0f, 0.0f};
    std::uint32_t resolution = 256;

    [[nodiscard]] std::size_t size() const { return std::size_t{resolution} * resolution; }
};

struct EvalOptions {
    /// Samples per batch, 1 to max_batch. Never changes a result: batches are bit-equal to single samples.
    std::uint32_t batch = max_batch;
};

/// Samples per job of the parallel evaluation: fixed, so the cut never depends on the thread count.
inline constexpr std::size_t parallel_grain = 4096;

// Every evaluate() writes `out` planar: output o of sample i at out[o * samples + i]; out.size() must be
// program.outputs.size() * samples.

void evaluate(const FieldProgram& program, const Lattice& lattice, std::span<const LatticePoint> points,
              std::span<float> out, EvalOptions options = {});
void evaluate(const FieldProgram& program, const Lattice& lattice, const LatticeBox& box, std::span<float> out,
              EvalOptions options = {});
/// The box on veach::JobSystem in chunks of parallel_grain samples; the same bits as on one thread.
void evaluate(JobSystem& jobs, const FieldProgram& program, const Lattice& lattice, const LatticeBox& box,
              std::span<float> out);
void evaluate(const FieldProgram& program, const FaceRegion& region, std::span<float> out, EvalOptions options = {});
/// The cube-face domain on any unit directions (the same values as a FaceRegion sample at the same direction).
void evaluate(const FieldProgram& program, std::span<const std::array<float, 3>> unit_directions, std::span<float> out,
              EvalOptions options = {});

/// One octave of veach:fbm@1's gradient noise at (x, y, z) * frequency, roughly in [-1, 1] (cube-face domain).
[[nodiscard]] float gradient_noise(float x, float y, float z, float frequency, std::uint32_t seed);

}  // namespace veach::procgen
