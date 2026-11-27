// Spike SP-03 (VCH-079), THROWAWAY: the cost of ADR 0046's Luau `Position` (three doubles in a tagged userdata)
// against Luau's native float32 `vector`.
//   build/release/tests/veach-bench-sp03-position [n=1000000] [repeats=5]
// Each case runs `n` operations in one Luau loop; prints the best of `repeats` runs in ns per operation, raw and
// minus the empty loop. The VM is set up like the engine's script runtime: interpreter only (no native codegen),
// compile optimization level 1, sandboxed globals. Findings: docs/research/spikes/sp-03-precision-frames.md.

#include <Luau/Compiler.h>
#include <lua.h>
#include <lualib.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr int position_tag = 7;

enum Atom : std::int16_t { atom_none = -1, atom_x, atom_y, atom_z, atom_relative, atom_length };

std::int16_t user_atom(lua_State*, const char* s, std::size_t l) {
    const std::string_view name(s, l);
    if (name == "x") {
        return atom_x;
    }
    if (name == "y") {
        return atom_y;
    }
    if (name == "z") {
        return atom_z;
    }
    if (name == "relative") {
        return atom_relative;
    }
    if (name == "length") {
        return atom_length;
    }
    return atom_none;
}

struct Position {
    double x, y, z;
};

Position* push_position(lua_State* L, double x, double y, double z) {
    auto* p = static_cast<Position*>(lua_newuserdatataggedwithmetatable(L, sizeof(Position), position_tag));
    *p = {x, y, z};
    return p;
}

const Position* to_position(lua_State* L, int index) {
    return static_cast<const Position*>(lua_touserdatatagged(L, index, position_tag));
}

const Position& check_position(lua_State* L, int index) {
    const Position* p = to_position(L, index);
    if (p == nullptr) {
        luaL_typeerrorL(L, index, "Position");
    }
    return *p;
}

int position_new(lua_State* L) {
    push_position(L, luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3));
    return 1;
}

// Position + vector -> Position
int position_add(lua_State* L) {
    const Position& p = check_position(L, 1);
    const float* v = luaL_checkvector(L, 2);
    push_position(L, p.x + static_cast<double>(v[0]), p.y + static_cast<double>(v[1]), p.z + static_cast<double>(v[2]));
    return 1;
}

// Position - vector -> Position; Position - Position -> vector (float32, computed in double)
int position_sub(lua_State* L) {
    const Position& a = check_position(L, 1);
    if (const Position* b = to_position(L, 2)) {
        lua_pushvector(L, static_cast<float>(a.x - b->x), static_cast<float>(a.y - b->y),
                       static_cast<float>(a.z - b->z));
        return 1;
    }
    const float* v = luaL_checkvector(L, 2);
    push_position(L, a.x - static_cast<double>(v[0]), a.y - static_cast<double>(v[1]), a.z - static_cast<double>(v[2]));
    return 1;
}

// Position * number -> Position (not in ADR 0046; measured because the spike brief asks for it)
int position_mul(lua_State* L) {
    const Position& p = check_position(L, 1);
    const double s = luaL_checknumber(L, 2);
    push_position(L, p.x * s, p.y * s, p.z * s);
    return 1;
}

int position_index(lua_State* L) {
    const Position& p = check_position(L, 1);
    int atom = atom_none;
    lua_tostringatom(L, 2, &atom);
    switch (atom) {
        case atom_x: lua_pushnumber(L, p.x); return 1;
        case atom_y: lua_pushnumber(L, p.y); return 1;
        case atom_z: lua_pushnumber(L, p.z); return 1;
        default: luaL_error(L, "Position has no field '%s'", luaL_checkstring(L, 2));
    }
}

int position_namecall(lua_State* L) {
    const Position& p = check_position(L, 1);
    int atom = atom_none;
    const char* name = lua_namecallatom(L, &atom);
    switch (atom) {
        case atom_relative: {  // p:relative(origin) -> vector: the float32 offset from origin, in double
            const Position& o = check_position(L, 2);
            lua_pushvector(L, static_cast<float>(p.x - o.x), static_cast<float>(p.y - o.y),
                           static_cast<float>(p.z - o.z));
            return 1;
        }
        case atom_length: lua_pushnumber(L, std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z)); return 1;
        default: luaL_error(L, "Position has no method '%s'", name ? name : "?");
    }
}

constexpr const char* script = R"(
local ITER = ...
local P = Position.new
local d = vector.create(0.001, 0.002, 0.003)
local s = 1.0000001
local bench = {}

function bench.empty(n)
	local x = 0
	for i = 1, n do x = i end
	return x
end

-- native vector (float32)
function bench.vec_add(n)
	local v = vector.create(1, 2, 3)
	for i = 1, n do v = v + d end
	return v
end
function bench.vec_sub(n)
	local a, b, r = vector.create(1, 2, 3), vector.create(4, 5, 6), nil
	for i = 1, n do r = a - b end
	return r
end
function bench.vec_mul(n)
	local v = vector.create(1, 2, 3)
	for i = 1, n do v = v * s end
	return v
end
function bench.vec_length(n)
	local v, l = vector.create(1, 2, 3), 0
	for i = 1, n do l = vector.magnitude(v) end
	return l
end
function bench.vec_relative(n)
	local v, o, r = vector.create(1, 2, 3), vector.create(0.5, 0.5, 0.5), nil
	for i = 1, n do r = v - o end
	return r
