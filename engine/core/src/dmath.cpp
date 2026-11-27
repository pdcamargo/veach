// veach::dmath on LLVM libc's header-only math (docs/research/spikes/sp-02-dmath.md).
//
// One code path everywhere. LLVM libc picks fused multiply-add code when the target has FMA (every arm64 Mac) and
// separate multiply and add code when it doesn't (MSVC x64, which never defines the macros it checks). Both are meant
// to be correctly rounded, but SP-02 found inputs where they differ (sin and tan below 2^-960), so FMA is turned off
// here on every platform: the Mac runs exactly the code MSVC runs, and equal bits rest only on IEEE + - * / sqrt.
// SP-02 measured the cost on an M5 Max at up to 2x per call (sin 3.5 -> 7.4 ns), see the spike for the table.
//
// Wrapper rules, each exact, where the pinned LLVM libc is not correctly rounded (compared against CORE-MATH over 10^7
// inputs per function and its worst-case lists, see the spike):
// - sin, tan: for tiny x the correctly rounded result is x itself (|sin x - x| < |x|^3/6 stays under half an ulp below
//   2^-26, |tan x - x| < |x|^3/2 below 2^-27); LLVM's no-FMA path rounds x - x*2^-54 twice and can land one ulp off
//   when x*2^-54 is subnormal.
// - atan2: when the quotient underflows, the result is a zero with the sign of y; LLVM returns +0 for y < 0.
// Double atan and atan2 stay faithfully rounded, not correctly rounded (dmath.hpp): LLVM has no correctly rounded
// version. atan(x) is atan2(x, 1), which misses fewer hard cases than LLVM's atan and none of 10^7 random inputs.

// The namespace keeps these inline functions apart from any other copy of LLVM libc linked into the process, and the
// flags match LLVM's own APFloat (llvm/lib/Support/APFloat.cpp): no errno, no explicit exception raising.
#define LIBC_NAMESPACE veach_llvm_libc
#define LIBC_MATH (LIBC_MATH_NO_ERRNO | LIBC_MATH_NO_EXCEPT)

// Must come before every other LLVM libc header: they test these macros as they are included.
#include "src/__support/macros/properties/cpu_features.h"
#undef LIBC_TARGET_CPU_HAS_FMA
#undef LIBC_TARGET_CPU_HAS_FMA_HALF
#undef LIBC_TARGET_CPU_HAS_FMA_FLOAT
#undef LIBC_TARGET_CPU_HAS_FMA_DOUBLE

#include "shared/math/acos.h"
#include "shared/math/acosf.h"
#include "shared/math/asin.h"
#include "shared/math/asinf.h"
#include "shared/math/atan2.h"
#include "shared/math/atan2f.h"
#include "shared/math/atanf.h"
#include "shared/math/cbrt.h"
#include "shared/math/cbrtf.h"
#include "shared/math/cos.h"
#include "shared/math/cosf.h"
#include "shared/math/cosh.h"
#include "shared/math/coshf.h"
#include "shared/math/exp.h"
#include "shared/math/exp2.h"
#include "shared/math/exp2f.h"
#include "shared/math/expf.h"
#include "shared/math/hypot.h"
#include "shared/math/hypotf.h"
#include "shared/math/log.h"
#include "shared/math/log10.h"
#include "shared/math/log10f.h"
#include "shared/math/log2.h"
#include "shared/math/log2f.h"
#include "shared/math/logf.h"
#include "shared/math/pow.h"
#include "shared/math/powf.h"
#include "shared/math/sin.h"
#include "shared/math/sinf.h"
#include "shared/math/sinh.h"
#include "shared/math/sinhf.h"
#include "shared/math/tan.h"
#include "shared/math/tanf.h"
#include "shared/math/tanh.h"
#include "shared/math/tanhf.h"

#if defined(LIBC_TARGET_CPU_HAS_FMA) || defined(LIBC_TARGET_CPU_HAS_FMA_DOUBLE) || \
    defined(LIBC_TARGET_CPU_HAS_FMA_FLOAT)
#error "veach::dmath must compile LLVM libc without FMA on every platform"
#endif
#ifdef LIBC_MATH_HAS_SKIP_ACCURATE_PASS
#error "veach::dmath needs LLVM libc's accurate passes: they make it correctly rounded"
#endif

#include <veach/core/dmath.hpp>

#include <bit>
#include <cstdint>

namespace veach::dmath {

namespace {

namespace libc = veach_llvm_libc::shared;

constexpr std::uint64_t abs_bits(double x) {
    return std::bit_cast<std::uint64_t>(x) & 0x7fffffffffffffffULL;
}

/// |x| < 2^-26 (0x3e5 is the biased exponent of 2^-26), ±0 included, NaN not.
constexpr bool below_2_pow_m26(double x) {
    return abs_bits(x) < 0x3e50000000000000ULL;
}

/// |x| < 2^-27.
constexpr bool below_2_pow_m27(double x) {
    return abs_bits(x) < 0x3e40000000000000ULL;
}

}  // namespace

double sin(double x) {
    return below_2_pow_m26(x) ? x : libc::sin(x);
}
double cos(double x) {
    return libc::cos(x);
}
double tan(double x) {
    return below_2_pow_m27(x) ? x : libc::tan(x);
}
double asin(double x) {
    return libc::asin(x);
}
double acos(double x) {
    return libc::acos(x);
}

double atan2(double y, double x) {
    const double r = libc::atan2(y, x);
    if (r == 0.0) {
        // An underflowing quotient (or y = ±0 with x > 0): exactly zero after rounding, signed like y.
        return std::bit_cast<double>(std::bit_cast<std::uint64_t>(y) & 0x8000000000000000ULL);
    }
    return r;
}

double atan(double x) {
    return atan2(x, 1.0);
}
double exp(double x) {
    return libc::exp(x);
}
double exp2(double x) {
    return libc::exp2(x);
}
double log(double x) {
    return libc::log(x);
}
double log2(double x) {
    return libc::log2(x);
}
double log10(double x) {
    return libc::log10(x);
}
double pow(double x, double y) {
    return libc::pow(x, y);
}
double cbrt(double x) {
    return libc::cbrt(x);
}
double hypot(double x, double y) {
    return libc::hypot(x, y);
}
double sinh(double x) {
    return libc::sinh(x);
}
double cosh(double x) {
    return libc::cosh(x);
}
double tanh(double x) {
    return libc::tanh(x);
}

float sin(float x) {
    return libc::sinf(x);
}
float cos(float x) {
    return libc::cosf(x);
}
float tan(float x) {
    return libc::tanf(x);
}
float asin(float x) {
    return libc::asinf(x);
}
float acos(float x) {
    return libc::acosf(x);
}
float atan(float x) {
    return libc::atanf(x);
}
float atan2(float y, float x) {
    return libc::atan2f(y, x);
}
float exp(float x) {
    return libc::expf(x);
}
float exp2(float x) {
    return libc::exp2f(x);
}
float log(float x) {
    return libc::logf(x);
}
float log2(float x) {
    return libc::log2f(x);
}
float log10(float x) {
    return libc::log10f(x);
}
float pow(float x, float y) {
    return libc::powf(x, y);
}
float cbrt(float x) {
    return libc::cbrtf(x);
}
float hypot(float x, float y) {
    return libc::hypotf(x, y);
}
float sinh(float x) {
    return libc::sinhf(x);
}
float cosh(float x) {
    return libc::coshf(x);
}
float tanh(float x) {
    return libc::tanhf(x);
}

}  // namespace veach::dmath
