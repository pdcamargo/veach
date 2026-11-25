#include <veach/core/ids.hpp>

#include <array>
#include <format>
#include <random>
#include <span>
#include <vector>

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

namespace {

/// SHA-1 (FIPS 180-4), only for name-based UUIDs.
std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> message) {
    std::array<std::uint32_t, 5> h{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<std::uint8_t> data(message.begin(), message.end());
    const std::uint64_t bits = std::uint64_t{message.size()} * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56) {
        data.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        data.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
    }
    const auto rotl = [](std::uint32_t v, int n) {
        return (v << n) | (v >> (32 - n));
    };
    for (std::size_t chunk = 0; chunk < data.size(); chunk += 64) {
        std::array<std::uint32_t, 80> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t{data[chunk + 4 * i]} << 24) | (std::uint32_t{data[chunk + 4 * i + 1]} << 16) |
                   (std::uint32_t{data[chunk + 4 * i + 2]} << 8) | std::uint32_t{data[chunk + 4 * i + 3]};
        }
        for (std::size_t i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::array<std::uint8_t, 20> out{};
    for (std::size_t i = 0; i < 5; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * j));
        }
    }
    return out;
}

}  // namespace

AssetId AssetId::derive(const AssetId& parent, std::string_view name) {
    // RFC 9562 section 5.5: SHA-1 over the namespace id's bytes (big-endian) and the name.
    std::vector<std::uint8_t> message;
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<std::uint8_t>(parent.hi >> (i * 8)));
    }
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<std::uint8_t>(parent.lo >> (i * 8)));
    }
    message.insert(message.end(), name.begin(), name.end());
    const std::array<std::uint8_t, 20> digest = sha1(message);
    AssetId id;
    for (std::size_t i = 0; i < 8; ++i) {
        id.hi = (id.hi << 8) | digest[i];
        id.lo = (id.lo << 8) | digest[8 + i];
    }
    id.hi = (id.hi & ~0xF000ULL) | 0x5000ULL;              // version 5
    id.lo = (id.lo & ~(0xC0ULL << 56)) | (0x80ULL << 56);  // RFC 9562 variant
    return id;
}

}  // namespace veach
