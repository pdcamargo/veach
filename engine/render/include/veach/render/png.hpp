#pragma once

// PNG files for screenshots and golden images (8-bit RGBA).

#include <veach/core/error.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace veach::render {

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;  ///< width * height * 4, rows top to bottom
};

[[nodiscard]] Result<> write_png(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                                 std::span<const std::uint8_t> rgba);
[[nodiscard]] Result<Image> read_png(const std::filesystem::path& path);
/// PNG file bytes in memory (for MCP screenshots).
[[nodiscard]] std::vector<std::uint8_t> encode_png(const Image& image);

}  // namespace veach::render
