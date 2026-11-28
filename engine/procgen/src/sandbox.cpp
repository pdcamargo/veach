// The Luau procgen sandbox (docs/adr/0070-generator-sandbox-rules.md), from spike SP-07 (VCH-083).

#include <veach/procgen/sandbox.hpp>

#include <veach/core/assert.hpp>
#include <veach/core/dmath.hpp>
#include <veach/core/hash.hpp>

#include <Luau/Ast.h>
#include <Luau/Compiler.h>
#include <Luau/Parser.h>
#include <lua.h>
#include <lualib.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <vector>

namespace veach::procgen {

namespace {

// Every math.* that the stock VM computes with the platform libm (lmathlib.cpp, lbuiltins.cpp). The rest of `math` is
// exact in IEEE arithmetic (abs, floor, ceil, sqrt, fmod, frexp, ldexp, modf, round, min, max, clamp, sign) or plain
// arithmetic the VM is built to round the same everywhere (deg, rad, lerp, map).
constexpr std::array<const char*, 14> math_names{"sin", "cos", "tan",   "asin", "acos", "atan", "atan2",
                                                 "exp", "log", "log10", "pow",  "sinh", "cosh", "tanh"};
// The same names as "math.<name>", null-terminated, for CompileOptions::disabledBuiltins.
constexpr std::array<const char*, 15> disabled_builtins{
    "math.sin", "math.cos",   "math.tan", "math.asin", "math.acos", "math.atan", "math.atan2", "math.exp",
    "math.log", "math.log10", "math.pow", "math.sinh", "math.cosh", "math.tanh", nullptr};
static_assert(disabled_builtins.size() == math_names.size() + 1);

// Removed from the sandbox's globals and libraries: I/O, clocks, environment tricks, platform-dependent randomness.
constexpr std::array<const char*, 9> removed_globals{"os",     "debug",          "print",    "getfenv", "setfenv",
                                                     "gcinfo", "collectgarbage", "newproxy", "require"};
constexpr std::array<const char*, 3> removed_math{"random", "randomseed", "noise"};

constexpr int max_hash_words = 6;  // veach::hash takes 1 to 6 key words

/// Finds `^` and `^=`: libm pow, either folded by the compiler on the host or run by the VM.
struct PowFinder : Luau::AstVisitor {
    int line = -1;
    bool visit(Luau::AstExprBinary* node) override {
        if (node->op == Luau::AstExprBinary::Pow && line < 0) {
            line = static_cast<int>(node->location.begin.line) + 1;
        }
        return true;
    }
    bool visit(Luau::AstStatCompoundAssign* node) override {
        if (node->op == Luau::AstExprBinary::Pow && line < 0) {
            line = static_cast<int>(node->location.begin.line) + 1;
        }
        return true;
    }
};

}  // namespace

std::span<const char* const> dmath_backed_math_functions() {
    return math_names;
}

struct Sandbox::Impl {
    using Clock = std::chrono::steady_clock;

    struct Hook {
        std::string chunk_name;
        std::string bytecode;
    };

    SandboxOptions options;
    lua_State* L = nullptr;
    std::vector<Hook> hooks;
    HookStats stats;

    // Per invocation.
    std::uint64_t seed = 0;
    Clock::time_point deadline;
    enum class Exceeded { none, safepoints, wall, memory } exceeded = Exceeded::none;

    std::size_t allocated = 0;
    bool enforce_memory = false;  // off while the sandbox builds its own libraries

