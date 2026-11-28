// The lab's terrain view (terrain_view.hpp): an engine-backed descent over a planet streamed by engine/voxel.

#include "terrain_view.hpp"

#include <veach/core/env.hpp>
#include <veach/procgen/field_graph.hpp>
#include <veach/render/mesh_store.hpp>
#include <veach/render/renderer.hpp>
#include <veach/runtime/terrain.hpp>
#include <veach/voxel/terrain.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace lab {

namespace {

namespace vx = veach::voxel;
namespace math = veach::math;

constexpr double pi = 3.141592653589793238462643383279502884;

/// SP-09's planet as a field graph: continents and ridged mountains in planet meters (relief x s^0.5), and a detail
/// band in meters that never scales (300 m down to 2.3 m, +-30 m). Material 0 lowland, 1 highland, 2 mountains.
std::string planet_graph(double relief_m) {
    return std::format(R"({{
      "veach_graph": 1, "id": "lab:planet", "inputs": {{ "p": "p_planet", "q": "p_local" }},
      "nodes": [
        {{ "id": "continents", "type": "veach:fbm@1", "inputs": {{ "p": "p" }},
          "params": {{ "wavelength": {{ "planet_m": 3000000 }}, "octaves": 6 }} }},
        {{ "id": "mountains", "type": "veach:fbm@1", "inputs": {{ "p": "p" }},
          "params": {{ "wavelength": {{ "planet_m": 600000 }}, "octaves": 5 }} }},
        {{ "id": "ridges", "type": "veach:ridge@1", "inputs": {{ "x": "mountains" }}, "params": {{ "offset": 1.0 }} }},
        {{ "id": "peaks", "type": "veach:remap@1", "inputs": {{ "x": "ridges" }},
          "params": {{ "from": [0.0, 1.0], "to": [0.35, 1.0] }} }},
        {{ "id": "land", "type": "veach:remap@1", "inputs": {{ "x": "continents" }},
          "params": {{ "from": [-1.0, 1.0], "to": [0.0, 0.6] }} }},
        {{ "id": "mask_raw", "type": "veach:remap@1", "inputs": {{ "x": "continents" }},
          "params": {{ "from": [0.05, 0.3], "to": [0.0, 1.0] }} }},
        {{ "id": "mask", "type": "veach:clamp@1", "inputs": {{ "x": "mask_raw" }}, "params": {{ "min": 0.0, "max": 1.0 }} }},
        {{ "id": "height", "type": "veach:blend@1", "inputs": {{ "a": "land", "b": "peaks", "t": "mask" }} }},
        {{ "id": "raise", "type": "veach:remap@1", "inputs": {{ "x": "height" }},
          "params": {{ "from": [0.0, 1.0], "to": [0.0, {0}] }} }},
        {{ "id": "detail", "type": "veach:fbm@1", "inputs": {{ "p": "q" }},
          "params": {{ "wavelength": {{ "m": 300 }}, "octaves": 8, "amplitude": 30 }} }},
        {{ "id": "density", "type": "veach:add@1", "inputs": {{ "a": "raise", "b": "detail" }} }},
        {{ "id": "material", "type": "veach:remap@1", "inputs": {{ "x": "height" }},
          "params": {{ "from": [0.2, 0.8], "to": [0.0, 2.0] }} }}
      ],
      "outputs": {{ "density": "density", "material": "material" }}
    }})",
                       -relief_m);
}

math::quat look_rotation(math::vec3 forward, math::vec3 up) {
    const math::vec3 z = -math::normalize(forward);
    const math::vec3 x = math::normalize(math::cross(up, z));
    const math::vec3 y = math::cross(z, x);
    return math::quat_cast(math::mat3(x, y, z));
}

using Vec = std::array<double, 3>;

