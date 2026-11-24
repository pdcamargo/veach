#pragma once

// "Did you mean ...?" support for actionable errors.

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace veach {

/// Case-insensitive edit distance.
[[nodiscard]] std::size_t edit_distance(std::string_view a, std::string_view b);

/// The candidate closest to `needle`, if it is close enough to be a plausible typo.
[[nodiscard]] std::optional<std::string> closest_match(std::string_view needle,
                                                       std::span<const std::string> candidates);

/// " Did you mean 'X'?" or an empty string. Convenient for building Error::hint.
[[nodiscard]] std::string did_you_mean(std::string_view needle, std::span<const std::string> candidates);

}  // namespace veach
