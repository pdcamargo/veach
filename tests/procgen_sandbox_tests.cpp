// Spike SP-07 (VCH-083), throwaway: the Luau procgen sandbox (ADR 0052). The fastcall bypass reproduced and closed,
// budgets, statelessness, and the climate hook's committed hash, which the macOS gate and Windows CI both check.
// docs/research/spikes/sp-07-luau-generator.md.

#include <doctest/doctest.h>
#include <support/sp07_climate.hpp>
#include <veach/core/dmath.hpp>
#include <veach/core/hash.hpp>
#include <veach/procgen/sandbox.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <set>
#include <span>
#include <string>

using veach::ErrorCode;
using veach::procgen::HookCall;
using veach::procgen::HookOutput;
using veach::procgen::HookParam;
using veach::procgen::Sandbox;
using veach::procgen::SandboxOptions;

namespace {

SandboxOptions bypass_open() {
    SandboxOptions o;
    o.close_fastcall_bypass = false;
    return o;
}

/// Runs `source` with params and one output buffer of `n` doubles; returns them.
std::vector<double> run_doubles(Sandbox& sandbox, const std::string& source, std::span<const HookParam> params = {},
                                std::size_t n = 1, std::uint64_t seed = 7) {
    auto hook = sandbox.load(source, "test.luau");
    REQUIRE_MESSAGE(hook.has_value(), (hook ? "" : hook.error().to_string()));
    std::vector<double> r(n);
    const HookOutput outputs[]{{"r", std::as_writable_bytes(std::span(r))}};
    auto ran = sandbox.run(*hook, HookCall{seed, params, {}, outputs});
    REQUIRE_MESSAGE(ran.has_value(), (ran ? "" : ran.error().to_string()));
    return r;
}

const std::string sin_loop = R"(
return function(ctx, inputs, outputs)
    local s = 0
    for i = 1, 100 do
        s += math.sin(i * 0.37)
    end
    buffer.writef64(outputs.r, 0, s)
end)";

}  // namespace

TEST_CASE("[sp07] fastcall bypass: math.sin rebound to dmath is ignored unless disabledBuiltins lists it") {
    SUBCASE("a call in a loop: the VM's fastcall goes straight to libm") {
        Sandbox open(bypass_open());
        (void)run_doubles(open, sin_loop);
        CHECK(open.last_stats().dmath_calls == 0);  // the reproduction: our math.sin never ran

        Sandbox closed;
        (void)run_doubles(closed, sin_loop);
        CHECK(closed.last_stats().dmath_calls == 100);
    }
    SUBCASE("a call on a constant: the compiler folds it with the host's libm") {
        const std::string folded =
            "return function(ctx, inputs, outputs) buffer.writef64(outputs.r, 0, "
            "math.sin(0.5)) end";
        Sandbox open(bypass_open());
        CHECK(run_doubles(open, folded)[0] == std::sin(0.5));
        CHECK(open.last_stats().dmath_calls == 0);
        Sandbox closed;
        CHECK(run_doubles(closed, folded)[0] == veach::dmath::sin(0.5));
        CHECK(closed.last_stats().dmath_calls == 1);
    }
    SUBCASE("every libm-backed math function goes through dmath") {
        const std::string all = R"(
return function(ctx, inputs, outputs)
    local x = ctx.params.x
    local r = math.sin(x) + math.cos(x) + math.tan(x) + math.asin(x) + math.acos(x) + math.atan(x)
        + math.atan2(x, 2) + math.exp(x) + math.log(x) + math.log(x, 3) + math.log10(x) + math.pow(x, 1.7)
        + math.sinh(x) + math.cosh(x) + math.tanh(x)
    buffer.writef64(outputs.r, 0, r)
end)";
        const HookParam params[]{{"x", 0.3}};
        Sandbox closed;
        const double r = run_doubles(closed, all, params)[0];
        CHECK(closed.last_stats().dmath_calls == 15);  // 14 functions, log twice
        namespace dm = veach::dmath;
        const double x = 0.3;
        const double expected = dm::sin(x) + dm::cos(x) + dm::tan(x) + dm::asin(x) + dm::acos(x) + dm::atan(x) +
                                dm::atan2(x, 2.0) + dm::exp(x) + dm::log(x) + dm::log(x) / dm::log(3.0) + dm::log10(x) +
                                dm::pow(x, 1.7) + dm::sinh(x) + dm::cosh(x) + dm::tanh(x);
        CHECK(r == expected);
        CHECK(veach::procgen::dmath_backed_math_functions().size() == 14);
    }
    SUBCASE("by value, at an input where this platform's libm and dmath differ") {
        // Search a fixed sequence of inputs for one where libm's sin is not correctly rounded here. Apple libm and
        // the MSVC CRT differ at different inputs, so the search runs on each platform.
        double x = 0;
        bool found = false;
        for (std::uint64_t k = 0; k < 2'000'000 && !found; ++k) {
            const double c = -100.0 + 200.0 * veach::unit_double(veach::hash(0x5350ULL, k));
            if (std::sin(c) != veach::dmath::sin(c)) {
                x = c;
                found = true;
            }
        }
        if (!found) {
            MESSAGE("this platform's sin agreed with dmath on all 2,000,000 inputs; the by-value check is skipped");
            return;
        }
        MESSAGE(std::format("x = {:a}: libm sin {:a}, dmath sin {:a}", x, std::sin(x), veach::dmath::sin(x)));
        const std::string by_value =
            "return function(ctx, inputs, outputs) local x = ctx.params.x for i = 1, 2 do "
            "buffer.writef64(outputs.r, 0, math.sin(x)) end end";
        const HookParam params[]{{"x", x}};
        Sandbox open(bypass_open());
        CHECK(run_doubles(open, by_value, params)[0] == std::sin(x));
        Sandbox closed;
        CHECK(run_doubles(closed, by_value, params)[0] == veach::dmath::sin(x));
    }
}

