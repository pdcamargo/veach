// veach: canonical
// The field-graph loader and compiler (promoted from spike SP-06, docs/research/spikes/sp-06-field-graph.md): JSON in,
// a validated FieldGraph, then a FieldProgram per body with its constants and seeds folded in. Canonical: the folded
// constants are double arithmetic rounded once to float, and seeds come from veach::hash (ADR 0044).

#include "field_graph_ir.hpp"

#include <veach/core/hash.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <format>
#include <map>
#include <optional>
#include <string>

namespace veach::procgen {

namespace {

using nlohmann::json;

constexpr std::array<NodeType, 7> node_types{{
    {"veach:fbm", 1, Op::fbm, {"p"}, {"wavelength", "octaves", "lacunarity", "gain", "amplitude"}},
    {"veach:ridge", 1, Op::ridge, {"x"}, {"offset"}},
    {"veach:remap", 1, Op::remap, {"x"}, {"from", "to"}},
    {"veach:clamp", 1, Op::clamp, {"x"}, {"min", "max"}},
    {"veach:blend", 1, Op::blend, {"a", "b", "t"}, {}},
    {"veach:add", 1, Op::add, {"a", "b"}, {}},
    {"veach:mul", 1, Op::mul, {"a", "b"}, {}},
}};

constexpr std::string_view unit_name(LengthUnit u) {
    return u == LengthUnit::planet_m ? "planet_m" : "m";
}
constexpr std::string_view kind_name(PositionKind k) {
    return k == PositionKind::p_planet ? "p_planet" : "p_local";
}
constexpr LengthUnit unit_of(PositionKind k) {
    return k == PositionKind::p_planet ? LengthUnit::planet_m : LengthUnit::m;
}

/// Builds errors that start with the graph's name.
struct Errors {
    std::string source;

    [[nodiscard]] std::unexpected<Error> graph(ErrorCode code, std::string_view message, std::string hint = {}) const {
        return make_error(code, std::format("{}: {}", source, message), std::move(hint));
    }
    [[nodiscard]] std::unexpected<Error> node(ErrorCode code, std::string_view id, std::string_view message,
                                              std::string hint = {}) const {
        return make_error(code, std::format("{}: node '{}': {}", source, id, message), std::move(hint));
    }
};

std::string known_types() {
    std::string out;
    for (const NodeType& t : node_types) {
        out += std::format("{}{}@{}", out.empty() ? "" : ", ", t.name, t.version);
    }
    return out;
}

struct ParamReader {
    const Errors& err;
    const NodeIR& node;
    const json& params;

    Result<double> number(std::string_view key, std::optional<double> fallback) const {
        const auto it = params.find(key);
        if (it == params.end()) {
            if (fallback) {
                return *fallback;
            }
            return err.node(ErrorCode::procgen_graph_invalid, node.id, std::format("missing parameter '{}'", key));
        }
        if (!it->is_number()) {
            return err.node(ErrorCode::procgen_graph_invalid, node.id, std::format("'{}' must be a number", key));
        }
        return it->get<double>();
    }

    /// A length: {"planet_m": x} or {"m": x}, positive. A bare number is an error (ADR 0052: every length has a
    /// unit class).
    Result<std::pair<double, LengthUnit>> length(std::string_view key) const {
        const auto it = params.find(key);
        if (it == params.end()) {
            return err.node(ErrorCode::procgen_graph_invalid, node.id, std::format("missing parameter '{}'", key));
        }
        if (it->is_number()) {
            return err.node(ErrorCode::procgen_graph_invalid, node.id,
                            std::format("'{}' is a length without a unit class", key),
                            std::format("write {{\"planet_m\": {0}}} (scaled with the planet, sampled at p_planet) or "
                                        "{{\"m\": {0}}} (never scaled, sampled at p_local)",
                                        it->dump()));
        }
        if (it->is_object() && it->size() == 1) {
            for (const LengthUnit u : {LengthUnit::planet_m, LengthUnit::m}) {
                const auto v = it->find(unit_name(u));
                if (v != it->end() && v->is_number() && v->get<double>() > 0.0) {
                    return std::pair{v->get<double>(), u};
                }
            }
        }
        return err.node(ErrorCode::procgen_graph_invalid, node.id,
                        std::format("'{}' must be a positive length like {{\"planet_m\": 1000}} or {{\"m\": 3}}", key));
    }

