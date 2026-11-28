// Spike SP-13 (VCH-089), throwaway bench: the numbers in docs/research/spikes/sp-13-walk-hover-land.md. Release:
//
//   veach-bench-sp13 landing [--scale S] [--climb]          descent, hover, landing (and climb): transfers, ms/step
//   veach-bench-sp13 freefall [--scale S] [--fictitious]    unpowered fall through the handoff vs an inertial control
//   veach-bench-sp13 walk [--scale S] [--hours H]           base, crate and character drift over H simulated hours
//   veach-bench-sp13 frames [--days D]                      frame math: tick phase vs integrated rotation

#include "sp13/sp13_sim.hpp"

#include <veach/core/dmath.hpp>
#include <veach/space/clock.hpp>
#include <veach/space/orbit.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace veach;
using namespace veach::sp13;

namespace {

struct Timing {
    std::vector<double> ms;
    void add(double v) { ms.push_back(v); }
    void print(const char* label) {
        if (ms.empty()) {
            return;
        }
        std::vector<double> sorted = ms;
        std::sort(sorted.begin(), sorted.end());
        double sum = 0.0;
        for (const double v : ms) {
            sum += v;
        }
        std::printf("%s: %zu steps, mean %.4f ms, p50 %.4f, p95 %.4f, p99 %.4f, max %.3f\n", label, ms.size(),
                    sum / static_cast<double>(ms.size()), sorted[sorted.size() / 2], sorted[sorted.size() * 95 / 100],
                    sorted[sorted.size() * 99 / 100], sorted.back());
    }
};

void print_transfers(const Sim& sim) {
    struct Row {
        int count = 0;
        double dv = 0.0, dx = 0.0, speed = 0.0;
    };
    std::map<std::string, Row> rows;
    for (const Transfer& t : sim.transfers()) {
        Row& r = rows[std::string(kind_name(t.kind)) + " / " + std::string(t.who)];
        ++r.count;
        r.dv = std::max(r.dv, t.dv);
        r.dx = std::max(r.dx, t.dx);
        r.speed = std::max(r.speed, t.speed);
        if (t.kind == Transfer::Kind::inertial_to_body_fixed || t.kind == Transfer::Kind::body_fixed_to_inertial ||
            t.kind == Transfer::Kind::flight_to_surface || t.kind == Transfer::Kind::surface_to_flight) {
            std::printf("  tick %lld %s: alt %.1f m, |v| %.3f m/s (old frame), |dv| %.3e m/s, |dx| %.3e m\n",
                        static_cast<long long>(t.tick), std::string(kind_name(t.kind)).c_str(), t.altitude, t.speed,
                        t.dv, t.dx);
        }
    }
    for (const auto& [name, r] : rows) {
        std::printf("  %-48s x%-5d max |dv| %.3e m/s  max |dx| %.3e m  (max |v| %.1f m/s)\n", name.c_str(), r.count,
                    r.dv, r.dx, r.speed);
    }
}

double arg_double(int argc, char** argv, const char* name, double fallback) {
    for (int i = 0; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return std::strtod(argv[i + 1], nullptr);
        }
    }
    return fallback;
}
bool flag(int argc, char** argv, const char* name) {
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

int landing(double scale, bool climb, bool walkers) {
    Sim sim(Config{.scale = scale, .walkers = walkers, .time_steps = true});
    Autopilot pilot(sim.planet(), climb);
    Timing timing;
    Controls c;
    Autopilot::Phase last = pilot.phase();
    const int limit = 60 * 60 * 10;
    for (int i = 0; i < limit && pilot.phase() != Autopilot::Phase::done; ++i) {
        sim.step(c);
        timing.add(sim.last_step_ms());
        const CraftView v = sim.craft();
        c = pilot.next(v);
        if (pilot.phase() != last) {
            std::printf("tick %lld: phase %d, alt %.2f m, v_s (%.3f, %.3f, %.3f), offset (%.2f, %.2f)\n",
                        static_cast<long long>(v.tick), static_cast<int>(pilot.phase()), v.altitude, v.ve, v.vn, v.vu,
                        v.offset_e, v.offset_n);
            last = pilot.phase();
        }
    }
    std::printf("s = %g: %lld ticks, hover altitude [%.3f, %.3f] m, touchdown %.3f m/s, %.2f m from the pad\n", scale,
                static_cast<long long>(sim.tick()), pilot.hover_min(), pilot.hover_max(), pilot.touchdown_speed(),
                pilot.landing_offset());
    std::printf("hash 0x%016llx\n", static_cast<unsigned long long>(sim.hash()));
    print_transfers(sim);
    timing.print("step");
    return 0;
}

int freefall(double scale, bool fictitious, int seconds) {
    Config config{.scale = scale, .craft_descent = 0.0, .craft_east = 0.0, .walkers = false, .fictitious = fictitious};
    Config control = config;
    control.always_inertial = true;
    Sim a(config);
    Sim b(control);
    double max_dx = 0.0, max_dv = 0.0;
    for (int i = 0; i < seconds * 60; ++i) {
        a.step({});
        b.step({});
        max_dx = std::max(max_dx, math::length(a.craft_position_inertial() - b.craft_position_inertial()));
        max_dv = std::max(max_dv, math::length(a.craft_velocity_inertial() - b.craft_velocity_inertial()));
    }
    std::printf(
        "s = %g, fictitious %s: after %d s, altitude %.1f m; vs inertial control max |dx| %.4e m, max |dv| "
        "%.4e m/s\n",
        scale, fictitious ? "on" : "off", seconds, a.craft().altitude, max_dx, max_dv);
    print_transfers(a);
    return 0;
}

int walk(double scale, double hours, bool reanchor) {
    Sim sim(Config{.scale = scale, .craft = false, .reanchor = reanchor, .time_steps = true});
    Sim control(Config{.scale = scale, .craft = false, .reanchor = false});
    const bool compare = reanchor;
    const Controls c = walk_controls(1.0, 0.025);
    // Settle first: characters start 2 cm and crates 1 cm above the ground.
    for (int i = 0; i < 300; ++i) {
        sim.step(c);
        if (compare) {
            control.step(c);
        }
    }
    const dvec3 base0 = sim.position_body("base");
    const dvec3 idler0 = sim.position_body("idler");
    const dvec3 sleepy0 = sim.position_body("crate_sleepy");
    const dvec3 awake0 = sim.position_body("crate_awake");
    Timing timing;
    const auto steps = static_cast<std::int64_t>(hours * 3600.0 * 60.0);
    double max_walker_diff = 0.0, max_idler_diff = 0.0;
    double min_walker_alt = 1e300, max_walker_alt = -1e300;
    int unsupported = 0;
    for (std::int64_t i = 0; i < steps; ++i) {
        sim.step(c);
        timing.add(sim.last_step_ms());
        if (compare) {
            control.step(c);
            max_walker_diff =
                std::max(max_walker_diff, math::length(sim.position_body("walker") - control.position_body("walker")));
            max_idler_diff =
                std::max(max_idler_diff, math::length(sim.position_body("idler") - control.position_body("idler")));
        }
        const double alt = math::length(sim.position_body("walker")) - sim.planet().radius;
        if (i > 60) {
            min_walker_alt = std::min(min_walker_alt, alt);
            max_walker_alt = std::max(max_walker_alt, alt);
            unsupported += sim.walker_supported() ? 0 : 1;
        }
    }
    std::printf("s = %g, %.2f h (%lld steps, %.3f rotations), re-anchor %s, %d terrain chunks at the end\n", scale,
                hours, static_cast<long long>(steps), hours * 3600.0 / 86'164.0905, reanchor ? "on" : "off",
                sim.terrain_chunks());
    std::printf("  base drift %.3e m, idler %.3e m, sleepy crate %.3e m, awake crate %.3e m\n",
                math::length(sim.position_body("base") - base0), math::length(sim.position_body("idler") - idler0),
                math::length(sim.position_body("crate_sleepy") - sleepy0),
                math::length(sim.position_body("crate_awake") - awake0));
    std::printf(
        "  walker height above the sphere [%.5f, %.5f] m, unsupported steps %d, walker ends %.1f m from the pad\n",
        min_walker_alt, max_walker_alt, unsupported, math::length(sim.position_body("walker") - sim.planet().site));
    if (compare) {
        std::printf("  vs no re-anchor: walker max |dx| %.3e m, idler %.3e m\n", max_walker_diff, max_idler_diff);
    }
    std::printf("  hash 0x%016llx\n", static_cast<unsigned long long>(sim.hash()));
    print_transfers(sim);
    timing.print("step");
    return 0;
}

/// Where a base on the equator is in the inertial frame after `days`: from the tick (space::orientation, exact phase)
/// against a rotation integrated step by step in double and in float32, the way a rotating scene that accumulates
/// its own orientation would do it.
int frames(double days) {
    const Planet p = make_planet(1.0);
    const space::Spin spin{.period_q = p.spin_period_q, .obliquity = p.obliquity};
    const space::Orientation o0 = space::orientation(spin, 0);
    const dvec3 base_b(p.radius, 0.0, 0.0);
    const double half = p.omega * step_seconds * 0.5;
    const math::dquat step_d(dmath::cos(half), 0.0, dmath::sin(half), 0.0);  // about the body's +Y
    const math::quat step_f(static_cast<float>(step_d.w), 0.0f, static_cast<float>(step_d.y), 0.0f);
    math::dquat q_d(1.0, 0.0, 0.0, 0.0);
    math::quat q_f(1.0f, 0.0f, 0.0f, 0.0f);
    const auto ticks = static_cast<std::int64_t>(days * 86'400.0 * 60.0);
    std::printf("tick phase vs integrated rotation, base on the equator at s = 1 (R = %.0f m); x 0.01 at s = 0.01\n",
                p.radius);
    std::int64_t report = 60 * 3600;
    for (std::int64_t t = 1; t <= ticks; ++t) {
        q_d = q_d * step_d;
        q_f = q_f * step_f;
        if (t == report || t == ticks) {
            const space::Orientation o = space::orientation(spin, t);
            const dvec3 exact = o.to_system(base_b);
            const dvec3 integrated_d = o0.to_system(q_d * base_b);
            const dvec3 integrated_f = o0.to_system(dvec3(math::mat3_cast(q_f) * math::vec3(base_b)));
            std::printf("  %10.3f h: |double integrated - tick| %.3e m, |float32 integrated - tick| %.3e m\n",
                        static_cast<double>(t) / 216'000.0, math::length(integrated_d - exact),
                        math::length(integrated_f - exact));
            report *= 4;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: veach-bench-sp13 landing|freefall|walk|frames [options]\n");
        return 2;
    }
    const std::string_view mode = argv[1];
    const double scale = arg_double(argc, argv, "--scale", 0.01);
    if (mode == "landing") {
        return landing(scale, flag(argc, argv, "--climb"), !flag(argc, argv, "--no-walkers"));
    }
    if (mode == "freefall") {
        return freefall(scale, flag(argc, argv, "--fictitious"),
                        static_cast<int>(arg_double(argc, argv, "--seconds", 60.0)));
    }
    if (mode == "walk") {
        return walk(scale, arg_double(argc, argv, "--hours", 0.1), !flag(argc, argv, "--no-reanchor"));
    }
    if (mode == "frames") {
        return frames(arg_double(argc, argv, "--days", 10.0));
    }
    std::fprintf(stderr, "unknown mode '%s'\n", argv[1]);
    return 2;
}
