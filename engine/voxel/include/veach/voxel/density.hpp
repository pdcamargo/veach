#pragma once

// Where a terrain's samples come from (docs/adr/0053-terrain-density-field.md, docs/specs/voxel-terrain/spec.md).
//
// A DensitySource fills a chunk's generated samples and proves, for a box of the lattice, that it holds no surface
// (all empty or all solid) when it can, so streaming never samples those chunks. Every sample is a pure function of
// its LOD0 lattice point (docs/adr/0074-amendments-from-sp-08.md rule 1), never of the chunk or level asking.
//
// ProgramDensity is the engine's source: a procgen field program evaluated on the LOD0 lattice, one chunk per call
// (docs/adr/0084-amendments-from-building-procgen.md rule 4), added to a base shape:
//
//   d(p) = shape(p) + density_scale * program_output(p)        meters, negative solid
//
// with shape a sphere (|p| - radius: a planet or an asteroid), a plane (p.y - height: a flat world) or nothing (the
// program is the whole density). Its box bounds come from procgen::lattice_bounds: one evaluation at the box center
// plus how far the field can move within the box (procgen::lattice_variation), split into eight up to
// `classify_depth` times.
//
// Canonical (docs/adr/0043-determinism-tiers-and-strict-floating-point.md): the same samples and the same box classes
// on every platform.

#include <veach/core/error.hpp>
#include <veach/procgen/field_graph.hpp>
#include <veach/voxel/voxel.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace veach::voxel {

/// What a box of the lattice holds, as far as a source can prove.
enum class BoxClass : std::uint8_t { empty, solid, surface };

/// A terrain's generated samples. Thread-safe: streaming calls it from many jobs at once.
class DensitySource {
public:
    virtual ~DensitySource() = default;
    /// Fills `out` for `key` (grid, apron and the full-resolution faces in `face_mask`, as sample_chunk does) with
    /// generated samples, no edits. Sets out.has_surface.
    virtual void sample(const ChunkKey& key, std::uint8_t face_mask, ChunkSamples& out) const = 0;
    /// Conservative class of every sample on the lattice points of `box`: `empty` or `solid` only when proven.
    [[nodiscard]] virtual BoxClass classify(const LatticeBox& box) const = 0;
    /// The same with little or no subdivision, for LOD0 collider chunks: they sit within a few meters of the surface,
    /// where a bound rarely proves anything and a deep split costs more than sampling the chunk (VCH-115).
    [[nodiscard]] virtual BoxClass classify_shallow(const LatticeBox& box) const { return classify(box); }
    /// How the source encodes distances (the mesher decodes with the same).
    [[nodiscard]] virtual DistanceEncoding encoding() const = 0;
    /// The generated (unedited) signed distance at a body-local point in meters, if the source can tell: terrain
    /// materials show what a dig uncovers by how deep below the generated surface a wall is.
    [[nodiscard]] virtual std::optional<double> generated_distance(double /*x*/, double /*y*/, double /*z*/) const {
        return std::nullopt;
    }
};

/// A DensityField (one sample at a time, C++) as a source. With no classifier every box may hold a surface.
class FieldSource final : public DensitySource {
public:
    using Classifier = BoxClass (*)(const DensityField& field, const LatticeBox& box);
    explicit FieldSource(const DensityField& field, DistanceEncoding encoding = DistanceEncoding::companded,
                         Classifier classifier = nullptr)
        : field_(field), encoding_(encoding), classifier_(classifier) {}

    void sample(const ChunkKey& key, std::uint8_t face_mask, ChunkSamples& out) const override {
        sample_chunk(field_, key, face_mask, out);
    }
    [[nodiscard]] BoxClass classify(const LatticeBox& box) const override {
        return classifier_ != nullptr ? classifier_(field_, box) : BoxClass::surface;
    }
    [[nodiscard]] DistanceEncoding encoding() const override { return encoding_; }

private:
    const DensityField& field_;
    DistanceEncoding encoding_;
    Classifier classifier_;
};

enum class BaseShape : std::uint8_t {
    none,    ///< d = density_scale * output
    plane,   ///< d = p.y - height_m + ...: a flat world, +Y up
    sphere,  ///< d = |p| - radius_m + ...: a body centered on the lattice origin
};

struct ProgramDensitySettings {
    BaseShape shape = BaseShape::sphere;
    double radius_m = 0.0;  ///< sphere
    double height_m = 0.0;  ///< plane
    /// The program output added to the shape, and its factor: -3000 makes a [0, 1] height output 3 km of relief.
    std::string density_output = "density";
    double density_scale = 1.0;
    /// An output rounded to the material id (clamped to 0..255); empty: every sample gets `material`.
    std::string material_output;
    std::uint8_t material = 0;
    DistanceEncoding encoding = DistanceEncoding::companded;
    /// Times a box the one-shot bound can't decide is split in eight (SP-09: 6 leaves 13 % of jobs empty).
    int classify_depth = 6;
};

class ProgramDensity final : public DensitySource {
public:
    /// Fails with voxel.density_invalid if an output is missing or a setting is out of range.
    [[nodiscard]] static Result<std::shared_ptr<const ProgramDensity>> make(procgen::FieldProgram program,
                                                                            ProgramDensitySettings settings);

    void sample(const ChunkKey& key, std::uint8_t face_mask, ChunkSamples& out) const override;
    [[nodiscard]] BoxClass classify(const LatticeBox& box) const override;
    /// One split at most.
    [[nodiscard]] BoxClass classify_shallow(const LatticeBox& box) const override;
    [[nodiscard]] DistanceEncoding encoding() const override { return settings_.encoding; }

    /// The signed distance at a body-local point in meters, unquantized (tools, flight, tests). Canonical.
    [[nodiscard]] double distance(double x, double y, double z) const;
    [[nodiscard]] std::optional<double> generated_distance(double x, double y, double z) const override {
        return distance(x, y, z);
    }
    /// The largest |p| (sphere) or p.y (plane) the surface can reach, from the bounds, meters.
    [[nodiscard]] double max_extent() const;
    [[nodiscard]] const ProgramDensitySettings& settings() const { return settings_; }
    [[nodiscard]] const procgen::FieldProgram& program() const { return program_; }

    ProgramDensity(procgen::FieldProgram program, ProgramDensitySettings settings, std::size_t density_index,
                   std::ptrdiff_t material_index);

private:
    struct Range {
        double lo, hi;
    };
    /// The shape's distance at a point, and its range over a box (meters).
    [[nodiscard]] double shape_at(double x, double y, double z) const;
    [[nodiscard]] Range shape_range(const std::array<double, 3>& lo, const std::array<double, 3>& hi) const;
    /// Class of a box given in 1/256 LOD0 units, split up to `depth` times.
    [[nodiscard]] BoxClass classify_to(const LatticeBox& box, int depth) const;
    [[nodiscard]] BoxClass classify_fine(const std::array<std::int64_t, 3>& lo, const std::array<std::int64_t, 3>& hi,
                                         int depth, double center) const;
    /// d at a fine-lattice point from the program's value there.
    [[nodiscard]] double center_distance(const std::array<std::int64_t, 3>& c, float value) const;

    procgen::FieldProgram program_;
    ProgramDensitySettings settings_;
    std::size_t density_index_ = 0;
    std::ptrdiff_t material_index_ = -1;
    procgen::FieldBound bound_;
    double margin_ = 0.0;  ///< meters: float error plus a quantum
};

}  // namespace veach::voxel
