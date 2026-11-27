#include "sp04_spike.hpp"

#include <veach/space/orrery.hpp>
#include <veach/space/presets.hpp>
#include <veach/space/sun.hpp>

#include <cstdlib>
#include <string>

namespace lab {

namespace {

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double deg = pi / 180.0;

// getenv is fine here: read once at startup, single-threaded. MSVC and clang-cl both deprecate it.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
const char* env(const char* name) {
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
    return std::getenv(name);
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

}  // namespace

void register_sp04_spike(veach::World& world) {
    const char* scale_text = env("VEACH_SP04_SCALE");
    if (scale_text == nullptr) {
        return;
    }
    namespace vs = veach::space;
    const double scale = std::strtod(scale_text, nullptr);
    const char* event_text = env("VEACH_SP04_EVENT");
    const char* time_text = env("VEACH_SP04_TIME");
    const std::string event = event_text != nullptr ? event_text : "june";
    const bool sunset = time_text != nullptr && std::string(time_text) == "sunset";
    const int quarter = event == "september" ? 1 : (event == "december" ? 2 : 0);

    const vs::System system = vs::earthlike({.body = scale, .orbit = scale});
    const std::int64_t year_q = system.node(1).period_q;
    std::int64_t tick = (year_q * quarter / 4 + vs::period_q_per_tick / 2) / vs::period_q_per_tick;
    const double latitude = 60.0 * deg;
    const double longitude = vs::subsolar_point(system, 1, 0, tick).longitude;
    if (sunset) {
        // Bisect on whole ticks for the first one after noon with the sun's center below the horizon.
        std::int64_t lo = tick, hi = tick + vs::ticks_per_day / 2;
        while (hi - lo > 1) {
            const std::int64_t mid = lo + (hi - lo) / 2;
            (vs::sun_local(system, 1, 0, latitude, longitude, mid).elevation > 0.0 ? lo : hi) = mid;
        }
        tick = lo;
    }
    const vs::SunSample s = vs::sun_local(system, 1, 0, latitude, longitude, tick);
    veach::log::info("sp04",
                     "earthlike at s = {}: {} {} at tick {}: sun elevation {:.4f} deg, azimuth {:.2f} deg, "
                     "local solar time {:.3f} h",
                     scale, event, sunset ? "sunset" : "noon", tick, s.elevation / deg, s.azimuth / deg,
                     s.local_solar_time);

    const veach::Entity orrery = world.entity("orrery");
    orrery.set<vs::Orrery>({.system = "earthlike", .scale = scale});
    orrery.set<vs::UniverseClock>({.tick = tick, .rate = 0});
    world.entity("observer").set<vs::SunObserver>({.body = "planet", .latitude = 60.0, .longitude = longitude / deg});
}

}  // namespace lab
