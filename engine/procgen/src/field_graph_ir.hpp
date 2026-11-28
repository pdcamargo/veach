#pragma once

// The validated IR behind FieldGraph (private to engine/procgen).

#include <veach/procgen/field_graph.hpp>

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace veach::procgen {

enum class LengthUnit : std::uint8_t { planet_m, m };

struct NodeType {
    std::string_view name;
    std::uint32_t version;
    Op op;
    std::array<std::string_view, 3> ports;  ///< inputs in a, b, c order; "p" is a position
    std::array<std::string_view, 5> params;
};

/// One node with its parameters resolved to numbers (defaults applied, ranges folded where it needs no body).
struct NodeIR {
    std::string id;
    const NodeType* type = nullptr;
    std::array<std::string, 3> inputs;  ///< what each port is wired to: a node id or a graph input name
    std::array<double, 5> values{};     ///< per op: see parse_params() in field_graph.cpp
    LengthUnit unit = LengthUnit::planet_m;
    PositionKind position = PositionKind::p_planet;  ///< of the "p" port, if any
};

struct FieldGraph::Impl {
    std::string id;
    std::uint64_t hash = 0;
    std::vector<std::pair<std::string, PositionKind>> inputs;
    std::vector<NodeIR> nodes;
    std::vector<std::string> output_names;  ///< sorted
    std::vector<std::size_t> output_nodes;  ///< parallel to output_names
    std::vector<std::size_t> order;         ///< reachable nodes in dependency order, depth first from the outputs
};

/// Parses an already-decoded JSON document (the Luau builder's route).
[[nodiscard]] Result<FieldGraph> parse_field_graph_json(const nlohmann::json& doc, std::string_view source_name);

}  // namespace veach::procgen