    explicit Impl(SandboxOptions o) : options(o) { open(); }
    ~Impl() {
        if (L != nullptr) {
            lua_close(L);
        }
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    static Impl& self(lua_State* S) { return *static_cast<Impl*>(lua_callbacks(S)->userdata); }

    // ---- VM callbacks ----------------------------------------------------------

    static void* allocate(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
        Impl& s = *static_cast<Impl*>(ud);
        if (nsize == 0) {
            std::free(ptr);
            s.allocated -= osize;
            return nullptr;
        }
        if (s.enforce_memory && nsize > osize && s.allocated - osize + nsize > s.options.memory_budget_bytes) {
            s.exceeded = Exceeded::memory;
            return nullptr;  // Luau raises a memory error
        }
        void* out = std::realloc(ptr, nsize);
        if (out != nullptr) {
            s.allocated = s.allocated - osize + nsize;
        }
        return out;
    }

    static void interrupt(lua_State* S, int gc) {
        if (gc >= 0) {
            return;  // a GC step, not a safepoint of the script
        }
        Impl& s = self(S);
        ++s.stats.safepoints;
        bool over = false;
        if (s.options.safepoint_budget > 0 && s.stats.safepoints > s.options.safepoint_budget) {
            s.exceeded = Exceeded::safepoints;
            over = true;
        } else if (s.options.wall_budget_ms > 0 && (s.stats.safepoints & 1023u) == 0 && Clock::now() > s.deadline) {
            s.exceeded = Exceeded::wall;
            over = true;
        }
        if (over) {
            // Raised again at every later safepoint, so a hook can't pcall its way past the budget.
            lua_rawcheckstack(S, 1);
            luaL_error(S, "procgen budget exceeded");
        }
    }

    // ---- math, rebound to dmath ------------------------------------------------

    template <double (*F)(double)>
    static int unary(lua_State* S) {
        const double x = luaL_checknumber(S, 1);
        ++self(S).stats.dmath_calls;
        lua_pushnumber(S, F(x));
        return 1;
    }

    template <double (*F)(double, double)>
    static int binary(lua_State* S) {
        const double a = luaL_checknumber(S, 1);
        const double b = luaL_checknumber(S, 2);
        ++self(S).stats.dmath_calls;
        lua_pushnumber(S, F(a, b));
        return 1;
    }

    // Same contract as Luau's math.log: log(x), or log base b, with exact paths for 2 and 10.
    static int log_any(lua_State* S) {
        const double x = luaL_checknumber(S, 1);
        ++self(S).stats.dmath_calls;
        double r = 0;
        if (lua_isnoneornil(S, 2)) {
            r = dmath::log(x);
        } else {
            const double base = luaL_checknumber(S, 2);
            r = base == 2.0 ? dmath::log2(x) : base == 10.0 ? dmath::log10(x) : dmath::log(x) / dmath::log(base);
        }
        lua_pushnumber(S, r);
        return 1;
    }

    static lua_CFunction math_function(std::string_view name) {
        constexpr std::array<std::pair<std::string_view, lua_CFunction>, 14> table{{
            {"sin", unary<&dmath::sin>},
            {"cos", unary<&dmath::cos>},
            {"tan", unary<&dmath::tan>},
            {"asin", unary<&dmath::asin>},
            {"acos", unary<&dmath::acos>},
            {"atan", unary<&dmath::atan>},
            {"atan2", binary<&dmath::atan2>},
            {"exp", unary<&dmath::exp>},
            {"log", log_any},
            {"log10", unary<&dmath::log10>},
            {"pow", binary<&dmath::pow>},
            {"sinh", unary<&dmath::sinh>},
            {"cosh", unary<&dmath::cosh>},
            {"tanh", unary<&dmath::tanh>},
        }};
        for (const auto& [n, fn] : table) {
            if (n == name) {
                return fn;
            }
        }
        return nullptr;
    }

    // ---- ctx -------------------------------------------------------------------

    /// ctx.random(a, b, ...): unit_double(hash(seed, a, b, ...)), 1 to 6 integer words (|w| < 2^53).
    static int ctx_random(lua_State* S) {
        Impl& s = self(S);
        const int n = lua_gettop(S);
        if (n < 1 || n > max_hash_words) {
            luaL_error(S, "ctx.random takes 1 to %d integer key words, got %d", max_hash_words, n);
        }
        std::uint64_t h = mix64(s.seed ^ hash_seed_salt);
        for (int i = 1; i <= n; ++i) {
            const double w = luaL_checknumber(S, i);
            if (!(std::abs(w) < 0x1p53) || w != std::floor(w)) {
                luaL_error(S, "ctx.random key word %d is %.17g; key words are integers below 2^53", i, w);
            }
            h = detail::hash_absorb(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(w)));
        }
        lua_pushnumber(S, unit_double(h));
        return 1;
    }

    // ---- setup -----------------------------------------------------------------

