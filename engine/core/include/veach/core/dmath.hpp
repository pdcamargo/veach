#pragma once

// veach::dmath: deterministic math for canonical code
// (docs/adr/0043-determinism-tiers-and-strict-floating-point.md, docs/research/spikes/sp-02-dmath.md).
//
// Every function here is **correctly rounded** in round-to-nearest (it returns the representable value nearest the
// exact result), except double atan and atan2, see below. Any two correctly rounded implementations agree bit for
// bit, so the implementation can change without changing a universe. std:: and glm:: transcendentals give different
// bits on Apple libm and the MSVC CRT; canonical code must not use them.
//
// Backed by LLVM libc's header-only math (pinned in cmake/VeachDependencies.cmake), compiled onto the same code path
// on every platform (no fused multiply-add), so every function, the two exceptions included, gives the same bits on
// macOS arm64 and Windows x64, in Debug and Release. tests/determinism_tests.cpp checks committed hashes and
// hard-to-round cases on the macOS gate and Windows CI.
//
// Exceptions: double atan and atan2 are faithfully rounded (always one of the two doubles around the exact result,
// the farther one on 15-20% of CORE-MATH's hardest inputs and on none of 10^7 random ones). The pinned LLVM libc has
// no correctly rounded version (follow-up in the spike). They are still deterministic, but a correctly rounded
// replacement will change some results: that is a `version` bump.
//
// Conditions: the floating-point environment is the default (round to nearest, no flush-to-zero), which canonical
// code asserts. NaN results are NaN, but their sign and payload are not portable (x86 and Arm make different default
// NaNs): canonical code may test for NaN, never hash its bits. Functions never set errno; floating-point exception
// flags are unspecified.
//
// Changing a result of any function here is a determinism contract change (ADR 0055): bump `version`.

#include <cstdint>

namespace veach::dmath {

/// Version of dmath's results. Part of the universe's determinism contract.
inline constexpr std::uint32_t version = 1;

// Trigonometric. Arguments in radians; sin, cos and tan reduce any finite argument exactly.
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);             ///< Faithfully rounded, not correctly rounded (see above).
double atan2(double y, double x);  ///< Faithfully rounded, not correctly rounded (see above).

// Exponential and logarithmic.
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);

// Roots and norms.
double cbrt(double x);
double hypot(double x, double y);

// Hyperbolic.
double sinh(double x);
double cosh(double x);
double tanh(double x);

// Single precision, also correctly rounded in float.
float sin(float x);
float cos(float x);
float tan(float x);
float asin(float x);
float acos(float x);
float atan(float x);
float atan2(float y, float x);
float exp(float x);
float exp2(float x);
float log(float x);
float log2(float x);
float log10(float x);
float pow(float x, float y);
float cbrt(float x);
float hypot(float x, float y);
float sinh(float x);
float cosh(float x);
float tanh(float x);

}  // namespace veach::dmath
