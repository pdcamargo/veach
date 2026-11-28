// veach: canonical
// Bounds of a field program's outputs over the lattice domain (field_graph.hpp, lattice_bounds). Interval arithmetic
// over the op list, in double, with each op's Lipschitz constant and a bound on the interpreter's float rounding.
// Canonical: + - * / and fabs only, in a fixed order, so every machine skips the same chunks.

#include <veach/procgen/field_graph.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace veach::procgen {

namespace {

/// |noise| <= 2: each corner's value g . (d - corner) has two ±1 gradient components on offsets in [-1, 1], and the
/// trilinear blend (fade weights in [0, 1]) never leaves the corners' range.
constexpr double noise_bound = 2.0;
/// |d noise / dq_axis| <= 1 + 1.875 * 4: the blended gradients (|g_axis| <= 1) plus the fade's slope (at most
/// 30 / 16) times the difference of two corner values (at most 4). Times sqrt(3) for the gradient's length.
constexpr double noise_lipschitz = 8.5 * 1.7320508075688772;
/// One float rounding, relative, with slack for the handful of operations inside each op.
constexpr double float_eps = 1.0 / 16777216.0;
/// The noise's own rounding per unit amplitude: the fraction rounded to float (2^-24 per axis, times the gradient
/// bound) and about thirty float operations on values below 4. Generous on purpose.
constexpr double noise_error = 1.0e-5;

/// An interval, a slope term and the rounding bound. `lipschitz` is the Lipschitz constant in Lipschitz mode and the
/// largest change within the radius in variation mode: the ops combine both the same way.
struct Value {
    double lo = 0.0, hi = 0.0, lipschitz = 0.0, error = 0.0;
    [[nodiscard]] double magnitude() const { return std::max(std::fabs(lo), std::fabs(hi)); }
};

float as_float(std::uint32_t bits) {
    return std::bit_cast<float>(bits);
}
double as_double(const std::uint32_t* words) {
    return std::bit_cast<double>(std::uint64_t{words[0]} | (std::uint64_t{words[1]} << 32));
}

/// The product of two intervals.
Value product(const Value& a, const Value& b) {
    const double p[4] = {a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi};
    Value out;
    out.lo = std::min(std::min(p[0], p[1]), std::min(p[2], p[3]));
    out.hi = std::max(std::max(p[0], p[1]), std::max(p[2], p[3]));
    out.lipschitz = a.magnitude() * b.lipschitz + b.magnitude() * a.lipschitz;
    out.error = a.magnitude() * b.error + b.magnitude() * a.error + a.error * b.error;
    return out;
}

/// Adds the rounding of the op's own result.
Value rounded(Value v) {
    v.error += 4.0 * float_eps * v.magnitude();
    return v;
}

/// radius < 0: Lipschitz mode. Otherwise each octave changes by at most the smaller of its slope times the radius
/// and its whole range (4 per unit amplitude).
Value fbm(const std::uint32_t* payload, double radius) {
    const std::uint32_t octaves = payload[0];
    const double gain = as_float(payload[3]);
    double amplitude = as_float(payload[4]);
    const std::uint32_t* lattice_f = payload + 5 + octaves;
    double sum = 0.0, lipschitz = 0.0;
    for (std::uint32_t o = 0; o < octaves; ++o) {
        const double a = std::fabs(amplitude) * (1.0 + 1.0e-6);  // the kernel steps the amplitude in float
        sum += a;
        const double slope = a * noise_lipschitz * std::fabs(as_double(lattice_f + 2 * o));
        lipschitz += radius < 0.0 ? slope : std::min(slope * radius, 2.0 * noise_bound * a);
        amplitude *= gain;
    }
    Value v{-noise_bound * sum, noise_bound * sum, lipschitz, sum * noise_error};
    // The running sum rounds once per octave.
    v.error += static_cast<double>(octaves) * float_eps * v.magnitude();
    return v;
}

std::vector<Value> pass(const FieldProgram& program, double radius) {
    std::vector<Value> reg(max_registers);
    const std::uint32_t* w = program.words.data();
    const std::size_t end = program.words.size();
    for (std::size_t pc = 0; pc < end; pc += op_header_words + w[pc + 5]) {
        const auto op = static_cast<Op>(w[pc]);
        const Value& a = reg[w[pc + 2]];
        const Value& b = reg[w[pc + 3]];
        const Value& c = reg[w[pc + 4]];
        const std::uint32_t* payload = w + pc + op_header_words;
        Value out;
        switch (op) {
            case Op::fbm: out = fbm(payload, radius); break;
            case Op::ridge: {
                // (offset - |a|)^2, with r = offset - |a| in [offset - max|a|, offset - min|a|].
                const double offset = as_float(payload[0]);
                const double abs_lo = a.lo <= 0.0 && a.hi >= 0.0 ? 0.0 : std::min(std::fabs(a.lo), std::fabs(a.hi));
                const Value r{offset - a.magnitude(), offset - abs_lo, a.lipschitz, a.error};
                const double rmax = r.magnitude();
                out.lo = r.lo <= 0.0 && r.hi >= 0.0 ? 0.0 : std::min(r.lo * r.lo, r.hi * r.hi);
                out.hi = rmax * rmax;
                out.lipschitz = 2.0 * rmax * r.lipschitz;
                out.error = 2.0 * rmax * r.error + r.error * r.error;
                break;
            }
            case Op::remap: {
                const double m = as_float(payload[0]);
                const double k = as_float(payload[1]);
                out.lo = std::min(a.lo * m, a.hi * m) + k;
                out.hi = std::max(a.lo * m, a.hi * m) + k;
                out.lipschitz = std::fabs(m) * a.lipschitz;
                out.error = std::fabs(m) * a.error;
                break;
            }
            case Op::clamp: {
                const double lo = as_float(payload[0]);
                const double hi = as_float(payload[1]);
                out.lo = std::min(std::max(a.lo, lo), hi);
                out.hi = std::min(std::max(a.hi, lo), hi);
                out.lipschitz = out.lo == out.hi ? 0.0 : a.lipschitz;
                out.error = a.error;
                break;
            }
            case Op::blend: {
                // a + c (b - a)
                const Value diff{b.lo - a.hi, b.hi - a.lo, a.lipschitz + b.lipschitz, a.error + b.error};
                const Value t = product(c, diff);
                out = {a.lo + t.lo, a.hi + t.hi, a.lipschitz + t.lipschitz, a.error + t.error};
                break;
            }
            case Op::add: out = {a.lo + b.lo, a.hi + b.hi, a.lipschitz + b.lipschitz, a.error + b.error}; break;
            case Op::mul: out = product(a, b); break;
        }
        if (radius >= 0.0) {
            out.lipschitz = std::min(out.lipschitz, out.hi - out.lo);  // never more than the whole range
        }
        reg[w[pc + 1]] = rounded(out);
    }
    std::vector<Value> out;
    out.reserve(program.outputs.size());
    for (const FieldOutput& o : program.outputs) {
        out.push_back(reg[o.reg]);
    }
    return out;
}

}  // namespace

std::vector<FieldBound> lattice_bounds(const FieldProgram& program) {
    std::vector<FieldBound> bounds;
    for (const Value& v : pass(program, -1.0)) {
        bounds.push_back({v.lo, v.hi, v.lipschitz, v.error});
    }
    return bounds;
}

std::vector<double> lattice_variation(const FieldProgram& program, double radius_m) {
    std::vector<double> out;
    for (const Value& v : pass(program, std::max(radius_m, 0.0))) {
        out.push_back(v.lipschitz);
    }
    return out;
}

}  // namespace veach::procgen