double length(const Vec& v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

Vec tangent_of(const Vec& n, const Vec& v) {
    const double d = n[0] * v[0] + n[1] * v[1] + n[2] * v[2];
    Vec t{v[0] - d * n[0], v[1] - d * n[1], v[2] - d * n[2]};
    const double l = length(t);
    return {t[0] / l, t[1] / l, t[2] / l};
}

struct Checkpoint {
    std::string label;
    double altitude = 0.0;  ///< taken when the descent crosses it; 0: taken by a phase change
    bool done = false;
};

class View {
public:
    [[nodiscard]] bool initialized() const { return body_ != nullptr; }

    void initialize(veach::World world, veach::Entity camera, const TerrainDescent& d) {
        config_ = d;
        const double radius = 6'371'000.0 * d.scale;
        auto graph = veach::procgen::parse_field_graph(planet_graph(6000.0 * std::sqrt(d.scale)), "lab:planet");
        auto program = graph ? veach::procgen::compile(*graph, {.seed = 0x5109, .planet_m_per_m = 1.0 / d.scale})
                             : veach::Result<veach::procgen::FieldProgram>(std::unexpected(graph.error()));
        if (!program) {
            veach::log::error("terrain", program.error());
            return;
        }
        auto density = vx::ProgramDensity::make(std::move(*program), {.shape = vx::BaseShape::sphere,
                                                                      .radius_m = radius,
                                                                      .density_output = "density",
                                                                      .material_output = "material"});
        if (!density) {
            veach::log::error("terrain", density.error());
            return;
        }
        density_ = *density;

        // The planet's frame: body-fixed, radial up. Body-local meters are its coordinates.
        planet_ = world.entity("planet").set(veach::ReferenceFrame{.up = veach::FrameUp::radial});
        vx::TerrainSettings settings;
        settings.octree = vx::OctreeBounds::around_origin(density_->max_extent() + 16.0);
        settings.lambda = d.lambda;
        settings.budget.jobs_per_frame = d.budget;
        body_ = std::make_shared<vx::TerrainBody>(density_, settings);
        auto& meshes = veach::services(world).get<veach::render::MeshStore>();
        const auto material = [&](const char* name, math::vec3 c, float roughness) {
            return meshes.add_material(
                {.name = name, .base_color = math::vec4(c, 1.0f), .metallic = 0.0f, .roughness = roughness});
        };
        veach::services(world).get<veach::TerrainBodies>().add(
            planet_, body_,
            {.materials = {material("lowland", math::vec3(0.20f, 0.30f, 0.11f), 0.9f),
                           material("highland", math::vec3(0.36f, 0.33f, 0.20f), 0.9f),
                           material("rock", math::vec3(0.33f, 0.30f, 0.27f), 0.8f)}});
        camera.child_of(planet_).add<veach::TerrainViewer>();

        // The site: a fixed direction, flying north-east-ish.
        const double l = length({0.3, 0.8, 0.52});
        up_ = {0.3 / l, 0.8 / l, 0.52 / l};
        forward_ = tangent_of(up_, {1.0, 0.0, 0.0});
        const double start = d.start_altitude > 0.0 ? d.start_altitude : std::max(radius, 200'000.0);
        const double surface = surface_radius(up_);
        for (std::size_t a = 0; a < 3; ++a) {
            position_[a] = up_[a] * (surface + start);
        }
        checkpoints_.push_back({"orbit", 0.0});
        for (const double a : {100'000.0, 10'000.0, 1'000.0}) {
            if (a < start * 0.9) {
                checkpoints_.push_back({std::format("{}km", a / 1000.0), a});
            }
        }
        for (const char* label : {"100m", "100m_skim", "10m"}) {
            checkpoints_.push_back({label, 0.0});
        }
        out_dir_ = veach::env("VEACH_TERRAIN_OUT").value_or("");
        if (!out_dir_.empty()) {
            std::filesystem::create_directories(out_dir_);
        }
        sun(world);
        veach::log::info("terrain",
                         "planet at s = {}: R = {:.0f} m, max radius {:.0f} m, root level {}, lambda {}, budget {} "
                         "chunks/frame, start altitude {:.0f} m",
                         d.scale, radius, density_->max_extent(), settings.octree.root_level, d.lambda, d.budget,
                         start);
    }

    /// Fixed step: the governor.
    void fly(veach::Entity camera, double dt) {
        const double r = length(position_);
        const Vec n{position_[0] / r, position_[1] / r, position_[2] / r};
        const double surface = surface_radius(n);
        double altitude = r - surface;
        double speed = 0.0;
        const auto skim = [&](double hold, double v) {
            const double angle = v * dt / r;
            const double c = std::cos(angle), s = std::sin(angle);
            Vec m{};
            for (std::size_t a = 0; a < 3; ++a) {
                m[a] = c * n[a] + s * forward_[a];
                forward_[a] = -s * n[a] + c * forward_[a];
            }
            const double ground = surface_radius(m);
            for (std::size_t a = 0; a < 3; ++a) {
                position_[a] = m[a] * (ground + hold);
            }
            speed = v;
        };
        const double phase_seconds = static_cast<double>(steps_ - phase_started_) * dt;
        switch (phase_) {
            case 0:  // warm-up: wait for the stream to catch up
                if (caught_up_ >= 2 || steps_ > 1200) {
                    next_phase(1);
                    descent_start_ = steps_;
                }
                break;
            case 1:
            case 3: {
                const double target = phase_ == 1 ? config_.end_altitude : config_.low_altitude;
                const double next = std::max(target, altitude - std::max(config_.v_floor, altitude / config_.tau) * dt);
                speed = (altitude - next) / dt;
                for (std::size_t a = 0; a < 3; ++a) {
                    position_[a] = n[a] * (surface + next);
                }
                altitude = next;
                if (next <= target) {
                    pending_ = phase_ == 1 ? "100m" : "";
                    next_phase(phase_ == 1 ? 2 : 4);
                }
                break;
            }
            case 2:
            case 4: {
                const bool low = phase_ == 4;
                skim(low ? config_.low_altitude : config_.end_altitude, config_.skim_speed);
                altitude = low ? config_.low_altitude : config_.end_altitude;
                if (phase_seconds >= (low ? config_.low_seconds : config_.skim_seconds)) {
                    pending_ = low ? "10m" : "100m_skim";
                    next_phase(low ? 5 : 3);
                }
                break;
            }
            default: break;
        }
        ++steps_;
        altitude_ = altitude;
        speed_ = speed;
        if (phase_ >= 1 && phase_ <= 4) {
            time_ = static_cast<double>(steps_ - descent_start_) * dt;
        }

        // Look along forward_, pitched down between the horizon and the nadir.
        const double r2 = length(position_);
        const Vec up{position_[0] / r2, position_[1] / r2, position_[2] / r2};
        const double radius = density_->settings().radius_m;
        const double dip = std::acos(std::clamp(radius / (radius + std::max(altitude, 0.0)), 0.0, 1.0));
        const double pitch = dip + 0.25 * (0.5 * pi - dip);
        forward_ = tangent_of(up, forward_);
        const math::vec3 look(static_cast<float>(std::cos(pitch) * forward_[0] - std::sin(pitch) * up[0]),
                              static_cast<float>(std::cos(pitch) * forward_[1] - std::sin(pitch) * up[1]),
                              static_cast<float>(std::cos(pitch) * forward_[2] - std::sin(pitch) * up[2]));
        const math::vec3 up_hint(static_cast<float>(up[0]), static_cast<float>(up[1]), static_cast<float>(up[2]));
        camera.set<veach::Transform>({.translation = math::dvec3(position_[0], position_[1], position_[2]),
                                      .rotation = look_rotation(look, up_hint)});
    }

    /// After TerrainModule streamed this frame: checkpoints, and the end of the run.
    void observe(veach::World world, veach::Entity camera) {
        const veach::TerrainBodies& bodies = veach::services(world).get<veach::TerrainBodies>();
        const vx::StreamStats* stats = bodies.stats(planet_);
        if (stats == nullptr) {
            return;
        }
        caught_up_ = stats->stale == 0 && stats->launched == 0 ? caught_up_ + 1 : 0;
        max_stale_ = phase_ >= 1 ? std::max(max_stale_, stats->stale) : max_stale_;
        lowest_lambda_ = std::min(lowest_lambda_, stats->lambda);
        camera.set<TerrainDescentState>({.altitude = altitude_,
                                         .speed = speed_,
                                         .phase = phase_,
                                         .time = time_,
                                         .drawn = stats->drawn,
                                         .backlog = stats->backlog,
                                         .lambda = stats->lambda});
        std::string label;
        if (phase_ >= 1 && !checkpoints_.front().done) {
            label = "orbit";
        }
        for (const Checkpoint& c : checkpoints_) {
            if (label.empty() && !c.done && c.altitude > 0.0 && phase_ == 1 && altitude_ <= c.altitude) {
                label = c.label;
            }
        }
        if (label.empty()) {
            label = std::exchange(pending_, {});
        }
        if (!label.empty()) {
            checkpoint(world, label, *stats);
        }
        if (phase_ == 5 && stats->stale == 0 && !quit_) {
            quit_ = true;
            veach::log::info("terrain",
                             "descent done: {:.1f} s, {} checkpoints, {} open edges in all, drawn set at most {} "
                             "frames stale, lowest lambda {:.2f}",
                             time_, taken_, open_edges_, max_stale_, lowest_lambda_);
            world.quit();
        }
    }

private:
    void next_phase(std::uint32_t phase) {
        phase_ = phase;
        phase_started_ = steps_;
    }

    /// The surface along unit direction n (bisection on the unquantized field).
    double surface_radius(const Vec& n) const {
        const double top = density_->max_extent();
        double lo = 2.0 * density_->settings().radius_m - top - 100.0, hi = top;
        for (int i = 0; i < 60; ++i) {
            const double mid = 0.5 * (lo + hi);
            (density_->distance(n[0] * mid, n[1] * mid, n[2] * mid) < 0.0 ? lo : hi) = mid;
        }
        return lo;
    }

    void checkpoint(veach::World world, const std::string& label, const vx::StreamStats& stats) {
        for (Checkpoint& c : checkpoints_) {
            if (c.label == label) {
                c.done = true;
            }
        }
        const vx::WeldReport w = body_->weld();
        std::array<std::uint32_t, 32> per_level{};
        for (const vx::ChunkRef& r : body_->drawn()) {
            ++per_level[r->slot.key.level];
        }
        std::string levels;
        for (std::size_t l = 0; l < per_level.size(); ++l) {
            if (per_level[l] != 0) {
                levels += std::format("{}L{}:{}", levels.empty() ? "" : " ", l, per_level[l]);
            }
        }
        ++taken_;
        open_edges_ += w.open;
        veach::log::info("terrain",
                         "checkpoint {}: altitude {:.1f} m, speed {:.1f} m/s, t = {:.2f} s, drawn {} chunks ({}), "
                         "{} triangles, lambda {:.2f}, stale {}; weld: {} edges, {} open",
                         label, altitude_, speed_, time_, stats.drawn, levels, w.triangles, stats.lambda, stats.stale,
                         w.edges, w.open);
        if (w.open != 0) {
            veach::log::error("terrain", "checkpoint {}: {} open edges in the drawn terrain", label, w.open);
        }
        if (!out_dir_.empty()) {
            veach::services(world).get<veach::render::Renderer>().request_screenshot(
                std::filesystem::path(out_dir_) / std::format("terrain_{}.png", label));
        }
    }

    void sun(veach::World world) {
        // From the side of the travel direction, 30 degrees above the site's horizon.
        const Vec side{forward_[1] * up_[2] - forward_[2] * up_[1], forward_[2] * up_[0] - forward_[0] * up_[2],
                       forward_[0] * up_[1] - forward_[1] * up_[0]};
        const double e = 30.0 * pi / 180.0;
        math::vec3 toward{};
        for (std::size_t a = 0; a < 3; ++a) {
            toward[static_cast<int>(a)] =
                static_cast<float>(std::sin(e) * up_[a] + std::cos(e) * (0.6 * side[a] + 0.8 * forward_[a]));
        }
        toward = math::normalize(toward);
        const math::vec3 hint = std::abs(toward.y) > 0.9f ? math::vec3(1.0f, 0.0f, 0.0f) : math::vec3(0.0f, 1.0f, 0.0f);
        world.entity("terrain_sun")
            .child_of(planet_)
            .set<veach::Transform>({.rotation = look_rotation(-toward, hint)})
            .set<veach::DirectionalLight>({.color = {1.0f, 0.96f, 0.9f}, .illuminance = 110000.0f, .angle = 0.0093f});
    }

    TerrainDescent config_;
    std::shared_ptr<const vx::ProgramDensity> density_;
    std::shared_ptr<vx::TerrainBody> body_;
    veach::Entity planet_;
    Vec position_{}, up_{0.0, 1.0, 0.0}, forward_{1.0, 0.0, 0.0};
    std::uint32_t phase_ = 0;
    std::uint64_t steps_ = 0, phase_started_ = 0, descent_start_ = 0;
    double altitude_ = 0.0, speed_ = 0.0, time_ = 0.0;
    std::string pending_;
    std::vector<Checkpoint> checkpoints_;
    std::string out_dir_;
    std::uint32_t caught_up_ = 0, max_stale_ = 0, taken_ = 0;
    std::uint64_t open_edges_ = 0;
    double lowest_lambda_ = 1e30;
    bool quit_ = false;
};

}  // namespace

void register_terrain_view(veach::World& world) {
    world.component<TerrainDescent>()
        .member(&TerrainDescent::scale, "scale")
        .member(&TerrainDescent::start_altitude, "start_altitude")
        .member(&TerrainDescent::end_altitude, "end_altitude")
        .member(&TerrainDescent::low_altitude, "low_altitude")
        .member(&TerrainDescent::tau, "tau")
        .member(&TerrainDescent::v_floor, "v_floor")
        .member(&TerrainDescent::skim_speed, "skim_speed")
        .member(&TerrainDescent::skim_seconds, "skim_seconds")
        .member(&TerrainDescent::low_seconds, "low_seconds")
        .member(&TerrainDescent::lambda, "lambda")
        .member(&TerrainDescent::budget, "budget")
        .add<veach::Scriptable>();
    world.component<TerrainDescentState>()
        .member(&TerrainDescentState::altitude, "altitude")
        .member(&TerrainDescentState::speed, "speed")
        .member(&TerrainDescentState::phase, "phase")
        .member(&TerrainDescentState::time, "time")
        .member(&TerrainDescentState::drawn, "drawn")
        .member(&TerrainDescentState::backlog, "backlog")
        .member(&TerrainDescentState::lambda, "lambda")
        .add<veach::Scriptable>();

    auto view = std::make_shared<View>();
    // immediate(): the first step creates the planet's frame and parents the camera to it.
    world.system<const TerrainDescent>("TerrainDescentFly")
        .kind<veach::phase::Simulation>()
        .immediate()
        .each([view](veach::Iter& it, std::size_t row, const TerrainDescent& descent) {
            veach::Entity camera = it.entity(row);
            if (!view->initialized()) {
                view->initialize(it.world(), camera, descent);
                if (!view->initialized()) {
                    return;
                }
            }
            view->fly(camera, static_cast<double>(it.delta_time()));
        });
    world.system<const TerrainDescent>("TerrainDescentObserve")
        .kind<veach::phase::PreExtract>()
        .each([view](veach::Iter& it, std::size_t row, const TerrainDescent&) {
            if (view->initialized()) {
                view->observe(it.world(), it.entity(row));
            }
        });
}

}  // namespace lab
