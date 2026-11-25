#pragma once

// Compiled shader code, embedded in binaries by cmake/VeachShaders.cmake
// (ADR 0026). One ShaderLibrary per .slang file.

#include <cstdint>
#include <span>
#include <string_view>

namespace veach::rhi {

enum class ShaderStage : std::uint8_t { vertex, fragment, compute };

struct ShaderEntry {
    std::string_view name;  ///< Entry point name in the .slang file.
    ShaderStage stage = ShaderStage::compute;
    std::span<const std::uint8_t> dxil;  ///< DXIL for this entry (Windows builds; empty elsewhere).
};

struct ShaderLibrary {
    std::string_view name;                   ///< The .slang file's stem.
    std::span<const std::uint8_t> metallib;  ///< Metal library with every entry (macOS builds; empty elsewhere).
    std::span<const ShaderEntry> entries;

    [[nodiscard]] const ShaderEntry* find(std::string_view entry) const {
        for (const ShaderEntry& e : entries) {
            if (e.name == entry) {
                return &e;
            }
        }
        return nullptr;
    }
};

}  // namespace veach::rhi
