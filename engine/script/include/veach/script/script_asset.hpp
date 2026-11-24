#pragma once

// A compiled Luau script. Loaded through the asset system by ScriptLoader, which
// compiles at load time so syntax errors surface with file and line before
// anything runs.

#include <veach/asset/loader.hpp>

#include <array>
#include <string>

namespace veach {

struct ScriptAsset {
    std::string chunk_name;  ///< "@scripts/hello.luau": how errors and tracebacks name the file.
    std::string bytecode;
};

class ScriptLoader final : public TypedAssetLoader<ScriptAsset> {
public:
    [[nodiscard]] std::string_view name() const override { return "luau-script"; }
    [[nodiscard]] std::span<const std::string_view> extensions() const override { return extensions_; }
    [[nodiscard]] Result<ScriptAsset> load(const LoadContext& ctx) const override;

private:
    static constexpr std::array<std::string_view, 1> extensions_{".luau"};
};

/// Compiles Luau source; fails with script.compile_failed.
[[nodiscard]] Result<ScriptAsset> compile_script(std::string_view source, std::string_view path);

}  // namespace veach
