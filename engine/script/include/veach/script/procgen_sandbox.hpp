#pragma once

// The Luau procgen sandbox (docs/adr/0052-procgen-runtime.md): a VM for generator hooks, separate from the gameplay
// ScriptRuntime. Spike SP-07 (VCH-083), throwaway: docs/research/spikes/sp-07-luau-generator.md. It moves to
// engine/procgen when that area exists (docs/adr/0065-engine-areas-for-the-toolbox.md).
//
// What a hook sees: the standard libraries minus os, debug, print, getfenv/setfenv, gcinfo, collectgarbage and
// newproxy. `math` is canonical (docs/adr/0043-determinism-tiers-and-strict-floating-point.md): every libm-backed
// function is veach::dmath, and the script compiles with Luau's disabledBuiltins listing them, so `math.sin(x)` can
// neither take the VM's fastcall path to the platform libm nor be constant-folded by the compiler's host libm.
// math.random, math.randomseed, math.noise and vector.angle are gone; the `^` operator is a compile error (it is
// libm pow, folded at compile time or run by the VM). Randomness comes only from ctx.random(...), veach::hash keyed by
// the invocation's seed. Native codegen is never used: hooks run interpreted.
//
// A hook is a chunk that returns one function `(ctx, inputs, outputs)`. Each call loads the chunk afresh into a new
// sandboxed thread, so nothing (globals, upvalues) survives from one invocation to the next and results never depend
// on call order. One Sandbox per worker thread; a Sandbox is not thread-safe.

#include <veach/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace veach::procgen {

struct SandboxOptions {
    /// Compile hooks with disabledBuiltins for the libm-backed math functions. False only to reproduce the fastcall
    /// bypass (SP-07): `math` is still rebound to dmath, and the VM ignores it.
    bool close_fastcall_bypass = true;
    /// Interrupt safepoints (loop back edges, calls and returns) one invocation may pass. Deterministic: a hook that
    /// fits on one machine fits on all of them. 0 is unlimited; with wall_budget_ms 0 too, no interrupt callback is
    /// set.
    std::uint64_t safepoint_budget = 100'000'000;
    /// Wall-clock safety net, checked every 1024 safepoints; 0 is off. Not deterministic: a slow machine can fail
    /// where a fast one passes, so it only guards against hangs, never decides a result.
    double wall_budget_ms = 0;
    /// Bytes the VM may hold at once (all hooks of this sandbox together).
    std::size_t memory_budget_bytes = std::size_t{256} << 20;
    /// Luau optimization level (2 inlines and folds builtins; the disabled ones are not folded).
    int optimization_level = 2;
};

struct HookParam {
    std::string_view name;
    double value = 0;
};

/// A named byte buffer: inputs are copied into a Luau `buffer` before the call, outputs are zeroed buffers of this
/// size copied back after a successful call.
struct HookInput {
    std::string_view name;
    std::span<const std::byte> bytes;
};
struct HookOutput {
    std::string_view name;
    std::span<std::byte> bytes;
};

struct HookCall {
    std::uint64_t seed = 0;  ///< ctx.random(a, b, ...) is unit_double(hash(seed, a, b, ...)).
    std::span<const HookParam> params;
    std::span<const HookInput> inputs;
    std::span<const HookOutput> outputs;
};

struct HookStats {
    std::uint64_t safepoints = 0;   ///< Interrupt safepoints of the last call (0 with safepoint_budget 0).
    std::uint64_t dmath_calls = 0;  ///< Calls into the dmath-backed `math` functions during the last call.
};

class Sandbox {
public:
    using HookId = std::uint32_t;

    explicit Sandbox(SandboxOptions options = {});
    ~Sandbox();
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    /// Compiles a hook. Fails with script.compile_failed (syntax, or `^`).
    [[nodiscard]] Result<HookId> load(std::string_view source, std::string_view chunk_name);
    /// Runs a hook once. Fails with procgen.budget_exceeded or script.runtime_error; outputs are untouched on failure.
    [[nodiscard]] Result<> run(HookId hook, const HookCall& call);

    [[nodiscard]] const HookStats& last_stats() const;
    [[nodiscard]] const SandboxOptions& options() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// The `math` functions rebound to veach::dmath and listed in disabledBuiltins.
[[nodiscard]] std::span<const char* const> dmath_backed_math_functions();

}  // namespace veach::procgen