    Result<std::array<double, 2>> range(std::string_view key) const {
        const auto it = params.find(key);
        if (it == params.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number() ||
            !(*it)[1].is_number()) {
            return err.node(ErrorCode::procgen_graph_invalid, node.id, std::format("'{}' must be [lo, hi]", key));
        }
        return std::array<double, 2>{(*it)[0].get<double>(), (*it)[1].get<double>()};
    }
};

/// Resolves a node's parameters into NodeIR::values:
///   fbm:   wavelength, octaves, lacunarity, gain, amplitude (and NodeIR::unit)
///   ridge: offset
///   remap: mul, add (folded from the ranges: a * mul + add maps `from` onto `to`)
///   clamp: min, max
Result<> parse_params(const Errors& err, const json& node_json, NodeIR& node) {
    static const json empty = json::object();
    const json& params = node_json.contains("params") ? node_json.at("params") : empty;
    if (!params.is_object()) {
        return err.node(ErrorCode::procgen_graph_invalid, node.id, "'params' must be an object");
    }
    for (const auto& [key, value] : params.items()) {
        if (std::find(node.type->params.begin(), node.type->params.end(), key) == node.type->params.end()) {
            std::string names;
            for (const std::string_view p : node.type->params) {
                if (!p.empty()) {
                    names += std::format("{}{}", names.empty() ? "" : ", ", p);
                }
            }
            return err.node(ErrorCode::data_unknown_field, node.id,
                            std::format("{}@{} has no parameter '{}'", node.type->name, node.type->version, key),
                            names.empty() ? std::string("this node type takes no parameters")
                                          : std::format("its parameters are {}", names));
        }
    }
    const ParamReader read{err, node, params};
    auto take = [](const auto& r, double& into) -> Result<> {
        if (!r) {
            return std::unexpected(r.error());
        }
        into = *r;
        return {};
    };
    switch (node.type->op) {
        case Op::fbm: {
            auto wavelength = read.length("wavelength");
            if (!wavelength) {
                return std::unexpected(wavelength.error());
            }
            node.values[0] = wavelength->first;
            node.unit = wavelength->second;
            if (auto ok = take(read.number("octaves", std::nullopt), node.values[1]); !ok) {
                return ok;
            }
            const double n = node.values[1];
            if (n < 1.0 || n > max_octaves || n != std::floor(n)) {
                return err.node(ErrorCode::procgen_graph_invalid, node.id,
                                std::format("'octaves' must be a whole number from 1 to {}", max_octaves));
            }
            for (const auto& [key, fallback, slot] :
                 {std::tuple{"lacunarity", 2.0, 2}, std::tuple{"gain", 0.5, 3}, std::tuple{"amplitude", 1.0, 4}}) {
                if (auto ok = take(read.number(key, fallback), node.values[static_cast<std::size_t>(slot)]); !ok) {
                    return ok;
                }
            }
            return {};
        }
        case Op::ridge: return take(read.number("offset", 1.0), node.values[0]);
        case Op::remap: {
            auto from = read.range("from");
            if (!from) {
                return std::unexpected(from.error());
            }
            auto to = read.range("to");
            if (!to) {
                return std::unexpected(to.error());
            }
            if ((*from)[1] == (*from)[0]) {
                return err.node(ErrorCode::procgen_graph_invalid, node.id, "'from' is an empty range");
            }
            const double m = ((*to)[1] - (*to)[0]) / ((*from)[1] - (*from)[0]);
            node.values[0] = m;
            node.values[1] = (*to)[0] - (*from)[0] * m;
            return {};
        }
        case Op::clamp:
            if (auto ok = take(read.number("min", 0.0), node.values[0]); !ok) {
                return ok;
            }
            return take(read.number("max", 1.0), node.values[1]);
        case Op::blend:
        case Op::add:
        case Op::mul: return {};
    }
    return {};
}

Result<const NodeType*> find_type(const Errors& err, const std::string& id, const std::string& type) {
    const std::size_t at = type.find('@');
    const std::string name = type.substr(0, at);
    std::uint32_t version = 0;
    if (at != std::string::npos) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(type.c_str() + at + 1, &end, 10);
        version = end != nullptr && *end == '\0' && at + 1 < type.size() ? static_cast<std::uint32_t>(v) : 0u;
    }
    for (const NodeType& t : node_types) {
        if (t.name == name && t.version == version) {
            return &t;
        }
    }
    const auto same =
        std::find_if(node_types.begin(), node_types.end(), [&](const NodeType& t) { return t.name == name; });
    return err.node(
        ErrorCode::procgen_graph_invalid, id, std::format("unknown node type '{}'", type),
        same != node_types.end()
            ? std::format("this build has {}@{}; a different version is a different node", name, same->version)
            : std::format("node types are versioned, like veach:fbm@1; this build has {}", known_types()));
}

std::uint32_t bits(double v) {
    return std::bit_cast<std::uint32_t>(static_cast<float>(v));
}

void push_double(std::vector<std::uint32_t>& out, double v) {
    const auto b = std::bit_cast<std::uint64_t>(v);
    out.push_back(static_cast<std::uint32_t>(b));
    out.push_back(static_cast<std::uint32_t>(b >> 32));
}

}  // namespace

