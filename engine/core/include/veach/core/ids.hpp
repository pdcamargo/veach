#pragma once

// Stable 128-bit identifiers, printed as UUIDs. AssetId is what .meta sidecars
// store; references between assets use the id, never the path, so renames and
// moves never break anything.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace veach {

struct AssetId {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;

    /// A new random (version 4) id. Not deterministic by design: ids are minted
    /// once, when a source asset is first seen, and then live in its .meta file.
    [[nodiscard]] static AssetId generate();
    /// A name-based (version 5, SHA-1) id under `parent`: the same parent and name always give the same id.
    /// Sub-assets use it: AssetId::derive(gltf id, "mesh/3") (ADR 0032).
    [[nodiscard]] static AssetId derive(const AssetId& parent, std::string_view name);
    /// Parses "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (case-insensitive).
    [[nodiscard]] static std::optional<AssetId> parse(std::string_view text);

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] constexpr bool is_valid() const { return hi != 0 || lo != 0; }

    constexpr auto operator<=>(const AssetId&) const = default;
};

}  // namespace veach

template <>
struct std::hash<veach::AssetId> {
    std::size_t operator()(const veach::AssetId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.hi ^ (id.lo * 0x9E3779B97F4A7C15ULL));
    }
};
