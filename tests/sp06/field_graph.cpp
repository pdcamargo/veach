// veach: canonical
// Spike SP-06 (VCH-082): the field-graph compiler and the CPU batch interpreter. The interpreter is canonical code
// (ADR 0043): only + - * / sqrt, floor, float/int conversions and integer hashing, evaluated in a fixed order, so a
// batch gives the same bits as one sample at a time and the same bits on every platform.

#include "sp06/field_graph.hpp"

#include <veach/core/hash.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>

// The batch loops vectorize only if the noise is inlined into them.
#if defined(_MSC_VER) && !defined(__clang__)
#define SP06_INLINE __forceinline
#else
#define SP06_INLINE [[gnu::always_inline]] inline
#endif

namespace veach::sp06 {

namespace {

// ---- the gradient noise (mirrored by tests/sp06/shaders/sp06_field_noise.slang) ------------------------------------

// Lattice hash: per-axis odd multipliers, xor-combined with the octave's seed, then Wellons' lowbias32 finalizer.
// 32-bit integer only, so it is bit-exact on the CPU (scalar and NEON/SSE) and on every GPU.
constexpr std::uint32_t prime_x = 0x8DA6B343u;
constexpr std::uint32_t prime_y = 0xD8163841u;
constexpr std::uint32_t prime_z = 0xCB1AB31Fu;

SP06_INLINE std::uint32_t finalize(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

/// Perlin's improved-noise gradients (the 12 cube edges, 4 repeated), chosen by the hash's low 4 bits. Selects and
/// one add: exact on every evaluator.
SP06_INLINE float grad(std::uint32_t h, float x, float y, float z) {
    const std::uint32_t k = h & 15u;
    const float u = k < 8u ? x : y;
    const float v = k < 4u ? y : (k == 12u || k == 14u ? x : z);
    return ((k & 1u) != 0u ? -u : u) + ((k & 2u) != 0u ? -v : v);
}

SP06_INLINE float lerp(float a, float b, float t) {
    return a + t * (b - a);
}

/// a * b + c rounded once: a correctly rounded fused multiply-add, for the two places where the GPU's fast math would
/// fuse anyway and the unfused rounding is large (SP-06's CPU/GPU differences): the lattice fraction p * f - floor,
/// whose product rounds by ulp(p * f), growing with the frequency, and the fade polynomial, which cancels from 10 to 1
/// near t = 1. Every evaluator computes these two this way. With hardware FMA this is std::fma; without (x64 SSE2,
/// MSVC's default) the product is exact in double (24 + 24 bits) and so is the sum for these operands (at most 48
/// significant bits), so rounding the double to float gives the same bits.
SP06_INLINE float fused(float a, float b, float c) {
#if defined(__ARM_FEATURE_FMA) || defined(__FMA__)
    return std::fma(a, b, c);
#else
    return static_cast<float>(static_cast<double>(a) * static_cast<double>(b) + static_cast<double>(c));
#endif
}

SP06_INLINE float fraction(float p, float f, float cell) {
    return fused(p, f, -cell);
}

SP06_INLINE float fade(float t) {
    return t * t * t * fused(t, fused(t, 6.0f, -15.0f), 10.0f);
}

/// One octave of gradient noise at p * f.
SP06_INLINE float noise(float px, float py, float pz, float f, std::uint32_t seed) {
    const float fx = std::floor(px * f);
    const float fy = std::floor(py * f);
    const float fz = std::floor(pz * f);
    const float dx = fraction(px, f, fx);
    const float dy = fraction(py, f, fy);
    const float dz = fraction(pz, f, fz);
    const auto ix = static_cast<std::uint32_t>(static_cast<std::int32_t>(fx));
    const auto iy = static_cast<std::uint32_t>(static_cast<std::int32_t>(fy));
    const auto iz = static_cast<std::uint32_t>(static_cast<std::int32_t>(fz));
    const std::uint32_t x0 = ix * prime_x;
    const std::uint32_t x1 = (ix + 1u) * prime_x;
    const std::uint32_t y0 = iy * prime_y;
    const std::uint32_t y1 = (iy + 1u) * prime_y;
    const std::uint32_t z0 = (iz * prime_z) ^ seed;
    const std::uint32_t z1 = ((iz + 1u) * prime_z) ^ seed;
    const float n000 = grad(finalize(x0 ^ y0 ^ z0), dx, dy, dz);
    const float n100 = grad(finalize(x1 ^ y0 ^ z0), dx - 1.0f, dy, dz);
    const float n010 = grad(finalize(x0 ^ y1 ^ z0), dx, dy - 1.0f, dz);
    const float n110 = grad(finalize(x1 ^ y1 ^ z0), dx - 1.0f, dy - 1.0f, dz);
    const float n001 = grad(finalize(x0 ^ y0 ^ z1), dx, dy, dz - 1.0f);
    const float n101 = grad(finalize(x1 ^ y0 ^ z1), dx - 1.0f, dy, dz - 1.0f);
    const float n011 = grad(finalize(x0 ^ y1 ^ z1), dx, dy - 1.0f, dz - 1.0f);
    const float n111 = grad(finalize(x1 ^ y1 ^ z1), dx - 1.0f, dy - 1.0f, dz - 1.0f);
    const float ux = fade(dx);
    const float uy = fade(dy);
    const float uz = fade(dz);
    const float nx00 = lerp(n000, n100, ux);
    const float nx10 = lerp(n010, n110, ux);
    const float nx01 = lerp(n001, n101, ux);
    const float nx11 = lerp(n011, n111, ux);
    return lerp(lerp(nx00, nx10, uy), lerp(nx01, nx11, uy), uz);
}

inline float as_float(std::uint32_t bits) {
    return std::bit_cast<float>(bits);
}

// ---- batch kernels: one op over n samples. Plain loops of selects and arithmetic, which Clang vectorizes. ----------

struct Batch {
    alignas(64) float x[batch_size];
    alignas(64) float y[batch_size];
    alignas(64) float z[batch_size];
    alignas(64) float reg[max_registers][batch_size];
};

void fbm_kernel(const Batch& b, float* __restrict out, std::size_t n, const std::uint32_t* payload) {
    const std::uint32_t octaves = payload[0];
    float frequency = as_float(payload[1]);
    const float lacunarity = as_float(payload[2]);
    const float gain = as_float(payload[3]);
    float amplitude = as_float(payload[4]);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = 0.0f;
    }
    for (std::uint32_t o = 0; o < octaves; ++o) {
        const std::uint32_t seed = payload[5 + o];
        const float f = frequency;
        const float a = amplitude;
        const float* __restrict px = b.x;
        const float* __restrict py = b.y;
        const float* __restrict pz = b.z;
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = out[i] + a * noise(px[i], py[i], pz[i], f, seed);
        }
        frequency = frequency * lacunarity;
        amplitude = amplitude * gain;
    }
}

void run(const Program& program, Batch& b, std::size_t n) {
    const std::uint32_t* w = program.words.data();
    const std::size_t end = program.words.size();
    std::size_t pc = 0;
    while (pc < end) {
        const auto op = static_cast<Op>(w[pc]);
        float* __restrict d = b.reg[w[pc + 1]];
        const float* __restrict ra = b.reg[w[pc + 2]];
        const float* __restrict rb = b.reg[w[pc + 3]];
        const float* __restrict rc = b.reg[w[pc + 4]];
        const std::uint32_t* payload = w + pc + op_header_words;
        switch (op) {
            case Op::fbm: fbm_kernel(b, d, n, payload); break;
            case Op::ridge: {
                const float offset = as_float(payload[0]);
                for (std::size_t i = 0; i < n; ++i) {
                    const float r = offset - std::fabs(ra[i]);
                    d[i] = r * r;
                }
                break;
            }
            case Op::remap: {
                const float m = as_float(payload[0]);
                const float k = as_float(payload[1]);
                for (std::size_t i = 0; i < n; ++i) {
                    d[i] = ra[i] * m + k;
                }
                break;
            }
            case Op::clamp: {
                const float lo = as_float(payload[0]);
                const float hi = as_float(payload[1]);
                for (std::size_t i = 0; i < n; ++i) {
                    d[i] = std::min(std::max(ra[i], lo), hi);
                }
                break;
            }
            case Op::blend:
                for (std::size_t i = 0; i < n; ++i) {
                    d[i] = ra[i] + rc[i] * (rb[i] - ra[i]);
                }
                break;
            case Op::add:
                for (std::size_t i = 0; i < n; ++i) {
                    d[i] = ra[i] + rb[i];
                }
                break;
            case Op::mul:
                for (std::size_t i = 0; i < n; ++i) {
                    d[i] = ra[i] * rb[i];
                }
                break;
        }
        pc += op_header_words + w[pc + 5];
    }
}

/// Unit-sphere positions of samples [i0, i0 + n) of row j (mirrored by sp06_field_noise.slang's face_direction).
void face_positions(const FaceRegion& r, std::uint32_t i0, std::uint32_t j, std::size_t n, Batch& b) {
    const float step = 2.0f / static_cast<float>(r.resolution);
    const float v = 1.0f - (static_cast<float>(j) + 0.5f) * step;
    for (std::size_t k = 0; k < n; ++k) {
        const float u = (static_cast<float>(i0 + static_cast<std::uint32_t>(k)) + 0.5f) * step - 1.0f;
        const float cx = r.center[0] + u * r.right[0] + v * r.up[0];
        const float cy = r.center[1] + u * r.right[1] + v * r.up[1];
        const float cz = r.center[2] + u * r.right[2] + v * r.up[2];
        const float len = std::sqrt(std::fma(cx, cx, std::fma(cy, cy, cz * cz)));
        b.x[k] = cx / len;
        b.y[k] = cy / len;
        b.z[k] = cz / len;
    }
}

void evaluate(const Program& program, const FaceRegion& region, std::span<float> out, std::size_t width) {
    const std::uint32_t res = region.resolution;
    auto batch = std::make_unique<Batch>();
    for (std::uint32_t j = 0; j < res; ++j) {
        for (std::uint32_t i0 = 0; i0 < res; i0 += static_cast<std::uint32_t>(width)) {
            const std::size_t n = std::min<std::size_t>(width, res - i0);
            face_positions(region, i0, j, n, *batch);
            run(program, *batch, n);
            std::memcpy(out.data() + std::size_t{j} * res + i0, batch->reg[program.output], n * sizeof(float));
        }
    }
}

// ---- the compiler
// ----------------------------------------------------------------------------------------------------

struct NodeType {
    std::string_view name;
    std::uint32_t version;
    Op op;
    std::array<std::string_view, 3> ports;  ///< scalar inputs in a, b, c order ("p" is the position)
    std::array<std::string_view, 5> params;
};

constexpr std::array<NodeType, 7> node_types{{
    {"veach:fbm", 1, Op::fbm, {"p"}, {"wavelength", "octaves", "lacunarity", "gain", "amplitude"}},
    {"veach:ridge", 1, Op::ridge, {"x"}, {"offset"}},
    {"veach:remap", 1, Op::remap, {"x"}, {"from", "to"}},
    {"veach:clamp", 1, Op::clamp, {"x"}, {"min", "max"}},
    {"veach:blend", 1, Op::blend, {"a", "b", "t"}, {}},
    {"veach:add", 1, Op::add, {"a", "b"}, {}},
    {"veach:mul", 1, Op::mul, {"a", "b"}, {}},
}};

struct Node {
    std::string id;
    const NodeType* type = nullptr;
    const nlohmann::json* json = nullptr;
    std::array<std::string, 3> inputs;
};

std::unexpected<Error> node_error(ErrorCode code, const std::string& id, const std::string& message,
                                  std::string hint = {}) {
    return make_error(code, "field graph node '" + id + "': " + message, std::move(hint));
}

Result<double> number(const Node& node, const nlohmann::json& params, std::string_view key,
                      std::optional<double> fallback) {
    const auto it = params.find(std::string(key));
    if (it == params.end()) {
        if (fallback) {
            return *fallback;
        }
        return node_error(ErrorCode::data_type_mismatch, node.id, "missing parameter '" + std::string(key) + "'");
    }
    if (!it->is_number()) {
        return node_error(ErrorCode::data_type_mismatch, node.id, "'" + std::string(key) + "' must be a number");
    }
    return it->get<double>();
}

/// A length: {"planet_m": x}. A bare number is an error (ADR 0052: every length has a unit class).
Result<double> planet_length(const Node& node, const nlohmann::json& params, std::string_view key) {
    const auto it = params.find(std::string(key));
    if (it == params.end()) {
        return node_error(ErrorCode::data_type_mismatch, node.id, "missing parameter '" + std::string(key) + "'");
    }
    if (it->is_number()) {
        return node_error(ErrorCode::data_type_mismatch, node.id,
                          "'" + std::string(key) + "' is a length without a unit class",
                          "write {\"planet_m\": " + it->dump() + "} (scaled with the planet) or {\"m\": ...}");
    }
    if (it->is_object() && it->size() == 1 && it->contains("planet_m") && it->at("planet_m").is_number()) {
        const double v = it->at("planet_m").get<double>();
        if (v > 0.0) {
            return v;
        }
    }
    if (it->is_object() && it->contains("m")) {
        return node_error(ErrorCode::data_type_mismatch, node.id,
                          "'" + std::string(key) + "' is in m, but its position input p_planet is in planet_m",
                          "use {\"planet_m\": ...}; m lengths need p_local, which this spike does not implement");
    }
    return node_error(ErrorCode::data_type_mismatch, node.id,
                      "'" + std::string(key) + "' must be a positive length like {\"planet_m\": 1000}");
}

Result<std::array<double, 2>> number_range(const Node& node, const nlohmann::json& params, std::string_view key) {
    const auto it = params.find(std::string(key));
    if (it == params.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number() || !(*it)[1].is_number()) {
        return node_error(ErrorCode::data_type_mismatch, node.id, "'" + std::string(key) + "' must be [lo, hi]");
    }
    return std::array<double, 2>{(*it)[0].get<double>(), (*it)[1].get<double>()};
}

std::uint32_t bits(double v) {
    return std::bit_cast<std::uint32_t>(static_cast<float>(v));
}

/// Appends the op's payload (constant folding happens here: lengths against the body's radius, ranges to mul/add).
Result<> emit_payload(const Node& node, const BodyContext& body, std::vector<std::uint32_t>& payload,
                      std::uint32_t& octaves_out) {
    static const nlohmann::json empty = nlohmann::json::object();
    const nlohmann::json& params = node.json->contains("params") ? node.json->at("params") : empty;
    if (!params.is_object()) {
        return node_error(ErrorCode::data_type_mismatch, node.id, "'params' must be an object");
    }
    for (const auto& [key, value] : params.items()) {
        if (std::find(node.type->params.begin(), node.type->params.end(), key) == node.type->params.end()) {
            return node_error(ErrorCode::data_unknown_field, node.id,
                              std::string(node.type->name) + " has no parameter '" + key + "'");
        }
    }
    switch (node.type->op) {
        case Op::fbm: {
            auto wavelength = planet_length(node, params, "wavelength");
            auto octaves = number(node, params, "octaves", std::nullopt);
            auto lacunarity = number(node, params, "lacunarity", 2.0);
            auto gain = number(node, params, "gain", 0.5);
            auto amplitude = number(node, params, "amplitude", 1.0);
            for (const auto* r : {&wavelength, &octaves, &lacunarity, &gain, &amplitude}) {
                if (!r->has_value()) {
                    return std::unexpected(r->error());
                }
            }
            const double n = *octaves;
            if (n < 1.0 || n > max_octaves || n != std::floor(n)) {
                return node_error(ErrorCode::data_type_mismatch, node.id,
                                  "'octaves' must be a whole number from 1 to " + std::to_string(max_octaves));
            }
            const auto count = static_cast<std::uint32_t>(n);
            // The position input is the unit direction; p_planet = direction * radius, folded into the frequency.
            payload.insert(payload.end(), {count, bits(body.radius_planet_m / *wavelength), bits(*lacunarity),
                                           bits(*gain), bits(*amplitude)});
            // ADR 0052: the node's seed is hash(body_seed, hash_id(node_id)); each octave hashes its index into it.
            const std::uint64_t node_seed = veach::hash(body.seed, hash_id(node.id));
            for (std::uint32_t o = 0; o < count; ++o) {
                const std::uint64_t h = veach::hash(node_seed, o);
                payload.push_back(static_cast<std::uint32_t>(h ^ (h >> 32)));
            }
            octaves_out += count;
            return {};
        }
        case Op::ridge: {
            auto offset = number(node, params, "offset", 1.0);
            if (!offset) {
                return std::unexpected(offset.error());
            }
            payload.push_back(bits(*offset));
            return {};
        }
        case Op::remap: {
            auto from = number_range(node, params, "from");
            auto to = number_range(node, params, "to");
            if (!from || !to) {
                return std::unexpected(from ? to.error() : from.error());
            }
            if ((*from)[1] == (*from)[0]) {
                return node_error(ErrorCode::data_type_mismatch, node.id, "'from' is an empty range");
            }
            const double m = ((*to)[1] - (*to)[0]) / ((*from)[1] - (*from)[0]);
            payload.insert(payload.end(), {bits(m), bits((*to)[0] - (*from)[0] * m)});
            return {};
        }
        case Op::clamp: {
            auto lo = number(node, params, "min", 0.0);
            auto hi = number(node, params, "max", 1.0);
            if (!lo || !hi) {
                return std::unexpected(lo ? hi.error() : lo.error());
            }
            payload.insert(payload.end(), {bits(*lo), bits(*hi)});
            return {};
        }
        case Op::blend:
        case Op::add:
        case Op::mul: return {};
    }
    return {};
}

}  // namespace

