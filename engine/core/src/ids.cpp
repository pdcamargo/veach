#include <veach/core/ids.hpp>

#include <format>
#include <random>

namespace veach {

AssetId AssetId::generate() {
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    AssetId id{engine(), engine()};
    id.hi = (id.hi & ~0xF000ULL) | 0x4000ULL;                          // version 4
    id.lo = (id.lo & ~0xC000000000000000ULL) | 0x8000000000000000ULL;  // RFC 4122 variant
    return id;
}

std::optional<AssetId> AssetId::parse(std::string_view text) {
    if (text.size() != 36) {
        return std::nullopt;
    }
    AssetId id;
    int nibbles = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return std::nullopt;
            }
            continue;
        }
        std::uint64_t value = 0;
        if (c >= '0' && c <= '9') {
            value = static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value = static_cast<std::uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value = static_cast<std::uint64_t>(c - 'A' + 10);
        } else {
            return std::nullopt;
        }
        std::uint64_t& half = nibbles < 16 ? id.hi : id.lo;
        half = (half << 4) | value;
        ++nibbles;
    }
    return id;
}

std::string AssetId::to_string() const {
    return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}", hi >> 32, (hi >> 16) & 0xFFFF, hi & 0xFFFF, lo >> 48,
                       lo & 0xFFFFFFFFFFFFULL);
}

}  // namespace veach
