#pragma once

// Universe addresses (docs/adr/0050-universe-addresses-and-seeds.md), spike SP-05 (VCH-081), throwaway.
//
// A SystemAddress is 64 bits, a layered octree. Cell edge at layer L is 10 ly · 2^L. Bit layout, high to low:
//
//   [63..61] layer (3)  | cell x (14 - L) | cell z (14 - L) | cell y (11 - L) | system index (13 + 3L) | body (9)
//
// The fields sum to 64 at every layer. The layer sits at a fixed position (the top) so decoding reads it first; the
// body index sits at the bottom so every body of a system shares the system's high bits and sorts next to it. Cell
// coordinates are unsigned offsets from the galaxy's corner; the galaxy's center is the middle cell of each axis.

#include <cstdint>
#include <optional>
#include <string>

namespace space::gen {

inline constexpr std::uint32_t address_layers = 8;
inline constexpr double cell_edge_ly_layer0 = 10.0;
inline constexpr std::uint32_t body_bits = 9;
inline constexpr std::uint32_t max_generated_body = 255;  ///< 1-255 generated; 256-511 reserved for mods.

constexpr std::uint32_t cell_bits_xz(std::uint32_t layer) {
    return 14u - layer;
}
constexpr std::uint32_t cell_bits_y(std::uint32_t layer) {
    return 11u - layer;
}
constexpr std::uint32_t system_bits(std::uint32_t layer) {
    return 13u + 3u * layer;
}

/// The decoded fields of a system address.
struct SystemCoords {
    std::uint32_t layer = 0;
    std::uint32_t x = 0, y = 0, z = 0;  ///< Cell, unsigned, at this layer's resolution.
    std::uint64_t system = 0;           ///< Index within the cell.
    friend bool operator==(const SystemCoords&, const SystemCoords&) = default;
};

struct BodyAddress;

struct SystemAddress {
    std::uint64_t value = 0;  ///< Body field always 0.

    /// Nullopt when a field does not fit its width at that layer.
    static std::optional<SystemAddress> encode(const SystemCoords& c);
    SystemCoords decode() const;
    BodyAddress body(std::uint32_t index) const;
    /// Cell origin in light-years relative to the galaxy's center (x, y, z).
    void cell_origin_ly(double out[3]) const;
    double cell_edge_ly() const;

    friend bool operator==(const SystemAddress&, const SystemAddress&) = default;
};

struct BodyAddress {
    std::uint64_t value = 0;
    SystemAddress system() const { return {value & ~((std::uint64_t{1} << body_bits) - 1u)}; }
    std::uint32_t index() const { return static_cast<std::uint32_t>(value & ((std::uint64_t{1} << body_bits) - 1u)); }
    friend bool operator==(const BodyAddress&, const BodyAddress&) = default;
};

/// Canonical text forms: "S:3f2a91c07e5b0000" and "S:3f2a91c07e5b0000/B:4".
std::string to_string(SystemAddress a);
std::string to_string(BodyAddress a);
std::optional<SystemAddress> parse_system_address(std::string_view text);

}  // namespace space::gen