    void open() {
        L = lua_newstate(allocate, this);
        VEACH_ASSERT(L != nullptr, "procgen sandbox: lua_newstate failed");
        lua_callbacks(L)->userdata = this;
        if (options.safepoint_budget > 0 || options.wall_budget_ms > 0) {
            lua_callbacks(L)->interrupt = interrupt;
        }
        luaL_openlibs(L);

        for (const char* name : removed_globals) {
            lua_pushnil(L);
            lua_setglobal(L, name);
        }
        lua_getglobal(L, "math");
        for (const char* name : math_names) {
            lua_pushcfunction(L, math_function(name), name);
            lua_setfield(L, -2, name);
        }
        for (const char* name : removed_math) {
            lua_pushnil(L);
            lua_setfield(L, -2, name);
        }
        lua_pop(L, 1);
        lua_getglobal(L, "vector");
        lua_pushnil(L);
        lua_setfield(L, -2, "angle");  // atan2 from libm, in double
        lua_pop(L, 1);

        luaL_sandbox(L);  // libraries and globals read-only, safe environment on
        enforce_memory = true;
    }

    Result<HookId> load(std::string_view source, std::string_view chunk_name) {
        const std::string name(chunk_name);
        {
            Luau::Allocator allocator;
            Luau::AstNameTable names(allocator);
            const Luau::ParseResult parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator);
            if (parsed.errors.empty()) {
                PowFinder pow;
                parsed.root->visit(&pow);
                if (pow.line >= 0) {
                    return make_error(
                        ErrorCode::script_compile_failed,
                        std::format("{}:{}: the ^ operator is not allowed in generator hooks", name, pow.line),
                        "^ is the platform's libm pow, so its result differs between machines; write "
                        "math.pow(x, y), which is canonical (veach::dmath), or x * x for a square.");
                }
            }
        }
        Luau::CompileOptions compile;
        compile.optimizationLevel = options.optimization_level;
        compile.debugLevel = 1;  // line info for errors
        if (options.close_fastcall_bypass) {
            compile.disabledBuiltins = disabled_builtins.data();
        }
        std::string bytecode = Luau::compile(std::string(source), compile);
        if (!bytecode.empty() && bytecode[0] == 0) {
            return make_error(ErrorCode::script_compile_failed, std::format("{}{}", name, bytecode.substr(1)),
                              "Fix the syntax error; run luau-analyze on the file for full diagnostics.");
        }
        hooks.push_back({std::format("@{}", name), std::move(bytecode)});
        const auto id = static_cast<HookId>(hooks.size() - 1);
        // Validate once: the chunk must return a function.
        if (auto ran = run(id, HookCall{}, /*validate_only=*/true); !ran) {
            hooks.pop_back();
            return std::unexpected(ran.error());
        }
        return id;
    }

    Error failure(const Hook& hook, std::string message) const {
        const std::string chunk = hook.chunk_name.substr(1);
        switch (exceeded) {
            case Exceeded::safepoints:
                return Error{
                    ErrorCode::procgen_budget_exceeded,
                    std::format("generator hook '{}' passed its budget of {} safepoints (loop iterations, calls "
                                "and returns): {}",
                                chunk, options.safepoint_budget, message),
                    "Look for a loop that never ends. If the work is genuinely that large, split it into "
                    "several calls (per cube face or per row) or raise SandboxOptions::safepoint_budget; the "
                    "budget is deterministic, so a hook that fits here fits on every machine."};
            case Exceeded::wall:
                return Error{ErrorCode::procgen_budget_exceeded,
                             std::format("generator hook '{}' ran past its wall-clock budget of {} ms: {}", chunk,
                                         options.wall_budget_ms, message),
                             "Look for a loop that never ends, or split the work into several calls. The wall-clock "
                             "budget is a hang guard and differs between machines; size the work by the safepoint "
                             "budget instead."};
            case Exceeded::memory:
                return Error{ErrorCode::procgen_budget_exceeded,
                             std::format("generator hook '{}' needed more than its memory budget of {} bytes: {}",
                                         chunk, options.memory_budget_bytes, message),
                             "Look for a table that grows without end. Write results into the output buffers instead "
                             "of tables, or raise SandboxOptions::memory_budget_bytes."};
            case Exceeded::none: break;
        }
        return Error{ErrorCode::script_runtime_error, std::format("generator hook '{}': {}", chunk, message),
                     "The hook raised an error; its outputs were discarded. Fix the error at the line shown."};
    }

    Result<> run(HookId id, const HookCall& call, bool validate_only = false) {
        if (id >= hooks.size()) {
            return make_error(ErrorCode::internal, std::format("unknown generator hook {}", id));
        }
        const Hook& hook = hooks[id];
        stats = {};
        exceeded = Exceeded::none;
        seed = call.seed;
        if (options.wall_budget_ms > 0) {
            deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                          std::chrono::duration<double, std::milli>(options.wall_budget_ms));
        }

