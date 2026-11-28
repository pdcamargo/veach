// Spike SP-09 (VCH-085), THROWAWAY: the descent, the chunk octree and the streaming (sp09_spike.hpp).

#include "sp09_spike.hpp"

#include "sp09_terrain.hpp"

#include <veach/core/env.hpp>
#include <veach/core/jobs.hpp>
#include <veach/render/mesh_store.hpp>
#include <veach/render/renderer.hpp>
#include <veach/voxel/voxel.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lab {

namespace {

namespace vx = veach::voxel;
namespace math = veach::math;
using sp09::BoxClass;

constexpr double pi = 3.141592653589793238462643383279502884;

double wall_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 31;
    x *= 0x7fb5d329728ea185ULL;
    x ^= x >> 27;
    x *= 0x81dadef4bc2dd44dULL;
    x ^= x >> 33;
    return x;
}

struct KeyHash {
    std::size_t operator()(const vx::ChunkKey& k) const {
        std::uint64_t h = mix(std::uint64_t{k.level} + 0x9e3779b97f4a7c15ULL);
        h = mix(h ^ static_cast<std::uint32_t>(k.x));
        h = mix(h ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.y)) << 21));
        h = mix(h ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.z)) << 42));
        return static_cast<std::size_t>(h);
    }
};
using KeySet = std::unordered_set<vx::ChunkKey, KeyHash>;

/// A mesh is valid for one chunk and one set of finer faces (Transvoxel's transition cells, ADR 0074 decision 2).
struct EntryKey {
    vx::ChunkKey key;
    std::uint8_t finer = 0;
    bool operator==(const EntryKey&) const = default;
    auto operator<=>(const EntryKey&) const = default;
};
struct EntryHash {
    std::size_t operator()(const EntryKey& e) const { return KeyHash{}(e.key) ^ (std::size_t{e.finer} * 0x9e37U); }
};

// ---------------------------------------------------------------------------------------------------------------
// The octree: distance-based (ratio lambda), pruned to boxes that may hold the surface, restricted so touching
// leaves (faces, edges, corners) differ by at most one level.

class Selector {
public:
    Selector(const sp09::TerrainField& field, int root_level) : field_(field), root_level_(root_level) {}

    int root_level() const { return root_level_; }

    std::vector<EntryKey> select(const std::array<double, 3>& focus, double lambda) {
        interior_.clear();
        std::vector<vx::ChunkKey> leaves;
        for (std::int32_t z = -1; z <= 0; ++z) {
            for (std::int32_t y = -1; y <= 0; ++y) {
                for (std::int32_t x = -1; x <= 0; ++x) {
                    refine(focus, lambda, {static_cast<std::uint8_t>(root_level_), x, y, z}, leaves);
                }
            }
        }
        // Restrict: a leaf touching a split node one level finer than itself touches leaves two levels finer.
        for (bool changed = true; changed;) {
            changed = false;
            std::vector<vx::ChunkKey> next;
            next.reserve(leaves.size());
            for (const vx::ChunkKey& k : leaves) {
                if (k.level >= 2 && touches_split_finer(k)) {
                    interior_.insert(k);
                    for (int i = 0; i < 8; ++i) {
                        const vx::ChunkKey c = child(k, i);
                        if (classify(c) == BoxClass::surface) {
                            next.push_back(c);
                        }
                    }
                    changed = true;
                } else {
                    next.push_back(k);
                }
            }
            leaves = std::move(next);
        }
        std::vector<EntryKey> out;
        out.reserve(leaves.size());
        for (const vx::ChunkKey& k : leaves) {
            std::uint8_t finer = 0;
            if (k.level > 0) {
                for (int f = 0; f < 6; ++f) {
                    vx::ChunkKey n = k;
                    const int step = (f & 1) ? 1 : -1;
                    (f / 2 == 0 ? n.x : (f / 2 == 1 ? n.y : n.z)) += step;
                    if (interior_.contains(n)) {
                        finer = static_cast<std::uint8_t>(finer | (1U << f));
                    }
                }
            }
            out.push_back({k, finer});
        }
        std::ranges::sort(out);
        return out;
    }

    BoxClass classify(const vx::ChunkKey& k) {
        if (const auto it = classes_.find(k); it != classes_.end()) {
            return it->second;
        }
        const auto o = vx::chunk_origin(k);
        const double e = static_cast<double>(vx::chunk_units(k.level)) * vx::lod0_cell_meters;
        double lo[3], hi[3];
        for (std::size_t a = 0; a < 3; ++a) {
            lo[a] = static_cast<double>(o[a]) * vx::lod0_cell_meters;
            hi[a] = lo[a] + e;
        }
        const BoxClass c = classify_box(lo, hi, refine_depth);
        classes_.emplace(k, c);
        return c;
    }

    /// Box bounds subdivided: a box the field's one-shot bound calls "surface" is split in 8, up to `depth` times,
    /// and is a surface only if some piece still is (or empty and solid pieces meet). Depth 3 tests pieces of 4 cells.
    static constexpr int refine_depth = 6;
    BoxClass classify_box(const double lo[3], const double hi[3], int depth) const {
        const BoxClass c = field_.classify(lo, hi);
        if (c != BoxClass::surface || depth == 0) {
            return c;
        }
        bool empty = false, solid = false;
        for (int i = 0; i < 8; ++i) {
            double clo[3], chi[3];
            for (int a = 0; a < 3; ++a) {
                const double mid = 0.5 * (lo[a] + hi[a]);
                const bool upper = ((i >> a) & 1) != 0;
                clo[a] = upper ? mid : lo[a];
                chi[a] = upper ? hi[a] : mid;
            }
            const BoxClass cc = classify_box(clo, chi, depth - 1);
            if (cc == BoxClass::surface) {
                return cc;
            }
            (cc == BoxClass::empty ? empty : solid) = true;
        }
        return empty && solid ? BoxClass::surface : (empty ? BoxClass::empty : BoxClass::solid);
    }

private:
    static vx::ChunkKey child(const vx::ChunkKey& k, int i) {
        return {static_cast<std::uint8_t>(k.level - 1), k.x * 2 + (i & 1), k.y * 2 + ((i >> 1) & 1),
                k.z * 2 + ((i >> 2) & 1)};
    }

    static double distance_sq(const std::array<double, 3>& p, const vx::ChunkKey& k) {
        const auto o = vx::chunk_origin(k);
        const auto e = static_cast<double>(vx::chunk_units(k.level));
        double d2 = 0.0;
        for (std::size_t a = 0; a < 3; ++a) {
            const auto lo = static_cast<double>(o[a]);
            const double hi = lo + e;
            const double d = p[a] < lo ? lo - p[a] : (p[a] > hi ? p[a] - hi : 0.0);
            d2 += d * d;
        }
        return d2;
    }