end
function bench.vec_integrate(n) -- pos = pos + vel * dt
	local pos, vel, dt = vector.create(1, 2, 3), vector.create(7, 8, 9), 1 / 60
	for i = 1, n do pos = pos + vel * dt end
	return pos
end

-- Position (three doubles in a userdata)
function bench.pos_add(n) -- Position + vector -> Position
	local p = P(1.496e11, 2, 3)
	for i = 1, n do p = p + d end
	return p
end
function bench.pos_sub_vec(n) -- Position - vector -> Position
	local p = P(1.496e11, 2, 3)
	for i = 1, n do p = p - d end
	return p
end
function bench.pos_sub(n) -- Position - Position -> vector
	local a, b, r = P(1.496e11, 2, 3), P(1.496e11 - 5, 1, 0), nil
	for i = 1, n do r = a - b end
	return r
end
function bench.pos_mul(n) -- Position * number -> Position
	local p = P(1, 2, 3)
	for i = 1, n do p = p * s end
	return p
end
function bench.pos_length(n)
	local p, l = P(1.496e11, 2, 3), 0
	for i = 1, n do l = p:length() end
	return l
end
function bench.pos_relative(n) -- p:relative(origin) -> vector
	local p, o, r = P(1.496e11, 2, 3), P(1.496e11 - 5, 1, 0), nil
	for i = 1, n do r = p:relative(o) end
	return r
end
function bench.pos_field(n)
	local p, x = P(1.496e11, 2, 3), 0
	for i = 1, n do x = p.x end
	return x
end
function bench.pos_integrate(n) -- pos = pos + vel * dt, vel a vector
	local pos, vel, dt = P(1.496e11, 2, 3), vector.create(7, 8, 9), 1 / 60
	for i = 1, n do pos = pos + vel * dt end
	return pos
end

-- Correctness at 1 AU: a 1 mm step survives (double ULP there is 3e-5 m; float32 would round it to 16 km).
local far = P(1.496e11, 0, 0)
local near = far + vector.create(0.001, 0, 0)
assert(math.abs((near - far).x - 0.001) < 1e-4, "Position - Position lost precision")
assert(math.abs(near.x - 1.496e11 - 0.001) < 1e-4, "Position + vector lost precision")
return bench
)";

const char* const cases[] = {"empty",        "vec_add",       "vec_sub",      "vec_mul",     "vec_length",
                             "vec_relative", "vec_integrate", "pos_add",      "pos_sub_vec", "pos_sub",
                             "pos_mul",      "pos_length",    "pos_relative", "pos_field",   "pos_integrate"};

}  // namespace

int main(int argc, char** argv) {
    const int n = argc > 1 ? std::atoi(argv[1]) : 1000000;
    const int repeats = argc > 2 ? std::atoi(argv[2]) : 5;

    for (const int level : {1, 2}) {
        lua_State* L = luaL_newstate();
        lua_callbacks(L)->useratom = user_atom;
        luaL_openlibs(L);

        lua_createtable(L, 0, 6);
        lua_pushcfunction(L, position_add, "__add");
        lua_setfield(L, -2, "__add");
        lua_pushcfunction(L, position_sub, "__sub");
        lua_setfield(L, -2, "__sub");
        lua_pushcfunction(L, position_mul, "__mul");
        lua_setfield(L, -2, "__mul");
        lua_pushcfunction(L, position_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, position_namecall, "__namecall");
        lua_setfield(L, -2, "__namecall");
        lua_pushstring(L, "Position");
        lua_setfield(L, -2, "__type");
        lua_setreadonly(L, -1, 1);
        lua_setuserdatametatable(L, position_tag);

        lua_createtable(L, 0, 1);
        lua_pushcfunction(L, position_new, "new");
        lua_setfield(L, -2, "new");
        lua_setreadonly(L, -1, 1);
        lua_setglobal(L, "Position");
        luaL_sandbox(L);

        lua_State* T = lua_newthread(L);
        luaL_sandboxthread(T);
        Luau::CompileOptions options;
        options.optimizationLevel = level;
        const std::string bytecode = Luau::compile(script, options);
        if (luau_load(T, "=sp03_position", bytecode.data(), bytecode.size(), 0) != 0) {
            std::fprintf(stderr, "load: %s\n", lua_tostring(T, -1));
            return 1;
        }
        lua_pushinteger(T, n);
        if (lua_pcall(T, 1, 1, 0) != 0) {
            std::fprintf(stderr, "run: %s\n", lua_tostring(T, -1));
            return 1;
        }
        std::printf("-- Luau -O%d, interpreter, %d operations per case, best of %d\n", level, n, repeats);
        double empty_ns = 0.0;
        for (const char* name : cases) {
            double best = 1e300;
            for (int r = 0; r < repeats; ++r) {
                lua_getfield(T, -1, name);
                lua_pushinteger(T, n);
                lua_gc(T, LUA_GCCOLLECT, 0);
                const auto start = std::chrono::steady_clock::now();
                if (lua_pcall(T, 1, 1, 0) != 0) {
                    std::fprintf(stderr, "%s: %s\n", name, lua_tostring(T, -1));
                    return 1;
                }
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                lua_pop(T, 1);
                best = ms < best ? ms : best;
            }
            const double ns = best * 1e6 / n;
            if (std::strcmp(name, "empty") == 0) {
                empty_ns = ns;
            }
            std::printf("%-14s %7.2f ns/op  (%6.2f ns/op minus the loop)  %8.2f ms per %d\n", name, ns, ns - empty_ns,
                        best, n);
        }
        lua_close(L);
    }
    return 0;
}
