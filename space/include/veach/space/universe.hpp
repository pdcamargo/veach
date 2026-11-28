#pragma once

// The universe in a world (docs/specs/first-slice/spec.md, ADR 0088): the realization layer's star system as
// entities. Import with engine.import<veach::space::UniverseModule>() after veach::PhysicsModule and
// veach::TerrainModule; nothing happens until an entity gets a `Universe` (the creator's settings, from a startup
// script or MCP):
//
//   veach.spawn("universe"):set(c.Universe, { seed = "42", system = "S:1000400100008000", scale = 0.01 })
//
// Setting it realizes the system (veach/space/realization.hpp) and builds:
//
//   universe (the settings and its UniverseClock)
//     system            ReferenceFrame, inertial: the star system's frame, the root of every body
//       star, b1, ...   ReferenceFrame, inertial, radial up: each body's center, on rails (ADR 0048)
//         fixed         ReferenceFrame, rotating, radial up: the body-fixed frame (spin and axial tilt, ADR 0048)
//
// Each fixed step, in PostSimulation, the clock advances one tick and every body frame is written for the end of the
// step from the orbit tree (ADR 0085 decision 5): positions and velocities from the tick, orientations from the
// exact spin phase, never integrated. The planet the creator's settings name (`home`) and its moons get a voxel
// TerrainBody in their body-fixed frame (TerrainModule draws and streams it); the physics gravity hook answers for
// every body frame with that body's g R² (ADR 0059); an extractor gives the renderer the sky of the nearest body with
// an atmosphere, the sun from the orbit tree at its physical illuminance, the starfield and auto-exposure.

#include <veach/space/realization.hpp>

#include <veach/veach.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace veach::space {

/// The creator's settings (UniverseConfig, with 64-bit values as strings, ADR 0046). Scriptable. Set once: a second
/// set with different values rebuilds nothing and logs an error.
struct Universe {
    std::string seed = "42";                    ///< decimal
    std::string system = "S:1000400100008000";  ///< a system address (ADR 0050)
    double scale = 0.1;                         ///< s_p (ADR 0066's default)
    double orbit_scale = 0.0;                   ///< s_o; 0: s_p
    double atmosphere_exponent = 0.5;
    double relief_exponent = 0.5;
    double gravity = 1.0;  ///< surface gravity multiplier
    double day = 1.0;      ///< day-length multiplier
    double habitability_bias = 0.5;
    std::string home;             ///< the body to draw terrain on ("b1"); empty: StarSystem::home()
    std::int64_t start_tick = 0;  ///< the universe clock at the start
};

/// On each body frame entity: which body and which of its two frames. Scriptable.
struct CelestialFrame {
    std::string body;
    std::uint32_t index = 0;
    bool body_fixed = false;
    double radius = 0.0;  ///< m, realized
};

/// The realized universe in a world: a service of UniverseModule, ready once a Universe is set.
class UniverseWorld {
public:
    [[nodiscard]] bool ready() const { return system_ != nullptr; }
    [[nodiscard]] const StarSystem& system() const { return *system_; }
    [[nodiscard]] const RealizedBody& home() const { return system_->bodies.at(home_); }
    [[nodiscard]] std::int64_t tick() const { return tick_; }  ///< the universe tick of the state (start of a step)

    [[nodiscard]] Entity system_frame() const { return system_frame_; }
    /// The body's center frame (inertial, on rails) and its body-fixed frame.
    [[nodiscard]] Entity orbit_frame(std::uint32_t body) const;
    [[nodiscard]] Entity fixed_frame(std::uint32_t body) const;
    /// The body a frame entity belongs to (either of its two frames), or null.
    [[nodiscard]] const RealizedBody* body_of_frame(std::uint64_t frame) const;
    [[nodiscard]] bool is_fixed_frame(std::uint64_t frame) const;

    /// The body's orientation at `tick` (its axes in the system frame) and its spin rate about its own +Y, rad/s.
    [[nodiscard]] Orientation orientation(std::uint32_t body, std::int64_t tick) const;
    [[nodiscard]] double spin_rate(std::uint32_t body) const;
    /// The body's terrain density and streamed terrain (home and its moons only), or null.
    [[nodiscard]] std::shared_ptr<const voxel::ProgramDensity> density(std::uint32_t body) const;
    [[nodiscard]] voxel::TerrainBody* terrain(std::uint32_t body) const;

    /// Local solar time (hours) and the sun's elevation (radians) at a body-fixed point on `body` at `tick`.
    struct LocalSun {
        double latitude = 0.0, longitude = 0.0;  ///< radians
        double local_solar_time = 0.0;
        double elevation = 0.0;
    };
    [[nodiscard]] LocalSun local_sun(std::uint32_t body, const math::dvec3& body_fixed, std::int64_t tick) const;

    // The module's own state.
    struct Frames {
        ecs::entity_t orbit = 0;
        ecs::entity_t fixed = 0;
    };
    std::shared_ptr<const StarSystem> system_;
    std::size_t home_ = 0;
    std::int64_t tick_ = 0;
    Entity system_frame_;
    std::vector<Frames> frames_;  ///< by body index
    std::map<std::uint64_t, std::uint32_t> body_by_frame_;
    std::map<std::uint32_t, std::shared_ptr<const voxel::ProgramDensity>> densities_;
    std::map<std::uint32_t, std::shared_ptr<voxel::TerrainBody>> terrains_;
    std::vector<Pose> poses_;
    std::string key_;  ///< the settings it was built from
};

/// The renderer's atmosphere medium (kilometers) from a realized record (meters).
[[nodiscard]] render::AtmosphereParams render_medium(const AtmosphereRecord& record);

struct UniverseModule {
    explicit UniverseModule(World& world);
};

}  // namespace veach::space
