#include <veach/script/script_asset.hpp>

#include <Luau/Compiler.h>

#include <format>

namespace veach {

Result<ScriptAsset> compile_script(std::string_view source, std::string_view path) {
    Luau::CompileOptions options;
    options.optimizationLevel = 1;
    options.debugLevel = 1;  // line info for errors
    std::string bytecode = Luau::compile(std::string(source), options);
    if (!bytecode.empty() && bytecode[0] == 0) {
        // Luau encodes compile errors as a zero byte followed by ":line: message".
        return make_error(ErrorCode::script_compile_failed, std::format("{}{}", path, bytecode.substr(1)),
                          "Fix the syntax error; run luau-analyze on the file for full diagnostics.");
    }
    return ScriptAsset{std::format("@{}", path), std::move(bytecode)};
}

Result<ScriptAsset> ScriptLoader::load(const LoadContext& ctx) const {
    return compile_script(ctx.bytes, ctx.path.generic_string());
}

}  // namespace veach
