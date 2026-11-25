#pragma once

// The Luau runtime (ADR 0006, docs/specs/scripting-luau-api-v0/spec.md).
//
// Scripts see only the curated `@veach` module, never raw Flecs. Every script
// invocation (running a script's top level, or one run of a script system) is
// a transaction: its world writes go to a CommandBuffer that is applied if the
// invocation returns normally and discarded if it raises an error.
// Systems receive whole query results in one call; the per-entity loop runs
// inside Luau.

#include <veach/core/error.hpp>
#include <veach/ecs/flecs.hpp>
#include <veach/script/script_asset.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace veach {

class ScriptRuntime {
public:
    explicit ScriptRuntime(World& world);
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;
    ScriptRuntime(ScriptRuntime&&) = delete;
    ScriptRuntime& operator=(ScriptRuntime&&) = delete;

    /// Runs a script's top level in its own sandboxed environment. Components and
    /// systems it defines, and its world writes, take effect only if it succeeds.
    [[nodiscard]] Result<> execute(const ScriptAsset& script);

    /// Hot reload (docs/specs/hot-reload/spec.md): runs a new version of a script that already ran, as if fresh.
    /// If it succeeds, the old version's systems are replaced (by name) and the entities its top level spawned
    /// are despawned, then the new top level's writes apply. If it fails, nothing changes. A script that never
    /// ran is left alone.
    [[nodiscard]] Result<> reload(const ScriptAsset& script);

    /// Runs Luau source directly (tests, tools, future MCP eval). Same transaction rules.
    [[nodiscard]] Result<> execute_source(std::string_view source, std::string_view chunk_name);

    /// Like execute_source, and returns the chunk's return values as strings (tostring). Used by MCP `eval`.
    [[nodiscard]] Result<std::vector<std::string>> evaluate(std::string_view source, std::string_view chunk_name);

    /// Removes every script system from the world. Call before the world is destroyed.
    void shutdown();

    [[nodiscard]] std::size_t system_count() const;
    [[nodiscard]] std::size_t component_count() const;

    /// Names of the functions in the `@veach` module, for type-generation tests.
    [[nodiscard]] static std::span<const std::string_view> api_names();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace veach
