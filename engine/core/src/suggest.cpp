#include <veach/core/suggest.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <vector>

namespace veach {

std::size_t edit_distance(std::string_view a, std::string_view b) {
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        row[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t above = row[j];
            const std::size_t cost = lower(a[i - 1]) == lower(b[j - 1]) ? 0 : 1;
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + cost});
            diagonal = above;
        }
    }
    return row[b.size()];
}

std::optional<std::string> closest_match(std::string_view needle, std::span<const std::string> candidates) {
    const std::size_t max_distance = std::max<std::size_t>(2, needle.size() / 3);
    std::optional<std::string> best;
    std::size_t best_distance = max_distance + 1;
    for (const std::string& candidate : candidates) {
        const std::size_t d = edit_distance(needle, candidate);
        if (d < best_distance) {
            best_distance = d;
            best = candidate;
        }
    }
    return best;
}

std::string did_you_mean(std::string_view needle, std::span<const std::string> candidates) {
    if (auto match = closest_match(needle, candidates)) {
        return std::format("Did you mean '{}'?", *match);
    }
    return {};
}

}  // namespace veach