        const int top = lua_gettop(L);
        // A fresh sandboxed thread and a fresh closure per call: no state survives between invocations.
        lua_State* T = lua_newthread(L);
        luaL_sandboxthread(T);
        if (luau_load(T, hook.chunk_name.c_str(), hook.bytecode.data(), hook.bytecode.size(), 0) != 0) {
            Error e{ErrorCode::script_compile_failed, lua_tostring(T, -1) ? lua_tostring(T, -1) : "load failed", {}};
            lua_settop(L, top);
            return std::unexpected(std::move(e));
        }
        if (lua_pcall(T, 0, 1, 0) != LUA_OK) {
            Error e = failure(hook, lua_tostring(T, -1) ? lua_tostring(T, -1) : "error");
            lua_settop(L, top);
            return std::unexpected(std::move(e));
        }
        if (lua_type(T, -1) != LUA_TFUNCTION) {
            lua_settop(L, top);
            return make_error(ErrorCode::script_runtime_error,
                              std::format("generator hook '{}' returned a {}, not a function",
                                          hook.chunk_name.substr(1), lua_typename(T, lua_type(T, -1))),
                              "End the hook's file with `return function(ctx, inputs, outputs) ... end`.");
        }
        if (validate_only) {
            lua_settop(L, top);
            return {};
        }

        // ctx = { params = {...}, random = fn }, inputs = { name = buffer }, outputs = { name = buffer }. Built
        // outside the memory budget: an allocation failure here would be unprotected.
        enforce_memory = false;
        lua_createtable(T, 0, 2);
        lua_createtable(T, 0, static_cast<int>(call.params.size()));
        for (const HookParam& p : call.params) {
            lua_pushlstring(T, p.name.data(), p.name.size());
            lua_pushnumber(T, p.value);
            lua_rawset(T, -3);
        }
        lua_setreadonly(T, -1, 1);
        lua_setfield(T, -2, "params");
        lua_pushcfunction(T, ctx_random, "random");
        lua_setfield(T, -2, "random");
        lua_setreadonly(T, -1, 1);

        lua_createtable(T, 0, static_cast<int>(call.inputs.size()));
        for (const HookInput& in : call.inputs) {
            lua_pushlstring(T, in.name.data(), in.name.size());
            void* data = lua_newbuffer(T, in.bytes.size());
            if (!in.bytes.empty()) {
                std::memcpy(data, in.bytes.data(), in.bytes.size());
            }
            lua_rawset(T, -3);
        }
        lua_setreadonly(T, -1, 1);

        lua_createtable(T, 0, static_cast<int>(call.outputs.size()));
        for (const HookOutput& out : call.outputs) {
            lua_pushlstring(T, out.name.data(), out.name.size());
            lua_newbuffer(T, out.bytes.size());  // zeroed
            lua_rawset(T, -3);
        }
        lua_setreadonly(T, -1, 1);
        lua_pushvalue(T, -1);
        const int outputs = lua_gettop(T);  // a copy below the call, read back afterwards
        lua_insert(T, outputs - 4);

        enforce_memory = true;
        if (lua_pcall(T, 3, 0, 0) != LUA_OK) {
            Error e = failure(hook, lua_tostring(T, -1) ? lua_tostring(T, -1) : "error");
            lua_settop(L, top);
            return std::unexpected(std::move(e));
        }
        for (const HookOutput& out : call.outputs) {
            lua_pushlstring(T, out.name.data(), out.name.size());
            lua_rawget(T, -2);
            std::size_t len = 0;
            const void* data = lua_tobuffer(T, -1, &len);
            if (data != nullptr && len == out.bytes.size() && len > 0) {
                std::memcpy(out.bytes.data(), data, len);
            }
            lua_pop(T, 1);
        }
        lua_settop(L, top);
        return {};
    }
};

Sandbox::Sandbox(SandboxOptions options) : impl_(std::make_unique<Impl>(options)) {}
Sandbox::~Sandbox() = default;

Result<Sandbox::HookId> Sandbox::load(std::string_view source, std::string_view chunk_name) {
    return impl_->load(source, chunk_name);
}

Result<> Sandbox::run(HookId hook, const HookCall& call) {
    return impl_->run(hook, call);
}

const HookStats& Sandbox::last_stats() const {
    return impl_->stats;
}

const SandboxOptions& Sandbox::options() const {
    return impl_->options;
}

}  // namespace veach::procgen
