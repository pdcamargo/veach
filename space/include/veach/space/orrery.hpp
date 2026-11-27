#pragma once

// The orrery module (spike SP-04, VCH-080): puts a star system in the world so Luau and MCP can inspect it. Import
// with engine.import<veach::space::OrreryModule>(). Nothing happens until an entity has an `Orrery`:
//
//   veach.spawn("orrery"):set(c.Orrery, { system = "sol", scale = 1 })
//
// Each fixed step the entity's UniverseClock advances by its rate (0 pauses), then every body's state at the new
// tick is written to a child entity's CelestialBody and every SunObserver is answered. Over MCP: set_component
// UniverseClock { tick = ..., rate = 0 } on "orrery", step_frames 1, then query_entities CelestialBody or
// get_component SunObserver.

#include <veach/space/clock.hpp>

#include <veach/veach.hpp>

#include <cstdint>
#include <string>

namespace veach::space {

/// Which system to realize, and at which scale (s_p = s_o = scale). Systems: "sol", "earthlike", "reference".
struct Orrery {
    std::string system = "sol";
    double scale = 1.0;
};

/// Written each fixed step on one child of the orrery per body. System frame (+Y north of the reference plane), m.
struct CelestialBody {
    std::string name;
    std::string parent;
    double x = 0.0, y = 0.0, z = 0.0;
    double vx = 0.0, vy = 0.0, vz = 0.0;
    double radius = 0.0;
    double distance_to_parent = 0.0;
    std::int64_t tick = 0;  ///< the tick this state is for
};

/// An observer on a body's surface. Inputs: body, latitude, longitude (degrees). The rest is written each step.
struct SunObserver {
    std::string body = "earth";
    double latitude = 0.0;
    double longitude = 0.0;
    double elevation = 0.0;                    ///< degrees
    double azimuth = 0.0;                      ///< degrees from north toward east
    double local_solar_time = 0.0;             ///< hours
    double east = 0.0, north = 0.0, up = 0.0;  ///< unit direction to the star
    std::int64_t tick = 0;
};

struct OrreryModule {
    explicit OrreryModule(World& world);
};

}  // namespace veach::space
