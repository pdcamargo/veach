// veach: canonical
// Terrain density from a procgen field program (density.hpp). Canonical (ADR 0043): procgen's interpreter, then
// + - * / and sqrt in double in a fixed order, and the integer encoding of voxel.hpp.

#include <veach/core/profile.hpp>
#include <veach/voxel/density.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <vector>

namespace veach::voxel {

namespace {

const procgen::Lattice lod0_lattice{.spacing_m = lod0_cell_meters, .origin_m = {0.0, 0.0, 0.0}};
/// Box bounds run on a lattice 256 times finer than LOD0, so split boxes keep exact integer centers.
constexpr std::int64_t fine = 256;
const procgen::Lattice fine_lattice{.spacing_m = lod0_cell_meters / fine, .origin_m = {0.0, 0.0, 0.0}};

std::uint8_t material_of(float v) {
    return static_cast<std::uint8_t>(std::clamp(std::floor(v + 0.5f), 0.0f, 255.0f));
}

std::int64_t floor_half(std::int64_t v) {
    return v >= 0 ? v / 2 : -((-v + 1) / 2);
}

}  // namespace

Result<std::shared_ptr<const ProgramDensity>> ProgramDensity::make(procgen::FieldProgram program,
                                                                   ProgramDensitySettings settings) {
    const auto find = [&](const std::string& name) -> std::ptrdiff_t {
        for (std::size_t i = 0; i < program.outputs.size(); ++i) {
            if (program.outputs[i].name == name) {
                return static_cast<std::ptrdiff_t>(i);
            }
        }
        return -1;
    };
    const auto outputs = [&] {
        std::string list;
        for (const procgen::FieldOutput& o : program.outputs) {
            list += (list.empty() ? "" : ", ") + o.name;
        }
        return list.empty() ? std::string("none") : list;
    };
    const std::ptrdiff_t density = find(settings.density_output);
    if (density < 0) {
        return make_error(
            ErrorCode::voxel_density_invalid,
            std::format("the field program has no output '{}'", settings.density_output),
            std::format("Set ProgramDensitySettings::density_output to one of its outputs ({}).", outputs()));
    }
    std::ptrdiff_t material = -1;
    if (!settings.material_output.empty()) {
        material = find(settings.material_output);
        if (material < 0) {
            return make_error(
                ErrorCode::voxel_density_invalid,
                std::format("the field program has no output '{}'", settings.material_output),
                std::format("Set material_output to one of its outputs ({}), or leave it empty.", outputs()));
        }
    }
    if (settings.shape == BaseShape::sphere && !(settings.radius_m > 0.0)) {
        return make_error(ErrorCode::voxel_density_invalid,
                          std::format("a sphere's radius must be positive, got {} m", settings.radius_m),
                          "Set ProgramDensitySettings::radius_m, in meters.");
    }
    if (settings.classify_depth < 0 || settings.classify_depth > 7) {
        return make_error(ErrorCode::voxel_density_invalid,
                          std::format("classify_depth is {}; it must be 0 to 7", settings.classify_depth),
                          "6 is the default: half-cell pieces at the box's level.");
    }
    return std::make_shared<const ProgramDensity>(std::move(program), std::move(settings),
                                                  static_cast<std::size_t>(density), material);
}

ProgramDensity::ProgramDensity(procgen::FieldProgram program, ProgramDensitySettings settings,
                               std::size_t density_index, std::ptrdiff_t material_index)
    : program_(std::move(program)),
      settings_(std::move(settings)),
      density_index_(density_index),
      material_index_(material_index) {
    bound_ = procgen::lattice_bounds(program_)[density_index_];
    const double scale = std::fabs(settings_.density_scale);
    const double extent = std::max(std::fabs(settings_.radius_m), std::fabs(settings_.height_m));
    // A quantum (a code is negative only below -1/2048 m), the interpreter's error twice (center and point), and the
    // shape's double rounding.
    margin_ = distance_quantum + 2.0 * scale * bound_.error + 1.0e-12 * extent;
}

double ProgramDensity::shape_at(double x, double y, double z) const {
    switch (settings_.shape) {
        case BaseShape::none: return 0.0;
        case BaseShape::plane: return y - settings_.height_m;
        case BaseShape::sphere: return std::sqrt(x * x + y * y + z * z) - settings_.radius_m;
    }
    return 0.0;
}

ProgramDensity::Range ProgramDensity::shape_range(const std::array<double, 3>& lo,
                                                  const std::array<double, 3>& hi) const {
    switch (settings_.shape) {
        case BaseShape::none: return {0.0, 0.0};
        case BaseShape::plane: return {lo[1] - settings_.height_m, hi[1] - settings_.height_m};
        case BaseShape::sphere: {
            double near2 = 0.0, far2 = 0.0;
            for (std::size_t a = 0; a < 3; ++a) {
                const double n = lo[a] > 0.0 ? lo[a] : (hi[a] < 0.0 ? -hi[a] : 0.0);
                const double f = std::max(std::fabs(lo[a]), std::fabs(hi[a]));
                near2 += n * n;
                far2 += f * f;
            }
            return {std::sqrt(near2) - settings_.radius_m, std::sqrt(far2) - settings_.radius_m};
        }
    }
    return {0.0, 0.0};
}

double ProgramDensity::max_extent() const {
    const double s = settings_.density_scale;
    const double raise = std::max(-s * bound_.lo, -s * bound_.hi) + margin_;  // how far d < 0 reaches past the shape
    switch (settings_.shape) {
        case BaseShape::none: return std::numeric_limits<double>::infinity();
        case BaseShape::plane: return settings_.height_m + std::max(raise, 0.0);
        case BaseShape::sphere: return settings_.radius_m + std::max(raise, 0.0);
    }
    return 0.0;
}

double ProgramDensity::distance(double x, double y, double z) const {
    const procgen::Lattice at{.spacing_m = 1.0, .origin_m = {x, y, z}};
    const procgen::LatticePoint origin{0, 0, 0};
    std::vector<float> values(program_.outputs.size());
    procgen::evaluate(program_, at, std::span(&origin, 1), values);
    return shape_at(x, y, z) + settings_.density_scale * static_cast<double>(values[density_index_]);
}

void ProgramDensity::sample(const ChunkKey& key, std::uint8_t face_mask, ChunkSamples& out) const {
    VEACH_ZONE("voxel: sample chunk");
    constexpr int n = ChunkSamples::grid_n;
    constexpr int fn = ChunkSamples::face_n;
    thread_local std::vector<float> values;
    out.key = key;
    out.face_mask = face_mask;
    const auto o = chunk_origin(key);
    const std::int64_t s = cell_units(key.level);
    const std::size_t outputs = program_.outputs.size();
    const double scale = settings_.density_scale;
    bool neg = false, pos = false;

    // One sample from planar outputs: index i of `count`, at lattice point p.
    const auto make = [&](std::size_t i, std::size_t count, std::int64_t px, std::int64_t py, std::int64_t pz) {
        const double x = static_cast<double>(px) * lod0_cell_meters;
        const double y = static_cast<double>(py) * lod0_cell_meters;
        const double z = static_cast<double>(pz) * lod0_cell_meters;
        const double d = shape_at(x, y, z) + scale * static_cast<double>(values[density_index_ * count + i]);
        const std::uint8_t m = material_index_ >= 0
                                   ? material_of(values[static_cast<std::size_t>(material_index_) * count + i])
                                   : settings_.material;
        return Sample{encode_distance(d, settings_.encoding), m};
    };

    const procgen::LatticeBox grid{.first = {o[0] - s, o[1] - s, o[2] - s}, .step = s, .count = {n, n, n}};
    const std::size_t count = grid.size();
    values.resize(outputs * count);
    procgen::evaluate(program_, lod0_lattice, grid, values);
    out.grid.resize(count);
    std::size_t i = 0;
    for (int z = -1; z <= chunk_cells + 1; ++z) {
        for (int y = -1; y <= chunk_cells + 1; ++y) {
            for (int x = -1; x <= chunk_cells + 1; ++x, ++i) {
                const Sample v = make(i, count, o[0] + x * s, o[1] + y * s, o[2] + z * s);
                out.grid[i] = v;
                (v.distance < 0 ? neg : pos) = true;
            }
        }
    }

    for (int f = 0; f < 6; ++f) {
        auto& face = out.faces[static_cast<std::size_t>(f)];
        if (!(face_mask & (1u << f)) || key.level == 0) {
            face.clear();
            continue;
        }
        const auto a = static_cast<std::size_t>(f / 2);
        const auto b = (a + 1) % 3, c = (a + 2) % 3;
        const std::int64_t h = s / 2;
        const std::int64_t plane = o[a] + ((f & 1) ? chunk_units(key.level) : 0);
        procgen::LatticeBox box{.step = h};
        box.first[a] = plane - h;
        box.count[a] = 3;
        box.first[b] = o[b] - h;
        box.count[b] = fn;
        box.first[c] = o[c] - h;
        box.count[c] = fn;
        const std::size_t face_count = box.size();
        values.resize(outputs * face_count);
        procgen::evaluate(program_, lod0_lattice, box, values);
        face.resize(face_count);
        std::size_t j = 0;
        std::array<std::uint32_t, 3> at{};
        for (at[2] = 0; at[2] < box.count[2]; ++at[2]) {
            for (at[1] = 0; at[1] < box.count[1]; ++at[1]) {
                for (at[0] = 0; at[0] < box.count[0]; ++at[0], ++j) {
                    const Sample v = make(j, face_count, box.first[0] + at[0] * h, box.first[1] + at[1] * h,
                                          box.first[2] + at[2] * h);
                    const std::size_t u = at[b], w = at[c], layer = at[a];
                    face[u + static_cast<std::size_t>(fn) * (w + static_cast<std::size_t>(fn) * layer)] = v;
                    if (layer == 1) {
                        (v.distance < 0 ? neg : pos) = true;
                    }
                }
            }
        }
    }
    out.has_surface = neg && pos;
}

namespace {

/// The center of a box in fine units (rounded down) and its radius to the farthest corner.
std::array<std::int64_t, 3> center_of(const std::array<std::int64_t, 3>& lo, const std::array<std::int64_t, 3>& hi) {
    return {floor_half(lo[0] + hi[0]), floor_half(lo[1] + hi[1]), floor_half(lo[2] + hi[2])};
}

}  // namespace

double ProgramDensity::center_distance(const std::array<std::int64_t, 3>& c, float value) const {
    constexpr double unit = lod0_cell_meters / static_cast<double>(fine);
    return shape_at(static_cast<double>(c[0]) * unit, static_cast<double>(c[1]) * unit,
                    static_cast<double>(c[2]) * unit) +
           settings_.density_scale * static_cast<double>(value);
}

BoxClass ProgramDensity::classify(const LatticeBox& box) const {
    std::array<std::int64_t, 3> lo{}, hi{};
    for (std::size_t a = 0; a < 3; ++a) {
        lo[a] = box.lo[a] * fine;
        hi[a] = box.hi[a] * fine;
    }
    const std::array<std::int64_t, 3> c = center_of(lo, hi);
    const procgen::LatticePoint point{c[0], c[1], c[2]};
    std::vector<float> values(program_.outputs.size());
    procgen::evaluate(program_, fine_lattice, std::span(&point, 1), values);
    return classify_fine(lo, hi, settings_.classify_depth, center_distance(c, values[density_index_]));
}

BoxClass ProgramDensity::classify_fine(const std::array<std::int64_t, 3>& lo, const std::array<std::int64_t, 3>& hi,
                                       int depth, double center) const {
    constexpr double unit = lod0_cell_meters / static_cast<double>(fine);
    std::array<double, 3> mlo{}, mhi{};
    for (std::size_t a = 0; a < 3; ++a) {
        mlo[a] = static_cast<double>(lo[a]) * unit;
        mhi[a] = static_cast<double>(hi[a]) * unit;
    }
    // The global range: the shape over the box plus the output's whole range.
    const Range shape = shape_range(mlo, mhi);
    const double s = settings_.density_scale;
    double dlo = shape.lo + std::min(s * bound_.lo, s * bound_.hi) - margin_;
    double dhi = shape.hi + std::max(s * bound_.lo, s * bound_.hi) + margin_;
    // The center's value plus how far d can move within the half diagonal: the shape is 1-Lipschitz, the program
    // moves by at most its variation (procgen::lattice_variation: fine octaves count by their range, not slope).
    const std::array<std::int64_t, 3> c = center_of(lo, hi);
    double r2 = 0.0;
    for (std::size_t a = 0; a < 3; ++a) {
        const double r = static_cast<double>(std::max(c[a] - lo[a], hi[a] - c[a])) * unit;
        r2 += r * r;
    }
    const double radius = std::sqrt(r2);
    const double reach = (settings_.shape == BaseShape::none ? 0.0 : radius) +
                         std::fabs(s) * procgen::lattice_variation(program_, radius)[density_index_];
    dlo = std::max(dlo, center - reach - margin_);
    dhi = std::min(dhi, center + reach + margin_);
    if (dlo > 0.0) {
        return BoxClass::empty;
    }
    if (dhi < 0.0) {
        return BoxClass::solid;
    }
    if (depth == 0) {
        return BoxClass::surface;
    }
    // Split in eight; the children's centers in one evaluation.
    std::array<std::array<std::int64_t, 3>, 8> clo{}, chi{};
    std::array<procgen::LatticePoint, 8> centers{};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t a = 0; a < 3; ++a) {
            const bool upper = ((i >> a) & 1) != 0;
            clo[i][a] = upper ? c[a] : lo[a];
            chi[i][a] = upper ? hi[a] : c[a];
        }
        const auto cc = center_of(clo[i], chi[i]);
        centers[i] = {cc[0], cc[1], cc[2]};
    }
    std::vector<float> values(program_.outputs.size() * 8);
    procgen::evaluate(program_, fine_lattice, centers, values);
    bool empty = false, solid = false;
    for (std::size_t i = 0; i < 8; ++i) {
        const double dc =
            center_distance({centers[i][0], centers[i][1], centers[i][2]}, values[density_index_ * 8 + i]);
        const BoxClass cc = classify_fine(clo[i], chi[i], depth - 1, dc);
        if (cc == BoxClass::surface) {
            return cc;
        }
        (cc == BoxClass::empty ? empty : solid) = true;
        if (empty && solid) {
            return BoxClass::surface;
        }
    }
    return empty ? BoxClass::empty : BoxClass::solid;
}

}  // namespace veach::voxel
