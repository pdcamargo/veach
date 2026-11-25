#pragma once

// Compiled shader code, embedded in binaries by cmake/VeachShaders.cmake
// (ADR 0026). One ShaderLibrary per .slang file.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
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

/// One field of a shader's parameter block, as Slang lays it out (VCH-041).
struct ParamField {
    std::string_view name;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

/// Generated per shader file as `<namespace>::layout::<stem>` by cmake/VeachShaders.cmake.
struct ParamLayout {
    std::string_view source;
    std::span<const ParamField> fields;
    std::uint32_t size = 0;
};

/// Never defined: calling it during constant evaluation turns a layout mismatch into a compile error whose
/// diagnostic shows the reason passed in.
void parameter_layout_mismatch(const char* reason);

/// True if the C++ struct T (described field by field with VEACH_PARAM) matches the shader's layout:
/// same fields in the same order, same offsets and sizes, and a size no smaller than the block.
///   static_assert(veach::rhi::parameters_match<TraceParams>(lab_shaders::layout::triangle_rt,
///       {VEACH_PARAM(TraceParams, output), VEACH_PARAM(TraceParams, scene), ...}));
template <class T>
consteval bool parameters_match(const ParamLayout& shader, std::initializer_list<ParamField> cpp) {
    if (cpp.size() != shader.fields.size()) {
        parameter_layout_mismatch("the C++ struct and the shader's Params have a different number of fields");
    }
    std::size_t i = 0;
    for (const ParamField& field : cpp) {
        const ParamField& expected = shader.fields[i++];
        if (field.name != expected.name) {
            parameter_layout_mismatch("field names or their order differ from the shader's Params");
        }
        if (field.offset != expected.offset) {
            parameter_layout_mismatch("a field's offset differs from the shader's Params");
        }
        if (field.size != expected.size) {
            parameter_layout_mismatch("a field's size differs from the shader's Params");
        }
    }
    if (sizeof(T) < shader.size) {
        parameter_layout_mismatch("the C++ struct is smaller than the shader's Params");
    }
    return true;
}

}  // namespace veach::rhi

/// Describes one member of a parameter struct for veach::rhi::parameters_match.
#define VEACH_PARAM(Type, field)                                                                                   \
    ::veach::rhi::ParamField {                                                                                     \
        #field, static_cast<std::uint32_t>(offsetof(Type, field)), static_cast<std::uint32_t>(sizeof(Type::field)) \
    }