const std::string& FieldGraph::id() const {
    return impl_->id;
}
std::uint64_t FieldGraph::hash() const {
    return impl_->hash;
}
std::size_t FieldGraph::node_count() const {
    return impl_->nodes.size();
}
std::span<const std::string> FieldGraph::outputs() const {
    return impl_->output_names;
}

Result<FieldGraph> parse_field_graph(std::string_view text, std::string_view source_name) {
    const json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded()) {
        return make_error(ErrorCode::data_parse_failed,
                          std::format("{}: not valid JSON", source_name.empty() ? "field graph" : source_name),
                          "a field graph is a JSON object; see docs/guides/procgen.md");
    }
    return parse_field_graph_json(doc, source_name);
}

Result<FieldGraph> parse_field_graph_json(const json& doc, std::string_view source_name) {
    Errors err{source_name.empty() ? std::string("field graph") : std::string(source_name)};
    if (!doc.is_object()) {
        return err.graph(ErrorCode::procgen_graph_invalid, "not a JSON object");
    }
    const auto format = doc.find("veach_graph");
    if (format == doc.end() || !format->is_number_integer() || format->get<std::int64_t>() != field_graph_format) {
        return err.graph(ErrorCode::procgen_graph_invalid, "\"veach_graph\" must be 1",
                         std::format("this build reads field graph format {}", field_graph_format));
    }
    for (const auto& [key, value] : doc.items()) {
        if (key != "veach_graph" && key != "id" && key != "inputs" && key != "nodes" && key != "outputs") {
            return err.graph(ErrorCode::data_unknown_field, std::format("unknown field '{}'", key),
                             "a graph has veach_graph, id, inputs, nodes and outputs");
        }
    }
    auto impl = std::make_shared<FieldGraph::Impl>();
    const auto id = doc.find("id");
    if (id == doc.end() || !id->is_string() || id->get<std::string>().empty()) {
        return err.graph(ErrorCode::procgen_graph_invalid, "\"id\" must be a non-empty string",
                         "name the graph like \"mymod:terrain/canyons\"");
    }
    impl->id = id->get<std::string>();
    if (source_name.empty()) {
        err.source = std::format("field graph '{}'", impl->id);
    }

    // Inputs: name -> position kind.
    if (const auto in = doc.find("inputs"); in != doc.end()) {
        if (!in->is_object()) {
            return err.graph(ErrorCode::procgen_graph_invalid, "\"inputs\" must map names to position kinds");
        }
        for (const auto& [name, kind] : in->items()) {
            const std::string k = kind.is_string() ? kind.get<std::string>() : std::string();
            if (k != "p_planet" && k != "p_local") {
                return err.graph(ErrorCode::procgen_graph_invalid, std::format("input '{}': unknown kind", name),
                                 "an input is \"p_planet\" (unscaled body-fixed position, planet_m) or \"p_local\" "
                                 "(realized position, m)");
            }
            impl->inputs.emplace_back(name, k == "p_planet" ? PositionKind::p_planet : PositionKind::p_local);
        }
    }
    auto input_kind = [&](std::string_view name) -> std::optional<PositionKind> {
        for (const auto& [n, k] : impl->inputs) {
            if (n == name) {
                return k;
            }
        }
        return std::nullopt;
    };

    // Nodes.
    const auto nodes = doc.find("nodes");
    if (nodes == doc.end() || !nodes->is_array()) {
        return err.graph(ErrorCode::procgen_graph_invalid, "\"nodes\" must be an array");
    }
    std::map<std::string, std::size_t, std::less<>> by_id;
    for (const json& n : *nodes) {
        if (!n.is_object() || !n.contains("id") || !n["id"].is_string() || !n.contains("type") ||
            !n["type"].is_string()) {
            return err.graph(ErrorCode::procgen_graph_invalid, "every node needs a string \"id\" and \"type\"");
        }
        NodeIR node;
        node.id = n["id"].get<std::string>();
        for (const auto& [key, value] : n.items()) {
            if (key != "id" && key != "type" && key != "inputs" && key != "params") {
                return err.node(ErrorCode::data_unknown_field, node.id, std::format("unknown field '{}'", key),
                                "a node has id, type, inputs and params");
            }
        }
        if (by_id.contains(node.id) || input_kind(node.id)) {
            return err.node(ErrorCode::procgen_graph_invalid, node.id, "the id is used twice",
                            "node ids are stable names that seed the node; give each its own");
        }
        auto type = find_type(err, node.id, n["type"].get<std::string>());
        if (!type) {
            return std::unexpected(type.error());
        }
        node.type = *type;
        static const json empty = json::object();
        const json& in = n.contains("inputs") ? n["inputs"] : empty;
        if (!in.is_object()) {
            return err.node(ErrorCode::procgen_graph_invalid, node.id, "'inputs' must map ports to node ids");
        }
        for (const auto& [port, ref] : in.items()) {
            const auto& ports = node.type->ports;
            const auto slot = std::find(ports.begin(), ports.end(), port);
            if (slot == ports.end() || port.empty()) {
                return err.node(ErrorCode::data_unknown_field, node.id,
                                std::format("{}@{} has no input '{}'", node.type->name, node.type->version, port));
            }
            if (!ref.is_string()) {
                return err.node(ErrorCode::procgen_graph_invalid, node.id,
                                std::format("input '{}' must be a node id or a graph input", port));
            }
            node.inputs[static_cast<std::size_t>(slot - ports.begin())] = ref.get<std::string>();
        }
        if (auto ok = parse_params(err, n, node); !ok) {
            return std::unexpected(ok.error());
        }
        by_id[node.id] = impl->nodes.size();
        impl->nodes.push_back(std::move(node));
    }

    // Ports: every one connected, positions to graph inputs, scalars to nodes; lengths match their position's unit.
    for (NodeIR& node : impl->nodes) {
        for (std::size_t s = 0; s < node.type->ports.size() && !node.type->ports[s].empty(); ++s) {
            const std::string_view port = node.type->ports[s];
            const std::string& ref = node.inputs[s];
            const bool position = port == "p";
            if (ref.empty()) {
                return err.node(ErrorCode::procgen_graph_invalid, node.id,
                                std::format("input '{}' is not connected", port));
            }
            const std::optional<PositionKind> kind = input_kind(ref);
            if (position != kind.has_value() || (!position && !by_id.contains(ref))) {
                return err.node(ErrorCode::procgen_graph_invalid, node.id,
                                std::format("input '{}' refers to '{}'", port, ref),
                                position ? "connect a graph input (p_planet or p_local)" : "connect a node's output");
            }
            if (position) {
                node.position = *kind;
                if (node.type->op == Op::fbm && node.unit != unit_of(*kind)) {
                    return err.node(
                        ErrorCode::procgen_graph_invalid, node.id,
                        std::format("'wavelength' is in {} but its position '{}' is {}", unit_name(node.unit), ref,
                                    kind_name(*kind)),
                        std::format("{} lengths sample {}: write {{\"{}\": ...}}, or connect a {} input",
                                    unit_name(unit_of(*kind)), kind_name(*kind), unit_name(unit_of(*kind)),
                                    kind_name(node.position == PositionKind::p_planet ? PositionKind::p_local
                                                                                      : PositionKind::p_planet)));
                }
            }
        }
    }

    // Outputs, sorted by name (a JSON object is).
    const auto outputs = doc.find("outputs");
    if (outputs == doc.end() || !outputs->is_object() || outputs->empty()) {
        return err.graph(ErrorCode::procgen_graph_invalid, "\"outputs\" must map one or more names to node ids");
    }
    for (const auto& [name, ref] : outputs->items()) {
        if (!ref.is_string() || !by_id.contains(ref.get<std::string>())) {
            return err.graph(ErrorCode::procgen_graph_invalid,
                             std::format("output '{}' is {}, not a node", name, ref.dump()),
                             "an output names the node whose value it is");
        }
        impl->output_names.push_back(name);
        impl->output_nodes.push_back(by_id.find(ref.get<std::string>())->second);
    }

    // Depth first from each output: dependency order, cycles, dead nodes left out.
    std::vector<std::uint8_t> mark(impl->nodes.size(), 0);  // 0 new, 1 on the stack, 2 done
    std::string cycle;
    auto visit = [&](auto&& self, std::size_t i) -> void {
        if (mark[i] == 2 || !cycle.empty()) {
            return;
        }
        if (mark[i] == 1) {
            cycle = impl->nodes[i].id;
            return;
        }
        mark[i] = 1;
        for (const std::string& ref : impl->nodes[i].inputs) {
            if (const auto it = by_id.find(ref); it != by_id.end()) {
                self(self, it->second);
            }
        }
        mark[i] = 2;
        impl->order.push_back(i);
    };
    for (const std::size_t o : impl->output_nodes) {
        visit(visit, o);
    }
    if (!cycle.empty()) {
        return err.node(ErrorCode::procgen_graph_invalid, cycle, "is part of a cycle", "field graphs are acyclic");
    }
    impl->hash = hash_id(doc.dump());
    return FieldGraph(std::move(impl));
}

