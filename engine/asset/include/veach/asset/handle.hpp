#pragma once

#include <veach/core/ids.hpp>

#include <compare>

namespace veach {

/// A typed reference to an asset. Cheap to copy; holds only the stable AssetId,
/// so it stays valid across renames, moves and (later) hot reloads.
template <class T>
struct Handle {
    AssetId id;

    [[nodiscard]] constexpr bool is_valid() const { return id.is_valid(); }
    constexpr auto operator<=>(const Handle&) const = default;
};

}  // namespace veach