    void refine(const std::array<double, 3>& focus, double lambda, const vx::ChunkKey& k,
                std::vector<vx::ChunkKey>& leaves) {
        if (classify(k) != BoxClass::surface) {
            return;
        }
        if (k.level > 0) {
            const double reach = lambda * static_cast<double>(vx::chunk_units(k.level - 1));
            if (distance_sq(focus, k) < reach * reach) {
                interior_.insert(k);
                for (int i = 0; i < 8; ++i) {
                    refine(focus, lambda, child(k, i), leaves);
                }
                return;
            }
        }
        leaves.push_back(k);
    }

    bool touches_split_finer(const vx::ChunkKey& k) const {
        const auto level = static_cast<std::uint8_t>(k.level - 1);
        for (std::int32_t dz = -1; dz <= 2; ++dz) {
            for (std::int32_t dy = -1; dy <= 2; ++dy) {
                for (std::int32_t dx = -1; dx <= 2; ++dx) {
                    if (dx >= 0 && dx <= 1 && dy >= 0 && dy <= 1 && dz >= 0 && dz <= 1) {
                        continue;  // k's own children
                    }
                    if (interior_.contains({level, 2 * k.x + dx, 2 * k.y + dy, 2 * k.z + dz})) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    const sp09::TerrainField& field_;
    int root_level_;
    KeySet interior_;
    std::unordered_map<vx::ChunkKey, BoxClass, KeyHash> classes_;
};

// ---------------------------------------------------------------------------------------------------------------

struct ChunkEntry {
    veach::AssetId mesh;  ///< invalid when the chunk has no triangles
    std::vector<std::array<std::int64_t, 3>> fixed;
    std::vector<std::uint32_t> indices;
    std::uint32_t triangles = 0;
};

struct Job {
    EntryKey entry;
    ChunkEntry result;
    double sample_ms = 0.0;
    double mesh_ms = 0.0;
    double build_ms = 0.0;
    bool sampled_surface = false;
    bool store = true;  ///< false: a benchmark job, not added to the MeshStore
};

/// Per-frame numbers, one CSV row each.
struct FrameStats {
    std::uint64_t frame = 0;
    std::uint32_t phase = 0;
    double time = 0.0;
    double altitude = 0.0;
    double speed = 0.0;
    std::uint32_t target = 0;
    std::uint32_t drawn = 0;
    std::uint32_t cached = 0;
    std::uint32_t requests = 0;  ///< target entries new since last frame's target
    std::uint32_t remeshes = 0;  ///< of those, chunks already in last frame's target with other finer faces
    std::uint32_t meshed = 0;    ///< jobs run this frame
    std::uint32_t empty = 0;     ///< of those, no triangles
    std::uint32_t backlog = 0;   ///< target entries still missing after this frame's jobs
    std::uint32_t stale = 0;     ///< frames since the drawn selection last matched the target
    double select_ms = 0.0;
    double stream_ms = 0.0;
    double frame_ms = 0.0;   ///< wall time since the previous frame's stream step
    double sample_ms = 0.0;  ///< summed over jobs (CPU time)
    double mesh_ms = 0.0;
    double build_ms = 0.0;
    std::uint64_t triangles = 0;
    double lambda = 0.0;  ///< the effective LOD ratio this frame
};

const std::array<math::vec3, 20> level_palette{{
    {0.85f, 0.15f, 0.12f}, {0.95f, 0.55f, 0.10f}, {0.30f, 0.70f, 0.20f}, {0.15f, 0.35f, 0.85f}, {0.70f, 0.25f, 0.80f},
    {0.95f, 0.85f, 0.20f}, {0.20f, 0.75f, 0.75f}, {0.90f, 0.45f, 0.60f}, {0.55f, 0.40f, 0.25f}, {0.60f, 0.60f, 0.60f},
    {0.85f, 0.15f, 0.12f}, {0.95f, 0.55f, 0.10f}, {0.30f, 0.70f, 0.20f}, {0.15f, 0.35f, 0.85f}, {0.70f, 0.25f, 0.80f},
    {0.95f, 0.85f, 0.20f}, {0.20f, 0.75f, 0.75f}, {0.90f, 0.45f, 0.60f}, {0.55f, 0.40f, 0.25f}, {0.60f, 0.60f, 0.60f},
}};

struct Checkpoint {
    std::string label;
    double altitude = 0.0;  ///< taken when the descent crosses it; 0 = taken by a phase change
    bool done = false;
};

struct WeldReport {
    std::uint64_t edges = 0;
    std::uint64_t open = 0;
    std::uint64_t non_manifold = 0;
};

class Spike {
public:
    Spike() = default;

    bool initialized() const { return field_ != nullptr; }

    void initialize(veach::Entity camera, Sp09Descent& d) {
        apply_environment(d);
        camera.set<Sp09Descent>(d);
        config_ = d;
        lambda_ = d.lambda;
        adaptive_ = veach::env("VEACH_SP09_ADAPTIVE") == "1";
        if (d.body == 1) {
            field_owner_ = std::make_unique<sp09::AsteroidField>(sp09::AsteroidSettings{
                .encoding = d.linear_encoding ? sp09::Encoding::linear : sp09::Encoding::companded});
            field_ = field_owner_.get();
        } else {
            planet_ = std::make_unique<sp09::PlanetField>(sp09::PlanetSettings{
                .scale = d.scale,
                .relief_exponent = d.relief_exponent,
                .encoding = d.linear_encoding ? sp09::Encoding::linear : sp09::Encoding::companded});
            field_ = planet_.get();
        }
        // Octree roots: 2 x 2 x 2 chunks of the smallest level that holds the body.
        const double reach = field_->max_radius() * 1.01 + 16.0;
        int root = 0;
        while (static_cast<double>(vx::chunk_units(root)) * vx::lod0_cell_meters < reach) {
            ++root;
        }
        selector_ = std::make_unique<Selector>(*field_, root);

        choose_site();
        const double surface = field_->radius() + field_->surface_height(up_[0], up_[1], up_[2]);
        double start = d.start_altitude;
        if (start <= 0.0) {
            start = d.body == 1 ? 5000.0 : std::max(field_->radius(), 200'000.0);
        }
        for (std::size_t a = 0; a < 3; ++a) {
            position_[a] = up_[a] * (surface + start);
        }

        out_dir_ = veach::env("VEACH_SP09_OUT").value_or("");
        prefix_ = veach::env("VEACH_SP09_PREFIX").value_or("sp09");
        shots_ = veach::env("VEACH_SP09_SHOTS").value_or("1") != "0" && !out_dir_.empty();
        bench_repeats_ =
            static_cast<int>(std::strtol(veach::env("VEACH_SP09_BENCH").value_or("0").c_str(), nullptr, 10));
        if (veach::env("VEACH_SP09_SKY") == "magenta") {
            // Hole check (as SP-08): a bright magenta sky shows through any gap; level colors absorb it in shade.
            if (veach::Entity sky = camera.world().lookup("sky"); sky.is_valid()) {
                sky.set<veach::Sky>({.mode = veach::SkyMode::constant, .color = {3000.0f, 0.0f, 3000.0f}});
            }
        }
        if (!out_dir_.empty()) {
            std::filesystem::create_directories(out_dir_);
            csv_.open(std::filesystem::path(out_dir_) / (prefix_ + "_frames.csv"));
            csv_ << "frame,phase,time,altitude,speed,target,drawn,cached,requests,remeshes,meshed,empty,backlog,"
                    "stale,select_ms,stream_ms,frame_ms,sample_ms,mesh_ms,build_ms,triangles,lambda\n";
        }
        checkpoints_.push_back({"orbit", 0.0});
        for (const double a : {100'000.0, 10'000.0, 1'000.0}) {
            if (a < start * 0.9) {
                checkpoints_.push_back({a >= 1000.0 ? std::format("{}km", a / 1000.0) : std::format("{}m", a), a});
            }
        }
        checkpoints_.push_back({"100m", 0.0});
        checkpoints_.push_back({"100m_skim", 0.0});
        checkpoints_.push_back({"10m", 0.0});

        make_materials(camera.world());
        auto& renderer = veach::services(camera.world()).get<veach::render::Renderer>();
        renderer.add_extractor("sp09_chunks",
                               [this](const veach::World&, veach::render::RenderWorld& out) { extract(out); });
        sun(camera.world());
        veach::log::info("sp09",
                         "{} at s = {}: R = {:.1f} m, max radius {:.1f} m, root level {}, relief exponent {}, "
                         "lambda {}, budget {} chunks/frame, {} samples, start altitude {:.0f} m",
                         d.body == 1 ? "asteroid" : "planet", d.scale, field_->radius(), field_->max_radius(), root,
                         d.relief_exponent, d.lambda, d.budget, d.linear_encoding ? "linear" : "companded", start);
    }

    /// Fixed step: the governor.
    void fly(veach::Entity camera, Sp09State& state, double dt) {
        const double r = length(position_);
        const std::array<double, 3> n{position_[0] / r, position_[1] / r, position_[2] / r};
        const double surface = field_->radius() + field_->surface_height(n[0], n[1], n[2]);
        double altitude = r - surface;
        double speed = 0.0;
        const auto governed = [&] {
            return std::max(config_.v_floor, altitude / config_.tau);
        };
        const auto skim = [&](double hold, double v) {
            // Along the great circle of `forward_`, terrain following at `hold` above the ground.
            const double angle = v * dt / r;
            const double c = std::cos(angle), s = std::sin(angle);
            std::array<double, 3> m{};
            for (std::size_t a = 0; a < 3; ++a) {
                m[a] = c * n[a] + s * forward_[a];
                forward_[a] = -s * n[a] + c * forward_[a];
            }
            const double ground = field_->radius() + field_->surface_height(m[0], m[1], m[2]);
            for (std::size_t a = 0; a < 3; ++a) {
                position_[a] = m[a] * (ground + hold);
            }
            speed = v;
        };
        switch (phase_) {
            case 0:  // warm-up: wait for the stream to catch up
                if (caught_up_frames_ >= 2 || warmup_steps_ > 1200) {
                    phase_ = 1;
                    phase_started_ = steps_;
                    descent_start_step_ = steps_;
                }
                ++warmup_steps_;
                break;
            case 1:
            case 3: {
                const double target = phase_ == 1 ? config_.end_altitude : config_.low_altitude;
                speed = governed();
                const double next = std::max(target, altitude - speed * dt);
                speed = (altitude - next) / dt;
                for (std::size_t a = 0; a < 3; ++a) {
                    position_[a] = n[a] * (surface + next);
                }
                altitude = next;
                if (next <= target) {
                    pending_shot_ = phase_ == 1 ? "100m" : "";
                    phase_ = phase_ == 1 ? 2 : 4;
                    phase_started_ = steps_;
                }
                break;
            }
            case 2:
            case 4: {
                const bool low = phase_ == 4;
                skim(low ? config_.low_altitude : config_.end_altitude, config_.skim_speed);
                altitude = low ? config_.low_altitude : config_.end_altitude;
                const double seconds = static_cast<double>(steps_ - phase_started_) * dt;
                if (seconds >= (low ? config_.low_seconds : config_.skim_seconds)) {
                    pending_shot_ = low ? "10m" : "100m_skim";
                    phase_ = low ? 5 : 3;
                    phase_started_ = steps_;
                }
                break;
            }
            default: break;
        }
        if (phase_ >= 1 && phase_ <= 4) {
            descent_time_ = static_cast<double>(steps_ - descent_start_step_) * dt;
        }
        ++steps_;
        altitude_ = altitude;
        speed_ = speed;

        // The camera: at the render origin, looking along forward_, pitched down between the horizon and the nadir.
        const double r2 = length(position_);
        const std::array<double, 3> up{position_[0] / r2, position_[1] / r2, position_[2] / r2};
        const double dip =
            std::acos(std::clamp(field_->radius() / (field_->radius() + std::max(altitude, 0.0)), 0.0, 1.0));
        const double pitch = dip + 0.25 * (0.5 * pi - dip);
        // Re-orthogonalize forward against up.
        double dot = 0.0;
        for (std::size_t a = 0; a < 3; ++a) {
            dot += forward_[a] * up[a];
        }
        std::array<double, 3> f{};
        for (std::size_t a = 0; a < 3; ++a) {
            f[a] = forward_[a] - dot * up[a];
        }
        const double fl = length(f);
        for (std::size_t a = 0; a < 3; ++a) {
            forward_[a] = f[a] / fl;
        }
        const math::vec3 look(static_cast<float>(std::cos(pitch) * forward_[0] - std::sin(pitch) * up[0]),
                              static_cast<float>(std::cos(pitch) * forward_[1] - std::sin(pitch) * up[1]),
                              static_cast<float>(std::cos(pitch) * forward_[2] - std::sin(pitch) * up[2]));
        const math::vec3 up_hint(static_cast<float>(up[0]), static_cast<float>(up[1]), static_cast<float>(up[2]));
        camera.set<veach::Transform>({.translation = math::vec3(0.0f), .rotation = look_rotation(look, up_hint)});

        state = {.x = position_[0],
                 .y = position_[1],
                 .z = position_[2],
                 .altitude = altitude_,
                 .speed = speed_,
                 .phase = phase_,
                 .time = descent_time_,
                 .drawn = static_cast<std::uint32_t>(drawn_.size()),
                 .backlog = backlog_};
    }

    /// Every frame: select, mesh up to the budget, switch the drawn selection when the new one is complete.
    void stream(veach::World world) {
        VEACH_ZONE("sp09 stream");
        const double t0 = wall_ms();
        FrameStats stats;
        stats.frame = ++frames_;
        stats.phase = phase_;
        stats.time = descent_time_;
        stats.altitude = altitude_;
        stats.speed = speed_;
        stats.frame_ms = last_stream_ > 0.0 ? t0 - last_stream_ : 0.0;
        last_stream_ = t0;

        std::array<double, 3> focus{};
        for (std::size_t a = 0; a < 3; ++a) {
            focus[a] = position_[a] / vx::lod0_cell_meters;
        }
        std::vector<EntryKey> target;
        {
            VEACH_ZONE("sp09 select");
            target = selector_->select(focus, lambda_);
        }
        const double t1 = wall_ms();
        stats.select_ms = t1 - t0;
        stats.target = static_cast<std::uint32_t>(target.size());

        // Demand: what the target asks for that last frame's did not.
        std::unordered_set<vx::ChunkKey, KeyHash> previous_keys;
        for (const EntryKey& e : previous_target_) {
            previous_keys.insert(e.key);
        }
        const std::unordered_set<EntryKey, EntryHash> previous(previous_target_.begin(), previous_target_.end());
        std::vector<EntryKey> missing;
        for (const EntryKey& e : target) {
            if (!previous.contains(e)) {
                ++stats.requests;
                if (previous_keys.contains(e.key)) {
                    ++stats.remeshes;
                }
            }
            if (!cache_.contains(e)) {
                missing.push_back(e);
            }
        }

        // Jobs: the first `budget` missing entries, coarse levels first (they cover the most screen).
        std::ranges::stable_sort(missing,
                                 [](const EntryKey& a, const EntryKey& b) { return a.key.level > b.key.level; });
        const std::size_t count = std::min<std::size_t>(missing.size(), config_.budget);
        std::vector<Job> jobs(count);
        for (std::size_t i = 0; i < count; ++i) {
            jobs[i].entry = missing[i];
        }
        if (count > 0) {
            run_jobs(world, jobs);
        }
        for (Job& job : jobs) {
            stats.sample_ms += job.sample_ms;
            stats.mesh_ms += job.mesh_ms;
            stats.build_ms += job.build_ms;
            ++stats.meshed;
            if (!job.result.mesh.is_valid()) {
                ++stats.empty;
            }
            total_sample_ms_ += job.sample_ms;
            total_mesh_ms_ += job.mesh_ms;
            total_build_ms_ += job.build_ms;
            ++total_jobs_;
            const std::size_t level = std::min<std::size_t>(job.entry.key.level, jobs_by_level_.size() - 1);
            ++jobs_by_level_[level];
            if (!job.result.mesh.is_valid()) {
                ++empty_by_level_[level];
            }
            if (job.sampled_surface) {
                ++total_surface_jobs_;
            }
            cache_.emplace(job.entry, std::move(job.result));
        }
        stats.backlog = static_cast<std::uint32_t>(missing.size() - count);

        if (stats.backlog == 0) {
            drawn_ = target;
            stale_ = 0;
            ++caught_up_frames_;
        } else {
            ++stale_;
            caught_up_frames_ = 0;
        }
        // Graceful coarsening (ADR 0053): a selection stale for over 0.25 s lowers the effective lambda, so the target
        // needs fewer fine chunks and can complete; it recovers slowly once the stream keeps up.
        if (adaptive_) {
            if (stale_ > 15) {
                lambda_ = std::max(2.0, lambda_ * 0.97);
            } else if (stale_ == 0) {
                lambda_ = std::min(config_.lambda, lambda_ * 1.005);
            }
        }
        stats.lambda = lambda_;
        backlog_ = stats.backlog;
        stats.stale = stale_;

        // Evict what neither the drawn nor the target selection uses.
        {
            std::unordered_set<EntryKey, EntryHash> keep(drawn_.begin(), drawn_.end());
            keep.insert(target.begin(), target.end());
            auto& meshes = veach::services(world).get<veach::render::MeshStore>();
            for (auto it = cache_.begin(); it != cache_.end();) {
                if (!keep.contains(it->first)) {
                    if (it->second.mesh.is_valid()) {
                        static_cast<void>(meshes.remove(it->second.mesh));
                    }
                    it = cache_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        previous_target_ = std::move(target);

        for (const EntryKey& e : drawn_) {
            stats.triangles += cache_.at(e).triangles;
        }
        stats.drawn = static_cast<std::uint32_t>(drawn_.size());
        stats.cached = static_cast<std::uint32_t>(cache_.size());
        stats.stream_ms = wall_ms() - t0;
        record(stats);
        checkpoints(world);
        if (phase_ == 5 && stale_ == 0 && !quit_requested_) {
            quit_requested_ = true;
            world.quit();
        }
    }

    void shutdown() {
        if (!initialized()) {
            return;
        }
        summarize();
    }

private:
    static double length(const std::array<double, 3>& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

    static math::quat look_rotation(math::vec3 forward, math::vec3 up) {
        const math::vec3 z = -math::normalize(forward);
        const math::vec3 x = math::normalize(math::cross(up, z));
        const math::vec3 y = math::cross(z, x);
        return math::quat_cast(math::mat3(x, y, z));
    }

    void apply_environment(Sp09Descent& d) {
        const auto number = [](const char* name, double& value) {
            if (const auto text = veach::env(name)) {
                value = std::strtod(text->c_str(), nullptr);
            }
        };
        if (const auto body = veach::env("VEACH_SP09_BODY")) {
            d.body = *body == "asteroid" ? 1U : 0U;
        }
        number("VEACH_SP09_SCALE", d.scale);
        number("VEACH_SP09_RELIEF", d.relief_exponent);
        number("VEACH_SP09_LAMBDA", d.lambda);
        number("VEACH_SP09_SKIM", d.skim_speed);
        number("VEACH_SP09_START", d.start_altitude);
        if (const auto budget = veach::env("VEACH_SP09_BUDGET")) {
            d.budget = static_cast<std::uint32_t>(std::strtoul(budget->c_str(), nullptr, 10));
        }
        if (veach::env("VEACH_SP09_COLORS") == "level") {
            d.level_colors = true;
        }
        if (veach::env("VEACH_SP09_ENCODING") == "linear") {
            d.linear_encoding = true;
        }
    }

    /// The descent's site: on the planet, a slope 0.06 rad from the highest mountain on a 20,000-point Fibonacci
    /// sphere, looking at it; on the asteroid, a fixed direction.
    void choose_site() {
        if (planet_ == nullptr) {
            const double l = std::sqrt(0.2 * 0.2 + 1.0 + 0.3 * 0.3);
            up_ = {0.2 / l, 1.0 / l, 0.3 / l};
            forward_ = tangent_of(up_, {1.0, 0.0, 0.0});
            return;
        }
        std::array<double, 3> best{0.0, 1.0, 0.0};
        double best_h = -1e30;
        constexpr int points = 20000;
        const double golden = pi * (3.0 - std::sqrt(5.0));
        for (int i = 0; i < points; ++i) {
            const double y = 1.0 - 2.0 * (i + 0.5) / points;
            const double rr = std::sqrt(1.0 - y * y);
            const double phi = golden * i;
            const std::array<double, 3> n{rr * std::cos(phi), y, rr * std::sin(phi)};
            const double h = planet_->planetary_height(n[0], n[1], n[2]);
            if (h > best_h) {
                best_h = h;
                best = n;
            }
        }
        const std::array<double, 3> away = tangent_of(best, {0.0, 1.0, 0.0});
        constexpr double offset = 0.06;
        for (std::size_t a = 0; a < 3; ++a) {
            up_[a] = std::cos(offset) * best[a] + std::sin(offset) * away[a];
        }
        // Forward: toward the peak.
        std::array<double, 3> toward{};
        for (std::size_t a = 0; a < 3; ++a) {
            toward[a] = best[a] - up_[a];
        }
        forward_ = tangent_of(up_, toward);
        veach::log::info("sp09", "highest planetary-band point {:.0f} m at ({:.4f}, {:.4f}, {:.4f})", best_h, best[0],
                         best[1], best[2]);
    }

    static std::array<double, 3> tangent_of(const std::array<double, 3>& n, const std::array<double, 3>& v) {
        const double d = n[0] * v[0] + n[1] * v[1] + n[2] * v[2];
        std::array<double, 3> t{v[0] - d * n[0], v[1] - d * n[1], v[2] - d * n[2]};
        double l = length(t);
        if (l < 1e-9) {
            t = {n[1], -n[0], 0.0};
            l = length(t);
        }
        return {t[0] / l, t[1] / l, t[2] / l};
    }

    void sun(veach::World world) {
        // From the side of the travel direction, 30 degrees above the site's horizon.
        std::array<double, 3> side{forward_[1] * up_[2] - forward_[2] * up_[1],
                                   forward_[2] * up_[0] - forward_[0] * up_[2],
                                   forward_[0] * up_[1] - forward_[1] * up_[0]};
        const double e = 30.0 * pi / 180.0;
        math::vec3 toward_sun{};
        for (int a = 0; a < 3; ++a) {
            const auto i = static_cast<std::size_t>(a);
            toward_sun[a] =
                static_cast<float>(std::sin(e) * up_[i] + std::cos(e) * (0.6 * side[i] + 0.8 * forward_[i]));
        }
        toward_sun = math::normalize(toward_sun);
        const math::vec3 hint = std::abs(toward_sun.y) > 0.9f ? math::vec3(1, 0, 0) : math::vec3(0, 1, 0);
        world.entity("sp09_sun")
            .set<veach::Transform>({.rotation = look_rotation(-toward_sun, hint)})
            .set<veach::DirectionalLight>({.color = {1.0f, 0.96f, 0.9f}, .illuminance = 110000.0f, .angle = 0.0093f});
    }

    void make_materials(veach::World world) {
        auto& meshes = veach::services(world).get<veach::render::MeshStore>();
        const auto material = [&](const char* name, math::vec3 c, float roughness) {
            return meshes.add_material(
                {.name = name, .base_color = math::vec4(c, 1.0f), .metallic = 0.0f, .roughness = roughness});
        };
        natural_ = {material("lowland", {0.20f, 0.30f, 0.11f}, 0.9f), material("highland", {0.36f, 0.33f, 0.20f}, 0.9f),
                    material("rock", {0.33f, 0.30f, 0.27f}, 0.8f), material("snow", {0.85f, 0.87f, 0.90f}, 0.6f),
                    material("regolith", {0.30f, 0.29f, 0.28f}, 0.9f)};
        for (std::size_t l = 0; l < level_palette.size(); ++l) {
            levels_.push_back(material("level", level_palette[l], 0.8f));
        }
    }

    void run_jobs(veach::World world, std::vector<Job>& jobs) {
        auto& job_system = veach::services(world).get<veach::JobSystem>();
        auto& meshes = veach::services(world).get<veach::render::MeshStore>();
        const std::size_t threads = job_system.thread_count();
        if (meshers_.size() < threads) {
            for (std::size_t i = meshers_.size(); i < threads; ++i) {
                meshers_.push_back(vx::make_transvoxel_mesher({.encoding = config_.linear_encoding
                                                                               ? vx::DistanceEncoding::linear
                                                                               : vx::DistanceEncoding::companded}));
                samples_.push_back(std::make_unique<vx::ChunkSamples>());
                chunk_meshes_.push_back(std::make_unique<vx::ChunkMesh>());
            }
        }
        job_system.parallel_for("sp09 chunks", jobs.size(), 1, [&](std::size_t begin, std::size_t end) {
            const std::size_t t = job_system.thread_index();
            for (std::size_t i = begin; i < end; ++i) {
                mesh_chunk(jobs[i], *meshers_[t], *samples_[t], *chunk_meshes_[t], meshes);
            }
        });
    }

    void mesh_chunk(Job& job, vx::IsoMesher& mesher, vx::ChunkSamples& samples, vx::ChunkMesh& mesh,
                    veach::render::MeshStore& meshes) const {
        const vx::ChunkNeighbors neighbors{.finer = job.entry.finer};
        const double t0 = wall_ms();
        vx::sample_chunk(*field_, job.entry.key, mesher.face_samples_needed(neighbors), samples);
        const double t1 = wall_ms();
        mesher.mesh(samples, neighbors, mesh);
        const double t2 = wall_ms();
        job.sample_ms = t1 - t0;
        job.mesh_ms = t2 - t1;
        job.sampled_surface = samples.has_surface;
        if (mesh.indices.empty() || !job.store) {
            job.result.triangles = static_cast<std::uint32_t>(mesh.indices.size() / 3);
            return;
        }
        // Render data: positions relative to the chunk origin, from the lattice positions (exact in double).
        const auto origin = vx::chunk_origin(job.entry.key);
        const double unit = vx::lod0_cell_meters / static_cast<double>(vx::fixed_one);
        veach::render::MeshData data;
        data.vertices.resize(mesh.fixed.size());
        for (std::size_t v = 0; v < mesh.fixed.size(); ++v) {
            veach::MeshVertex& out = data.vertices[v];
            for (std::size_t a = 0; a < 3; ++a) {
                out.position[a] =
                    static_cast<float>(static_cast<double>(mesh.fixed[v][a] - origin[a] * vx::fixed_one) * unit);
            }
            math::vec3 n(mesh.normal[v][0], mesh.normal[v][1], mesh.normal[v][2]);
            const float nl = math::length(n);
            n = nl > 0.0f ? n / nl : math::vec3(0, 1, 0);
            out.normal = {n.x, n.y, n.z};
            const math::vec3 t =
                math::normalize(math::cross(std::abs(n.y) < 0.9f ? math::vec3(0, 1, 0) : math::vec3(1, 0, 0), n));
            out.tangent = {t.x, t.y, t.z, 1.0f};
        }
        data.indices = mesh.indices;
        const std::size_t triangles = mesh.indices.size() / 3;
        // Level colors keep the natural slots (several primitives per chunk, as in the natural view) with one color.
        data.materials =
            config_.level_colors
                ? std::vector<veach::AssetId>(natural_.size(),
                                              levels_[std::min<std::size_t>(job.entry.key.level, levels_.size() - 1)])
                : natural_;
        {
            data.triangle_slots.resize(triangles);
            for (std::size_t tri = 0; tri < triangles; ++tri) {
                const std::uint32_t a = mesh.indices[3 * tri], b = mesh.indices[3 * tri + 1],
                                    c = mesh.indices[3 * tri + 2];
                std::uint32_t slot = std::max({mesh.material[a], mesh.material[b], mesh.material[c]});
                if (slot != sp09::Material::regolith) {
                    // Slope against radial up, from the lattice positions.
                    std::array<double, 3> p0{}, e1{}, e2{};
                    for (std::size_t k = 0; k < 3; ++k) {
                        p0[k] = static_cast<double>(mesh.fixed[a][k]);
                        e1[k] = static_cast<double>(mesh.fixed[b][k]) - p0[k];
                        e2[k] = static_cast<double>(mesh.fixed[c][k]) - p0[k];
                    }
                    const std::array<double, 3> fn{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                                   e1[0] * e2[1] - e1[1] * e2[0]};
                    const double fl = length(fn), pl = length(p0);
                    if (fl > 0.0 && pl > 0.0) {
                        const double up = (fn[0] * p0[0] + fn[1] * p0[1] + fn[2] * p0[2]) / (fl * pl);
                        if (up < 0.72 && slot != sp09::Material::snow) {
                            slot = sp09::Material::rock;
                        } else if (up < 0.5) {
                            slot = sp09::Material::rock;
                        }
                    }
                }
                data.triangle_slots[tri] = slot;
            }
        }
        job.result.fixed = mesh.fixed;
        job.result.indices = mesh.indices;
        job.result.triangles = static_cast<std::uint32_t>(triangles);
        const double t3 = wall_ms();
        auto added = meshes.add(std::move(data));
        job.build_ms = wall_ms() - t3;
        if (added) {
            job.result.mesh = *added;
        }
    }

    void extract(veach::render::RenderWorld& out) const {
        for (const EntryKey& e : drawn_) {
            const auto it = cache_.find(e);
            if (it == cache_.end() || !it->second.mesh.is_valid()) {
                continue;
            }
            if (!out.runtime_assets) {
                return;
            }
            const auto mesh = out.runtime_assets->meshes.find(it->second.mesh);
            if (mesh == out.runtime_assets->meshes.end()) {
                continue;
            }
            const auto origin = vx::chunk_origin(e.key);
            math::mat4 world(1.0f);
            for (int a = 0; a < 3; ++a) {
                const auto i = static_cast<std::size_t>(a);
                world[3][a] = static_cast<float>(static_cast<double>(origin[i]) * vx::lod0_cell_meters - position_[i]);
            }
            out.meshes.push_back({.mesh = it->second.mesh,
                                  .materials = mesh->second->materials,
                                  .world = world,
                                  .entity = 0x5309000000000000ULL ^ KeyHash{}(e.key)});
        }
    }

    WeldReport weld() const {
        std::vector<std::uint64_t> edges;
        for (const EntryKey& e : drawn_) {
            const ChunkEntry& c = cache_.at(e);
            for (std::size_t t = 0; t + 2 < c.indices.size(); t += 3) {
                for (int k = 0; k < 3; ++k) {
                    const auto& a = c.fixed[c.indices[t + static_cast<std::size_t>(k)]];
                    const auto& b = c.fixed[c.indices[t + static_cast<std::size_t>((k + 1) % 3)]];
                    if (a == b) {
                        continue;
                    }
                    const auto& lo = a < b ? a : b;
                    const auto& hi = a < b ? b : a;
                    std::uint64_t h = 0x243f6a8885a308d3ULL;
                    for (const auto v : {lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]}) {
                        h = mix(h ^ static_cast<std::uint64_t>(v));
                    }
                    edges.push_back(h);
                }
            }
        }
        std::ranges::sort(edges);
        WeldReport report;
        for (std::size_t i = 0; i < edges.size();) {
            std::size_t j = i;
            while (j < edges.size() && edges[j] == edges[i]) {
                ++j;
            }
            ++report.edges;
            if (j - i == 1) {
                ++report.open;
            } else if (j - i > 2) {
                ++report.non_manifold;
            }
            i = j;
        }
        return report;
    }

    void checkpoints(veach::World world) {
        std::string label;
        if (phase_ >= 1 && !checkpoints_.front().done) {
            label = "orbit";  // the warm-up is over: the drawn selection is complete
        }
        for (Checkpoint& c : checkpoints_) {
            if (label.empty() && !c.done && c.altitude > 0.0 && phase_ == 1 && altitude_ <= c.altitude) {
                label = c.label;
            }
        }
        if (label.empty() && !pending_shot_.empty()) {
            label = pending_shot_;
        }
        pending_shot_.clear();
        if (label.empty()) {
            return;
        }
        for (Checkpoint& c : checkpoints_) {
            if (c.label == label) {
                c.done = true;
            }
        }
        const WeldReport w = weld();
        std::array<std::uint32_t, 24> per_level{};
        std::uint64_t triangles = 0;
        for (const EntryKey& e : drawn_) {
            ++per_level[std::min<std::size_t>(e.key.level, per_level.size() - 1)];
            triangles += cache_.at(e).triangles;
        }
        std::string levels;
        for (std::size_t l = 0; l < per_level.size(); ++l) {
            if (per_level[l] != 0) {
                levels += std::format("{}L{}:{}", levels.empty() ? "" : " ", l, per_level[l]);
            }
        }
        veach::log::info("sp09",
                         "checkpoint {}: altitude {:.1f} m, speed {:.1f} m/s, t = {:.2f} s, drawn {} chunks ({}), "
                         "{} triangles, stale {} frames; weld: {} edges, {} open, {} non-manifold",
                         label, altitude_, speed_, descent_time_, drawn_.size(), levels, triangles, stale_, w.edges,
                         w.open, w.non_manifold);
        shots_taken_.push_back(std::format(
            R"({{"label": "{}", "altitude": {:.2f}, "speed": {:.2f}, "time": {:.3f}, "drawn": {}, "triangles": {}, )"
            R"("stale": {}, "edges": {}, "open_edges": {}, "non_manifold": {}, "levels": "{}"}})",
            label, altitude_, speed_, descent_time_, drawn_.size(), triangles, stale_, w.edges, w.open, w.non_manifold,
            levels));
        if (shots_) {
            const auto path = std::filesystem::path(out_dir_) / std::format("{}_{}.png", prefix_, label);
            veach::services(world).get<veach::render::Renderer>().request_screenshot(path);
        }
        if (label == "100m" && bench_repeats_ > 0) {
            bench(world);
        }
    }

    /// Throughput: re-mesh every drawn chunk of this checkpoint `bench_repeats_` times on the job system (no
    /// MeshStore), best run.
    void bench(veach::World world) {
        std::vector<Job> jobs(drawn_.size());
        double best = 1e30, sample = 0.0, mesh = 0.0;
        std::uint64_t surface = 0;
        for (int r = 0; r < bench_repeats_; ++r) {
            for (std::size_t i = 0; i < drawn_.size(); ++i) {
                jobs[i] = Job{.entry = drawn_[i], .store = false};
            }
            const double t0 = wall_ms();
            run_jobs(world, jobs);
            const double ms = wall_ms() - t0;
            if (ms < best) {
                best = ms;
                sample = mesh = 0.0;
                surface = 0;
                for (const Job& j : jobs) {
                    sample += j.sample_ms;
                    mesh += j.mesh_ms;
                    surface += j.result.triangles > 0 ? 1 : 0;
                }
            }
        }
        const auto threads = veach::services(world).get<veach::JobSystem>().thread_count();
        const double n = static_cast<double>(jobs.size());
        bench_ = std::format(
            R"({{"threads": {}, "chunks": {}, "with_triangles": {}, "best_ms": {:.1f}, "chunks_per_s": {:.0f}, )"
            R"("sample_ms_per_chunk": {:.3f}, "mesh_ms_per_chunk": {:.4f}}})",
            threads, jobs.size(), surface, best, n / (best / 1000.0), sample / n, mesh / n);
        veach::log::info("sp09", "bench {}", bench_);
    }

    void record(const FrameStats& s) {
        history_.push_back(s);
        if (csv_.is_open()) {
            csv_ << std::format(
                "{},{},{:.4f},{:.2f},{:.2f},{},{},{},{},{},{},{},{},{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},"
                "{:.3f},{},{:.3f}\n",
                s.frame, s.phase, s.time, s.altitude, s.speed, s.target, s.drawn, s.cached, s.requests, s.remeshes,
                s.meshed, s.empty, s.backlog, s.stale, s.select_ms, s.stream_ms, s.frame_ms, s.sample_ms, s.mesh_ms,
                s.build_ms, s.triangles, s.lambda);
        }
    }

    void summarize() {
        // Peak demand over any 1 s (60 frames: one fixed step each in headless runs), per phase group.
        const auto peak = [&](auto pick, auto in) {
            double best = 0.0, sum = 0.0;
            std::size_t start = 0;
            std::vector<double> window;
            for (const FrameStats& s : history_) {
                if (!in(s)) {
                    continue;
                }
                window.push_back(pick(s));
            }
            for (std::size_t i = 0; i < window.size(); ++i) {
                sum += window[i];
                if (i >= 60) {
                    sum -= window[start++];
                }
                best = std::max(best, sum);
            }
            return best;
        };
        const auto requests = [](const FrameStats& s) {
            return static_cast<double>(s.requests);
        };
        const auto meshed = [](const FrameStats& s) {
            return static_cast<double>(s.meshed);
        };
        const auto descent = [](const FrameStats& s) {
            return s.phase == 1;
        };
        const auto skim = [](const FrameStats& s) {
            return s.phase == 2;
        };
        const auto low = [](const FrameStats& s) {
            return s.phase == 3 || s.phase == 4;
        };
        const auto flying = [](const FrameStats& s) {
            return s.phase >= 1 && s.phase <= 4;
        };
        std::vector<double> frame_ms;
        std::uint32_t max_backlog = 0, max_stale = 0, max_drawn = 0, max_cached = 0;
        std::uint64_t max_triangles = 0;
        double max_stream = 0.0, descent_requests = 0.0;
        for (const FrameStats& s : history_) {
            if (s.phase >= 1 && s.phase <= 4) {
                frame_ms.push_back(s.frame_ms);
                max_backlog = std::max(max_backlog, s.backlog);
                max_stale = std::max(max_stale, s.stale);
                max_stream = std::max(max_stream, s.stream_ms);
            }
            if (s.phase == 1) {
                descent_requests += s.requests;
            }
            max_drawn = std::max(max_drawn, s.drawn);
            max_cached = std::max(max_cached, s.cached);
            max_triangles = std::max(max_triangles, s.triangles);
        }
        std::ranges::sort(frame_ms);
        const auto pct = [&](double p) {
            return frame_ms.empty() ? 0.0
                                    : frame_ms[static_cast<std::size_t>(p * static_cast<double>(frame_ms.size() - 1))];
        };
        const double descent_seconds = static_cast<double>(std::ranges::count_if(history_, descent)) / 60.0;
        const double jobs = static_cast<double>(std::max<std::uint64_t>(total_jobs_, 1));
        const std::string summary = std::format(
            R"({{"body": "{}", "scale": {}, "relief_exponent": {}, "lambda": {}, "budget": {}, "encoding": "{}", )"
            R"("descent_seconds": {:.2f}, "peak_requests_per_s_descent": {}, "mean_requests_per_s_descent": {:.1f}, )"
            R"("peak_requests_per_s_skim": {}, "peak_requests_per_s_low": {}, "peak_requests_per_s": {}, )"
            R"("peak_meshed_per_s": {}, "max_backlog": {}, "max_stale_frames": {}, "max_drawn": {}, "max_cached": {}, )"
            R"("max_triangles": {}, "jobs": {}, "jobs_with_surface": {}, "sample_ms_per_job": {:.3f}, )"
            R"("mesh_ms_per_job": {:.3f}, "build_ms_per_job": {:.3f}, "frame_ms_p50": {:.2f}, "frame_ms_p95": {:.2f}, )"
            R"("frame_ms_max": {:.2f}, "stream_ms_max": {:.2f}, "jobs_by_level": [{}], "empty_by_level": [{}], )"
            R"("bench": {}, "checkpoints": [{}]}})",
            config_.body == 1 ? "asteroid" : "planet", config_.scale, config_.relief_exponent, config_.lambda,
            config_.budget, config_.linear_encoding ? "linear" : "companded", descent_seconds, peak(requests, descent),
            descent_requests / std::max(descent_seconds, 1e-9), peak(requests, skim), peak(requests, low),
            peak(requests, flying), peak(meshed, flying), max_backlog, max_stale, max_drawn, max_cached, max_triangles,
            total_jobs_, total_surface_jobs_, total_sample_ms_ / jobs, total_mesh_ms_ / jobs, total_build_ms_ / jobs,
            pct(0.5), pct(0.95), pct(1.0), max_stream, numbers(jobs_by_level_), numbers(empty_by_level_),
            bench_.empty() ? std::string("null") : bench_, join(shots_taken_));
        veach::log::info("sp09", "summary {}", summary);
        if (!out_dir_.empty()) {
            std::ofstream(std::filesystem::path(out_dir_) / (prefix_ + "_summary.json")) << summary << "\n";
        }
    }

    template <typename Array>
    static std::string numbers(const Array& values) {
        std::string out;
        for (const auto v : values) {
            out += std::format("{}{}", out.empty() ? "" : ", ", v);
        }
        return out;
    }

    static std::string join(const std::vector<std::string>& parts) {
        std::string out;
        for (const std::string& p : parts) {
            out += (out.empty() ? "" : ", ") + p;
        }
        return out;
    }

    Sp09Descent config_;
    std::unique_ptr<sp09::PlanetField> planet_;
    const sp09::TerrainField* field_ = nullptr;
    std::unique_ptr<sp09::TerrainField> field_owner_;
    std::unique_ptr<Selector> selector_;

    std::array<double, 3> position_{};
    std::array<double, 3> up_{0.0, 1.0, 0.0};
    std::array<double, 3> forward_{1.0, 0.0, 0.0};
    std::uint32_t phase_ = 0;
    std::uint64_t steps_ = 0;
    std::uint64_t phase_started_ = 0;
    std::uint64_t descent_start_step_ = 0;
    std::uint64_t warmup_steps_ = 0;
    double descent_time_ = 0.0;
    double altitude_ = 0.0;
    double speed_ = 0.0;
    std::string pending_shot_;

    std::unordered_map<EntryKey, ChunkEntry, EntryHash> cache_;
    std::vector<EntryKey> drawn_;
    std::vector<EntryKey> previous_target_;
    std::uint32_t backlog_ = 0;
    std::uint32_t stale_ = 0;
    std::uint32_t caught_up_frames_ = 0;
    double lambda_ = 4.0;
    bool adaptive_ = false;
    bool quit_requested_ = false;

    std::vector<std::unique_ptr<vx::IsoMesher>> meshers_;
    std::vector<std::unique_ptr<vx::ChunkSamples>> samples_;
    std::vector<std::unique_ptr<vx::ChunkMesh>> chunk_meshes_;
    std::vector<veach::AssetId> natural_;
    std::vector<veach::AssetId> levels_;

    std::string out_dir_;
    std::string prefix_;
    bool shots_ = false;
    std::ofstream csv_;
    std::vector<Checkpoint> checkpoints_;
    std::vector<std::string> shots_taken_;
    std::vector<FrameStats> history_;
    std::uint64_t frames_ = 0;
    double last_stream_ = 0.0;
    double total_sample_ms_ = 0.0;
    double total_mesh_ms_ = 0.0;
    double total_build_ms_ = 0.0;
    std::uint64_t total_jobs_ = 0;
    std::array<std::uint64_t, 20> jobs_by_level_{};
    std::array<std::uint64_t, 20> empty_by_level_{};
    int bench_repeats_ = 0;
    std::string bench_;
    std::uint64_t total_surface_jobs_ = 0;
};

}  // namespace

void register_sp09_spike(veach::World& world) {
    world.component<Sp09Descent>()
        .member(&Sp09Descent::body, "body")
        .member(&Sp09Descent::scale, "scale")
        .member(&Sp09Descent::relief_exponent, "relief_exponent")
        .member(&Sp09Descent::start_altitude, "start_altitude")
        .member(&Sp09Descent::end_altitude, "end_altitude")
        .member(&Sp09Descent::low_altitude, "low_altitude")
        .member(&Sp09Descent::tau, "tau")
        .member(&Sp09Descent::v_floor, "v_floor")
        .member(&Sp09Descent::skim_speed, "skim_speed")
        .member(&Sp09Descent::skim_seconds, "skim_seconds")
        .member(&Sp09Descent::low_seconds, "low_seconds")
        .member(&Sp09Descent::lambda, "lambda")
        .member(&Sp09Descent::budget, "budget")
        .member(&Sp09Descent::level_colors, "level_colors")
        .member(&Sp09Descent::linear_encoding, "linear_encoding")
        .add<veach::Scriptable>();
    world.component<Sp09State>()
        .member(&Sp09State::x, "x")
        .member(&Sp09State::y, "y")
        .member(&Sp09State::z, "z")
        .member(&Sp09State::altitude, "altitude")
        .member(&Sp09State::speed, "speed")
        .member(&Sp09State::phase, "phase")
        .member(&Sp09State::time, "time")
        .member(&Sp09State::drawn, "drawn")
        .member(&Sp09State::backlog, "backlog")
        .add<veach::Scriptable>();

    auto spike = std::make_shared<Spike>();
    world.system<Sp09Descent>("Sp09Fly").kind<veach::phase::Simulation>().immediate().each(
        [spike](veach::Iter& it, std::size_t row, Sp09Descent& descent) {
            veach::Entity camera = it.entity(row);
            if (!spike->initialized()) {
                spike->initialize(camera, descent);
            }
            Sp09State state = camera.has<Sp09State>() ? *camera.try_get<Sp09State>() : Sp09State{};
            spike->fly(camera, state, static_cast<double>(it.delta_time()));
            camera.set<Sp09State>(state);
        });
    world.system("Sp09Stream").kind<veach::phase::PreExtract>().run([spike](veach::Iter& it) {
        if (spike->initialized()) {
            spike->stream(it.world());
        }
    });
    world.system("Sp09Summary").kind<veach::phase::Shutdown>().run([spike](veach::Iter&) { spike->shutdown(); });
}

}  // namespace lab