Result<FieldProgram> compile(const FieldGraph& graph, const BodyConstants& body) {
    const FieldGraph::Impl& g = graph.impl();
    const Errors err{std::format("field graph '{}'", g.id)};
    std::map<std::string, std::size_t, std::less<>> by_id;
    for (std::size_t i = 0; i < g.nodes.size(); ++i) {
        by_id[g.nodes[i].id] = i;
    }
    auto node_of = [&](const std::string& ref) -> std::optional<std::size_t> {
        const auto it = by_id.find(ref);
        return it == by_id.end() ? std::nullopt : std::optional(it->second);
    };

    // Uses of each node by reachable nodes; outputs keep their registers to the end.
    std::vector<std::uint32_t> uses(g.nodes.size(), 0);
    for (const std::size_t i : g.order) {
        for (const std::string& ref : g.nodes[i].inputs) {
            if (const auto n = node_of(ref)) {
                ++uses[*n];
            }
        }
    }
    std::vector<bool> pinned(g.nodes.size(), false);
    for (const std::size_t o : g.output_nodes) {
        pinned[o] = true;
    }

    FieldProgram program;
    program.graph_hash = g.hash;
    std::vector<std::uint32_t> reg(g.nodes.size(), 0);
    std::vector<std::uint32_t> free_regs;
    for (std::uint32_t r = max_registers; r-- > 0;) {
        free_regs.push_back(r);
    }
    for (const std::size_t i : g.order) {
        const NodeIR& node = g.nodes[i];
        if (free_regs.empty()) {
            return err.node(ErrorCode::procgen_graph_invalid, node.id,
                            std::format("needs more than {} live values", max_registers),
                            "split the graph, or reorder it so fewer intermediate values are alive at once");
        }
        const std::uint32_t dst = free_regs.back();
        free_regs.pop_back();
        program.registers = std::max(program.registers, max_registers - static_cast<std::uint32_t>(free_regs.size()));
        reg[i] = dst;
        std::array<std::uint32_t, 3> src{};
        for (std::size_t s = 0; s < 3; ++s) {
            if (const auto n = node_of(node.inputs[s])) {
                src[s] = reg[*n];
            }
        }

        std::vector<std::uint32_t> payload;
        const std::array<double, 5>& v = node.values;
        switch (node.type->op) {
            case Op::fbm: {
                const double wavelength = v[0];
                const auto octaves = static_cast<std::uint32_t>(v[1]);
                // Cube-face domain: the position is the unit direction, so the radius (in the position's unit) folds
                // into the frequency.
                const double radius = node.position == PositionKind::p_planet
                                          ? body.radius_planet_m
                                          : body.radius_planet_m / body.planet_m_per_m;
                payload.insert(payload.end(), {octaves, bits(radius / wavelength), bits(v[2]), bits(v[3]), bits(v[4])});
                // ADR 0052: the node's seed is hash(body_seed, hash_id(node_id)); each octave hashes its index into it.
                const std::uint64_t node_seed = veach::hash(body.seed, hash_id(node.id));
                for (std::uint32_t o = 0; o < octaves; ++o) {
                    const std::uint64_t h = veach::hash(node_seed, o);
                    payload.push_back(static_cast<std::uint32_t>(h ^ (h >> 32)));
                }
                // Lattice domain: cycles per realized meter for each octave, in double.
                double f = (node.position == PositionKind::p_planet ? body.planet_m_per_m : 1.0) / wavelength;
                for (std::uint32_t o = 0; o < octaves; ++o) {
                    push_double(payload, f);
                    f = f * v[2];
                }
                program.noise_octaves += octaves;
                break;
            }
            case Op::ridge: payload.push_back(bits(v[0])); break;
            case Op::remap:
            case Op::clamp: payload.insert(payload.end(), {bits(v[0]), bits(v[1])}); break;
            case Op::blend:
            case Op::add:
            case Op::mul: break;
        }
        program.words.insert(program.words.end(), {static_cast<std::uint32_t>(node.type->op), dst, src[0], src[1],
                                                   src[2], static_cast<std::uint32_t>(payload.size())});
        program.words.insert(program.words.end(), payload.begin(), payload.end());
        for (std::size_t s = 0; s < 3; ++s) {
            if (const auto n = node_of(node.inputs[s]); n && --uses[*n] == 0 && !pinned[*n]) {
                free_regs.push_back(reg[*n]);
            }
        }
    }
    for (std::size_t o = 0; o < g.output_names.size(); ++o) {
        program.outputs.push_back({g.output_names[o], reg[g.output_nodes[o]]});
    }
    return program;
}

}  // namespace veach::procgen