TEST_CASE("[sp07] the sandbox has no libm randomness, clocks, I/O or environment tricks") {
    const std::string probe = R"(
return function(ctx, inputs, outputs)
    local missing = 0
    for _, v in { math.random, math.randomseed, math.noise, vector.angle, os, debug, print, getfenv, setfenv,
                  gcinfo, collectgarbage, newproxy, require } do
        missing += 1
    end
    buffer.writef64(outputs.r, 0, missing)
    buffer.writef64(outputs.r, 8, if pcall(function() math.sin = function(x: number) return 0 end end) then 1 else 0)
end)";
    Sandbox sandbox;
    const auto r = run_doubles(sandbox, probe, {}, 2);
    CHECK(r[0] == 0);  // every one of them is nil, so the table constructor holds nothing
    CHECK(r[1] == 0);  // math is read-only
}

TEST_CASE("[sp07] ^ is a compile error whose hint names math.pow") {
    Sandbox sandbox;
    for (const char* source :
         {"return function() local x = 2 ^ 0.5 end", "return function()\n local x = 3\n x ^= 2\nend"}) {
        auto hook = sandbox.load(source, "pow.luau");
        REQUIRE_FALSE(hook.has_value());
        CHECK(hook.error().code == ErrorCode::script_compile_failed);
        CHECK(hook.error().message.find("pow.luau:") != std::string::npos);
        CHECK(hook.error().hint.find("math.pow") != std::string::npos);
    }
}

TEST_CASE("[sp07] a runaway hook stops with procgen.budget_exceeded, and the sandbox stays usable") {
    SUBCASE("safepoint budget: deterministic") {
        SandboxOptions o;
        o.safepoint_budget = 1'000'000;
        Sandbox sandbox(o);
        for (const char* source : {"return function() while true do end end",
                                   "return function() while true do pcall(function() while true do end end) end end"}) {
            auto hook = sandbox.load(source, "runaway.luau");
            REQUIRE(hook.has_value());
            auto ran = sandbox.run(*hook, {});
            REQUIRE_FALSE(ran.has_value());
            CHECK(ran.error().code == ErrorCode::procgen_budget_exceeded);
            CHECK(ran.error().message.find("runaway.luau") != std::string::npos);
            CHECK(ran.error().hint.find("loop that never ends") != std::string::npos);
            CHECK(sandbox.last_stats().safepoints > o.safepoint_budget);
        }
        CHECK(run_doubles(sandbox, sin_loop)[0] != 0);
    }
    SUBCASE("wall-clock budget: a hang guard") {
        SandboxOptions o;
        o.safepoint_budget = 0;
        o.wall_budget_ms = 20;
        Sandbox sandbox(o);
        auto hook = sandbox.load("return function() local i = 0 while true do i += 1 end end", "hang.luau");
        REQUIRE(hook.has_value());
        const auto start = std::chrono::steady_clock::now();
        auto ran = sandbox.run(*hook, {});
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        REQUIRE_FALSE(ran.has_value());
        CHECK(ran.error().code == ErrorCode::procgen_budget_exceeded);
        CHECK(ran.error().message.find("wall-clock") != std::string::npos);
        CHECK(ms < 2000);
    }
    SUBCASE("memory budget") {
        SandboxOptions o;
        o.memory_budget_bytes = std::size_t{16} << 20;
        Sandbox sandbox(o);
        auto hook = sandbox.load("return function() local t = {} local i = 0 while true do i += 1 t[i] = i end end",
                                 "grow.luau");
        REQUIRE(hook.has_value());
        auto ran = sandbox.run(*hook, {});
        REQUIRE_FALSE(ran.has_value());
        CHECK(ran.error().code == ErrorCode::procgen_budget_exceeded);
        CHECK(ran.error().message.find("memory") != std::string::npos);
        CHECK(run_doubles(sandbox, sin_loop)[0] != 0);
    }
}