Result<Program> compile(std::string_view text, const BodyContext& body) {
    const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return make_error(ErrorCode::data_parse_failed, "field graph: not a JSON object");
    }
    if (doc.value("veach_graph", 0) != 1) {
        return make_error(ErrorCode::data_type_mismatch, "field graph: \"veach_graph\" must be 1",
                          "this build reads field graph format 1");
    }
    for (const auto& [key, value] : doc.items()) {
        if (key != "veach_graph" && key != "id" && key != "inputs" && key != "nodes" && key != "outputs") {
            return make_error(ErrorCode::data_unknown_field, "field graph: unknown field '" + key + "'");
        }
    }

    // Inputs: the only kind in the spike is p_planet (the body-fixed position, in planet_m).
    std::map<std::string, std::string, std::less<>> inputs;
    if (doc.contains("inputs") && doc["inputs"].is_object()) {
        for (const auto& [name, kind] : doc["inputs"].items()) {
            if (!kind.is_string() || kind.get<std::string>() != "p_planet") {
                return make_error(ErrorCode::data_type_mismatch, "field graph input '" + name + "': unknown kind",
                                  "the spike supports \"p_planet\" only");
            }
            inputs[name] = "p_planet";
        }
    }

    // Nodes.
    std::vector<Node> nodes;
    std::map<std::string, std::size_t, std::less<>> by_id;
    if (!doc.contains("nodes") || !doc["nodes"].is_array()) {
        return make_error(ErrorCode::data_type_mismatch, "field graph: \"nodes\" must be an array");
    }
    for (const nlohmann::json& n : doc["nodes"]) {
        Node node;
        if (!n.is_object() || !n.contains("id") || !n["id"].is_string() || !n.contains("type") ||
            !n["type"].is_string()) {
            return make_error(ErrorCode::data_type_mismatch, "field graph: every node needs a string id and type");
        }
        node.id = n["id"].get<std::string>();
        node.json = &n;
        if (by_id.contains(node.id) || inputs.contains(node.id)) {
            return node_error(ErrorCode::data_type_mismatch, node.id, "the id is used twice");
        }
        const std::string type = n["type"].get<std::string>();
        const std::size_t at = type.find('@');
        const std::string name = type.substr(0, at);
        const std::uint32_t version =
            at == std::string::npos ? 0u : static_cast<std::uint32_t>(std::strtoul(type.c_str() + at + 1, nullptr, 10));
        for (const NodeType& t : node_types) {
            if (t.name == name && t.version == version) {
                node.type = &t;
            }
        }
        if (node.type == nullptr) {
            const bool known =
                std::any_of(node_types.begin(), node_types.end(), [&](const NodeType& t) { return t.name == name; });
            return node_error(ErrorCode::data_type_mismatch, node.id, "unknown node type '" + type + "'",
                              known ? "this build has " + name + "@1" : "node types are versioned, like veach:fbm@1");
        }
        const nlohmann::json empty = nlohmann::json::object();
        const nlohmann::json& in = n.contains("inputs") ? n["inputs"] : empty;
        for (const auto& [port, ref] : in.items()) {
            const auto& ports = node.type->ports;
            const auto slot = std::find(ports.begin(), ports.end(), port);
            if (slot == ports.end() || port.empty()) {
                return node_error(ErrorCode::data_unknown_field, node.id, type + " has no input '" + port + "'");
            }
            if (!ref.is_string()) {
                return node_error(ErrorCode::data_type_mismatch, node.id, "input '" + port + "' must be a node id");
            }
            node.inputs[static_cast<std::size_t>(slot - ports.begin())] = ref.get<std::string>();
        }
        by_id[node.id] = nodes.size();
        nodes.push_back(std::move(node));
    }

    // Output: one per program in the spike (ADR 0052's multi-output programs are a follow-up).
    if (!doc.contains("outputs") || !doc["outputs"].is_object() || doc["outputs"].size() != 1 ||
        !doc["outputs"].begin()->is_string()) {
        return make_error(ErrorCode::data_type_mismatch, "field graph: \"outputs\" must map one name to a node id");
    }
    const std::string output = doc["outputs"].begin()->get<std::string>();
    if (!by_id.contains(output)) {
        return make_error(ErrorCode::data_type_mismatch, "field graph: output '" + output + "' is not a node");
    }

    // Port checks, then a depth-first walk from the output: dependency order, cycles, dead nodes dropped.
    for (const Node& node : nodes) {
        for (std::size_t s = 0; s < node.type->ports.size() && !node.type->ports[s].empty(); ++s) {
            const std::string& ref = node.inputs[s];
            const bool position = node.type->ports[s] == "p";
            if (ref.empty()) {
                return node_error(ErrorCode::data_type_mismatch, node.id,
                                  "input '" + std::string(node.type->ports[s]) + "' is not connected");
            }
            if (position != inputs.contains(ref) || (!position && !by_id.contains(ref))) {
                return node_error(ErrorCode::data_type_mismatch, node.id,
                                  "input '" + std::string(node.type->ports[s]) + "' refers to '" + ref + "'",
                                  position ? "connect a p_planet input" : "connect a node's output");
            }
        }
    }
    std::vector<std::uint8_t> mark(nodes.size(), 0);  // 0 new, 1 on the stack, 2 done
    std::vector<std::size_t> order;
    std::vector<std::uint32_t> uses(nodes.size(), 0);
    std::string cycle;
    auto visit = [&](auto&& self, std::size_t i) -> void {
        if (mark[i] == 2 || !cycle.empty()) {
            return;
        }
        if (mark[i] == 1) {
            cycle = nodes[i].id;
            return;
        }
        mark[i] = 1;
        for (std::size_t s = 0; s < 3; ++s) {
            if (const auto it = by_id.find(nodes[i].inputs[s]); it != by_id.end()) {
                ++uses[it->second];
                self(self, it->second);
            }
        }
        mark[i] = 2;
        order.push_back(i);
    };
    visit(visit, by_id[output]);
    if (!cycle.empty()) {
        return node_error(ErrorCode::data_type_mismatch, cycle, "is part of a cycle", "field graphs are acyclic");
    }

    // Emit, allocating registers so a node's output never shares a register with its inputs.
    Program program;
    std::vector<std::uint32_t> reg(nodes.size(), 0);
    std::vector<std::uint32_t> free_regs;
    for (std::uint32_t r = max_registers; r-- > 0;) {
        free_regs.push_back(r);
    }
    for (const std::size_t i : order) {
        const Node& node = nodes[i];
        if (free_regs.empty()) {
            return node_error(ErrorCode::data_type_mismatch, node.id, "needs more than 8 live values",
                              "the spike's interpreter has 8 registers");
        }
        const std::uint32_t dst = free_regs.back();
        free_regs.pop_back();
        program.registers = std::max(program.registers, max_registers - static_cast<std::uint32_t>(free_regs.size()));
        reg[i] = dst;
        std::array<std::uint32_t, 3> src{};
        for (std::size_t s = 0; s < 3; ++s) {
            if (const auto it = by_id.find(node.inputs[s]); it != by_id.end()) {
                src[s] = reg[it->second];
            }
        }
        std::vector<std::uint32_t> payload;
        if (auto ok = emit_payload(node, body, payload, program.noise_octaves); !ok) {
            return std::unexpected(ok.error());
        }
        program.words.insert(program.words.end(), {static_cast<std::uint32_t>(node.type->op), dst, src[0], src[1],
                                                   src[2], static_cast<std::uint32_t>(payload.size())});
        program.words.insert(program.words.end(), payload.begin(), payload.end());
        for (std::size_t s = 0; s < 3; ++s) {
            if (const auto it = by_id.find(node.inputs[s]); it != by_id.end() && --uses[it->second] == 0) {
                free_regs.push_back(reg[it->second]);
            }
        }
    }
    program.output = reg[by_id[output]];
    return program;
}

Result<std::string> terrain_graph_json() {
    std::ifstream file(VEACH_SOURCE_DIR "/tests/sp06/terrain.field.json", std::ios::binary);
    if (!file) {
        return make_error(ErrorCode::io_failed, "sp06: cannot read " VEACH_SOURCE_DIR "/tests/sp06/terrain.field.json");
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void evaluate_batch(const Program& program, const FaceRegion& region, std::span<float> out) {
    evaluate(program, region, out, batch_size);
}

void evaluate_scalar(const Program& program, const FaceRegion& region, std::span<float> out) {
    evaluate(program, region, out, 1);
}

float evaluate_point(const Program& program, float x, float y, float z) {
    auto batch = std::make_unique<Batch>();
    batch->x[0] = x;
    batch->y[0] = y;
    batch->z[0] = z;
    run(program, *batch, 1);
    return batch->reg[program.output][0];
}

float gradient_noise(float x, float y, float z, float frequency, std::uint32_t seed) {
    return noise(x, y, z, frequency, seed);
}

}  // namespace veach::sp06