TEST_CASE("[sp07] nothing survives between invocations, so results never depend on call order") {
    const std::string stateful = R"(
local calls = 0
return function(ctx, inputs, outputs)
    calls += 1
    leaked = (leaked or 0) + 1
    buffer.writef64(outputs.r, 0, calls)
    buffer.writef64(outputs.r, 8, leaked)
end)";
    Sandbox sandbox;
    for (int i = 0; i < 3; ++i) {
        const auto r = run_doubles(sandbox, stateful, {}, 2);
        CHECK(r[0] == 1);
        CHECK(r[1] == 1);
    }
}

TEST_CASE("[sp07] ctx.random is veach::hash keyed by the call's seed") {
    const std::string source = R"(
return function(ctx, inputs, outputs)
    buffer.writef64(outputs.r, 0, ctx.random(1, 2, 3))
    buffer.writef64(outputs.r, 8, ctx.random(-5))
end)";
    Sandbox sandbox;
    const auto r = run_doubles(sandbox, source, {}, 2, 99);
    CHECK(r[0] == veach::unit_double(veach::hash(99, 1, 2, 3)));
    CHECK(r[1] == veach::unit_double(veach::hash(99, std::int64_t{-5})));

    auto hook = sandbox.load("return function(ctx) ctx.random(0.5) end", "bad_word.luau");
    REQUIRE(hook.has_value());
    auto ran = sandbox.run(*hook, {});
    REQUIRE_FALSE(ran.has_value());
    CHECK(ran.error().code == ErrorCode::script_runtime_error);
}

TEST_CASE("[sp07] the climate hook's output hash matches the committed one (macOS arm64 and Windows x64)") {
    const sp07::Body body = sp07::pick_body();
    REQUIRE_FALSE(body.address.empty());
    const sp07::Inputs in = sp07::make_inputs();
    const std::uint64_t input_hash = sp07::hash_inputs(body, in);

    Sandbox sandbox;
    auto hook = sandbox.load(sp07::read_hook(), "tests/scripts/sp07_climate.luau");
    REQUIRE_MESSAGE(hook.has_value(), (hook ? "" : hook.error().to_string()));
    sp07::Outputs out;
    auto ran = sp07::run(sandbox, *hook, body, in, out);
    REQUIRE_MESSAGE(ran.has_value(), (ran ? "" : ran.error().to_string()));
    const std::uint64_t output_hash = sp07::hash_outputs(out);
    INFO(
        std::format("body {} T_eq {} K tilt {} rad; inputs 0x{:016x}, outputs 0x{:016x}; {} safepoints, {} dmath calls",
                    body.address, body.equilibrium_temperature_k, body.axial_tilt_rad, input_hash, output_hash,
                    sandbox.last_stats().safepoints, sandbox.last_stats().dmath_calls));

    // A plausible climate: several biomes, temperatures between frozen and hot.
    std::set<int> biomes(out.biome.begin(), out.biome.end());
    CHECK(biomes.size() >= 6);
    for (std::uint32_t c = 0; c < sp07::cells; ++c) {
        CHECK_MESSAGE((out.temperature[c] > 150 && out.temperature[c] < 400), c);
        CHECK_MESSAGE(out.precipitation[c] >= 0, c);
    }

    // Stable from run to run and sandbox to sandbox.
    Sandbox other;
    auto hook2 = other.load(sp07::read_hook(), "tests/scripts/sp07_climate.luau");
    REQUIRE(hook2.has_value());
    sp07::Outputs again;
    REQUIRE(sp07::run(other, *hook2, body, in, again).has_value());
    CHECK(sp07::hash_outputs(again) == output_hash);

    // Regenerate only on purpose: a different value is a change to the hook, its inputs, dmath, veach::hash or the
    // Luau VM's arithmetic. Inputs first, so an input change is told apart from a hook change.
    CHECK(input_hash == 0xc699444c7897321cULL);
    CHECK(output_hash == 0x7e5315bcd6461c30ULL);
}
