#include <veach/script/script_runtime.hpp>

#include <veach/core/assert.hpp>
#include <veach/core/log.hpp>
#include <veach/core/math.hpp>
#include <veach/core/profile.hpp>
#include <veach/core/suggest.hpp>
#include <veach/ecs/command_buffer.hpp>
#include <veach/ecs/components.hpp>
#include <veach/ecs/frames.hpp>
#include <veach/ecs/phases.hpp>
#include <veach/ecs/reflection.hpp>
#include <veach/platform/input.hpp>

#include "marshal.hpp"
#include "position.hpp"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

namespace veach {

namespace {

using script::entity_tag;
constexpr std::size_t max_query_components = 4;

constexpr std::array<std::string_view, 18> api_function_names{
    "component", "query", "system", "spawn",  "log",      "time", "components", "f32",      "f64",
    "i32",       "u32",   "vec3",   "entity", "optional", "quat", "input",      "position", "frames",
};

/// Explicit field types: veach.f32(...) and friends return a marker the
/// type checker sees as a plain value (number, vector, Entity?, T?).
struct TypeMarkerKind {
    const char* name;
    bool has_default;  ///< accepts an optional default argument
    int lua_type;      ///< expected type of that default
};
constexpr std::array<TypeMarkerKind, 6> type_markers{{
    {"f32", true, LUA_TNUMBER},
    {"f64", true, LUA_TNUMBER},
    {"i32", true, LUA_TNUMBER},
    {"u32", true, LUA_TNUMBER},
    {"vec3", true, LUA_TVECTOR},
    {"entity", false, LUA_TNIL},
}};

// Runs a system's callback over a whole batch. The per-entity loop lives here,
// in Luau, so C++ calls into the VM once per system run, never once per entity.
constexpr std::string_view dispatcher_source = R"(
return function(fn, entities, a, b, c, d)
    local n = #entities
    if d ~= nil then
        for i = 1, n do fn(entities[i], a[i], b[i], c[i], d[i]) end
    elseif c ~= nil then
        for i = 1, n do fn(entities[i], a[i], b[i], c[i]) end
    elseif b ~= nil then
        for i = 1, n do fn(entities[i], a[i], b[i]) end
    else
        for i = 1, n do fn(entities[i], a[i]) end
    end
end
)";

struct ComponentInfo {
    std::string name;
    ecs::entity_t id = 0;
    std::vector<FieldInfo> fields;
    bool script_defined = false;
    void* defaults = nullptr;  ///< Owned default value (ecs_ptr_new), copied before each set.
    const script::TypePlan* plan = nullptr;
    std::size_t size = 0;
    int table_ref = 0;  ///< The one Component table scripts see for this component.
};

struct ScriptSystem {
    std::string name;
    std::string owner;  ///< chunk name of the script that defined it
    PhaseId phase = PhaseId::simulation;
    std::vector<const ComponentInfo*> components;
    int fn_ref = 0;
    ecs::entity_t entity = 0;
};

using script::raise;

}  // namespace

struct ScriptRuntime::Impl {
    World world;
    lua_State* L = nullptr;
    std::unordered_map<std::string, std::unique_ptr<ComponentInfo>> components;
    std::vector<std::unique_ptr<ScriptSystem>> systems;
    std::vector<std::unique_ptr<ScriptSystem>> pending_systems;
    std::vector<int> thread_refs;
    CommandBuffer* active = nullptr;  ///< Buffer of the invocation currently running.
    bool loading = false;             ///< A script's top level is running.
    std::string reloading;            ///< Chunk being reloaded: its own systems' names may be reused.
    /// Per script that ran: the entities its top level spawned (despawned when it reloads).
    std::unordered_map<std::string, std::vector<ecs::entity_t>> spawned_by;
    int dispatcher_ref = 0;
    int entity_metatable_ref = 0;
    int veach_ref = 0;
    int components_ref = 0;
    int type_marker_ref = 0;  ///< metatable identifying veach.f32(...) etc. markers
    script::Marshal marshal;
    std::unordered_map<const void*, ComponentInfo*> by_table;  ///< Component table identity -> info.

    explicit Impl(World& w) : world(w.c_ptr()), marshal(world) {}

    static Impl& self(lua_State* L) { return *static_cast<Impl*>(lua_tolightuserdata(L, lua_upvalueindex(1))); }

    void push_function(lua_CFunction fn, const char* name) {
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, fn, name, 1);
    }

    // ---- entities and components ---------------------------------------------

    void push_entity(lua_State* S, ecs::entity_t id) {
        auto* data =
            static_cast<std::uint64_t*>(lua_newuserdatataggedwithmetatable(S, sizeof(std::uint64_t), entity_tag));
        *data = id;
    }

    static ecs::entity_t check_entity(lua_State* S, int index) {
        auto* data = static_cast<std::uint64_t*>(lua_touserdatatagged(S, index, entity_tag));
        if (!data) {
            luaL_typeerror(S, index, "Entity");
        }
        return *data;
    }

    const ComponentInfo& check_component(lua_State* S, int index) {
        if (!lua_istable(S, index)) {
            luaL_typeerror(S, index, "Component");
        }
        if (const auto it = by_table.find(lua_topointer(S, index)); it != by_table.end()) {
            return *it->second;  // fast path: the canonical table
        }
        lua_getfield(S, index, "name");
        if (lua_type(S, -1) != LUA_TSTRING) {
            raise(S, ErrorCode::script_unknown_component, "expected a component",
                  "Pass a value from veach.component(...) or veach.components.<Name>.");
        }
        const std::string name = lua_tostring(S, -1);
        lua_pop(S, 1);
        const auto it = components.find(name);
        if (it == components.end()) {
            std::vector<std::string> names;
            for (const auto& [n, info] : components) {
                names.push_back(n);
            }
            raise(S, ErrorCode::script_unknown_component, std::format("unknown component '{}'", name),
                  did_you_mean(name, names));
        }
        return *it->second;
    }

    CommandBuffer& require_active(lua_State* S, const char* what) const {
        if (!active) {
            raise(S, ErrorCode::internal, std::format("{} called outside a script invocation", what));
        }
        return *active;
    }

    void require_loading(lua_State* S, const char* what) const {
        if (!loading) {
            raise(S, ErrorCode::script_load_time_only, std::format("{} can only be called while a script loads", what),
                  "Call it at the top level of the script, not inside a system.");
        }
    }

    /// Pushes the component's canonical table { name = ..., defaults = ... },
    /// creating it on first use. Identity lookups make check_component O(1).
    void push_component_table(lua_State* S, ComponentInfo& info) {
        if (info.table_ref == 0) {
            info.plan = &marshal.plan(info.id);
            info.size = static_cast<std::size_t>(ecs_get_type_info(world.c_ptr(), info.id)->size);
            lua_createtable(L, 0, 2);
            lua_pushstring(L, info.name.c_str());
            lua_setfield(L, -2, "name");
            marshal.push(L, *info.plan, info.defaults);
            lua_setfield(L, -2, "defaults");
            lua_setreadonly(L, -1, 1);
            by_table.emplace(lua_topointer(L, -1), &info);
            info.table_ref = lua_ref(L, -1);
            lua_pop(L, 1);
        }
        lua_getref(S, info.table_ref);
    }

    /// Picks up C++ components tagged Scriptable that modules registered since the last call.
    void refresh_cpp_components() {
        const ecs::entity_t scriptable = world.id<Scriptable>();
        world.each([&](Entity e, const flecs::Component&) {
            if (!e.has(scriptable)) {
                return;
            }
            const std::string name = e.name().c_str();
            if (components.contains(name)) {
                return;
            }
            auto fields = struct_fields(world, e.id());
            if (value_kind(world, e.id()) != ValueKind::structure ||
                std::ranges::any_of(fields, [](const FieldInfo& f) { return f.kind == ValueKind::unsupported; })) {
                log::warn("script", "component '{}' is tagged Scriptable but has fields Luau cannot represent; skipped",
                          e.path().c_str());
                return;
            }
            auto info = std::make_unique<ComponentInfo>();
            info->name = name;
            info->id = e.id();
            info->fields = std::move(fields);
            info->defaults = ecs_ptr_new(world.c_ptr(), e.id());
            components.emplace(name, std::move(info));
        });

        lua_getref(L, components_ref);
        lua_setreadonly(L, -1, 0);
        for (auto& [name, info] : components) {
            if (!info->script_defined) {
                push_component_table(L, *info);
                lua_setfield(L, -2, name.c_str());
            }
        }
        lua_setreadonly(L, -1, 1);
        lua_pop(L, 1);
    }

    // ---- explicit field types ---------------------------------------------------

    [[nodiscard]] bool is_type_marker(lua_State* S, int index) const {
        if (!lua_istable(S, index) || !lua_getmetatable(S, index)) {
            return false;
        }
        lua_getref(S, type_marker_ref);
        const bool marker = lua_rawequal(S, -1, -2) != 0;
        lua_pop(S, 2);
        return marker;
    }

    /// The storage type for a field from its defaults-table value: a plain
    /// value (inferred) or a type marker (explicit).
    ecs::entity_t field_type(lua_State* S, int index, const std::string& component, const std::string& field) {
        if (is_type_marker(S, index)) {
            lua_getfield(S, index, "kind");
            const std::string kind = lua_tostring(S, -1);
            lua_pop(S, 1);
            if (kind == "f32") {
                return world.id<float>();
            }
            if (kind == "f64") {
                return world.id<double>();
            }
            if (kind == "i32") {
                return world.id<std::int32_t>();
            }
            if (kind == "u32") {
                return world.id<std::uint32_t>();
            }
            if (kind == "vec3") {
                return world.id<math::vec3>();
            }
            if (kind == "entity") {
                return flecs::Entity;
            }
            if (kind == "optional") {
                lua_getfield(S, index, "inner");
                const ecs::entity_t inner = field_type(S, lua_gettop(S), component, field);
                lua_pop(S, 1);
                return optional_type(world, inner);
            }
        }
        switch (lua_type(S, index)) {
            case LUA_TBOOLEAN: return world.id<bool>();
            case LUA_TNUMBER: return world.id<double>();
            case LUA_TSTRING: return world.id<std::string>();
            case LUA_TVECTOR: return world.id<math::vec3>();
            case LUA_TUSERDATA:
                if (script::to_position(S, index) != nullptr) {
                    return world.id<math::dvec3>();
                }
                [[fallthrough]];
            default:
                raise(S, ErrorCode::script_unsupported_field,
                      std::format("component '{}': field '{}' has a default of type {}", component, field,
                                  luaL_typename(S, index)),
                      "Use a boolean, number, string, vector or Position default, or an explicit type: veach.f32(), "
                      "veach.i32(), veach.u32(), veach.f64(), veach.vec3(), veach.entity(), veach.optional(...).");
        }
    }

    /// Writes the defaults table into the component's default value. Markers
    /// contribute their own default (if any); optional fields start absent.
    void fill_defaults(lua_State* S, int table, ComponentInfo& info) {
        const script::TypePlan& plan = marshal.plan(info.id);
        for (const script::FieldPlan& f : plan.fields) {
            lua_getfield(S, table, f.name.c_str());
            const int value = lua_gettop(S);
            void* dst = static_cast<char*>(info.defaults) + f.offset;
            const script::Path path{nullptr, f.name};
            if (is_type_marker(S, value)) {
                lua_getfield(S, value, "default");
                if (!lua_isnil(S, -1) && f.type->kind != ValueKind::optional) {
                    marshal.read(S, lua_gettop(S), *f.type, dst, path);
                }
                lua_pop(S, 1);
            } else {
                marshal.read(S, value, *f.type, dst, path);
            }
            lua_pop(S, 1);
        }
    }

    static int api_type_marker(lua_State* S) {
        Impl& rt = self(S);
        const auto& kind = type_markers.at(static_cast<std::size_t>(lua_tointeger(S, lua_upvalueindex(2))));
        const bool has_default = kind.has_default && !lua_isnoneornil(S, 1);  // before pushing anything
        if (has_default && lua_type(S, 1) != kind.lua_type) {
            raise(S, ErrorCode::script_invalid_value,
                  std::format("veach.{}() default must be a {}, got {}", kind.name, lua_typename(S, kind.lua_type),
                              luaL_typename(S, 1)));
        }
        lua_createtable(S, 0, 2);
        lua_pushstring(S, kind.name);
        lua_setfield(S, -2, "kind");
        if (has_default) {
            lua_pushvalue(S, 1);
            lua_setfield(S, -2, "default");
        }
        lua_getref(S, rt.type_marker_ref);
        lua_setmetatable(S, -2);
        lua_setreadonly(S, -1, 1);
        return 1;
    }

    static int api_optional(lua_State* S) {
        Impl& rt = self(S);
        luaL_checkany(S, 1);
        if (rt.is_type_marker(S, 1)) {
            lua_getfield(S, 1, "kind");
            if (std::string_view(lua_tostring(S, -1)) == "optional") {
                raise(S, ErrorCode::script_invalid_value, "veach.optional(veach.optional(...)) is not allowed");
            }
            lua_pop(S, 1);
        }
        lua_createtable(S, 0, 2);
        lua_pushstring(S, "optional");
        lua_setfield(S, -2, "kind");
        lua_pushvalue(S, 1);
        lua_setfield(S, -2, "inner");
        lua_getref(S, rt.type_marker_ref);
        lua_setmetatable(S, -2);
        lua_setreadonly(S, -1, 1);
        return 1;
    }

    // ---- bindings: veach.* ---------------------------------------------------

    static int api_component(lua_State* S) {
        Impl& rt = self(S);
        rt.require_loading(S, "veach.component()");
        const std::string name = luaL_checkstring(S, 1);
        luaL_checktype(S, 2, LUA_TTABLE);

        struct NewField {
            std::string name;
            ecs::entity_t type;
        };
        std::vector<NewField> fields;
        lua_pushnil(S);
        while (lua_next(S, 2) != 0) {
            if (lua_type(S, -2) != LUA_TSTRING) {
                raise(S, ErrorCode::script_unsupported_field,
                      std::format("component '{}': field names must be strings", name));
            }
            const std::string field = lua_tostring(S, -2);
            fields.push_back({field, rt.field_type(S, lua_gettop(S), name, field)});
            lua_pop(S, 1);
        }
        // Lua table order is unspecified; sort so the layout is deterministic.
        std::ranges::sort(fields, {}, &NewField::name);

        if (const auto it = rt.components.find(name); it != rt.components.end()) {
            ComponentInfo& existing = *it->second;
            bool same = existing.script_defined && existing.fields.size() == fields.size();
            for (std::size_t i = 0; same && i < fields.size(); ++i) {
                same = existing.fields[i].name == fields[i].name && existing.fields[i].type == fields[i].type;
            }
            if (!same) {
                raise(S, ErrorCode::script_component_conflict,
                      std::format("component '{}' is already defined with different fields", name),
                      "Use a different name, or make the definitions identical.");
            }
            rt.push_component_table(S, existing);
            return 1;
        }
        if (rt.world.lookup(name.c_str())) {
            raise(S, ErrorCode::script_component_conflict,
                  std::format("'{}' already names an entity or C++ component", name),
                  "Pick another name for the script component.");
        }

        auto component = rt.world.component(name.c_str());
        for (const NewField& f : fields) {
            component.member(f.type, f.name.c_str());
        }
        auto info = std::make_unique<ComponentInfo>();
        info->name = name;
        info->id = component.id();
        info->fields = struct_fields(rt.world, component.id());
        info->script_defined = true;
        info->defaults = ecs_ptr_new(rt.world.c_ptr(), component.id());
        rt.fill_defaults(S, 2, *info);
        log::debug("script", "component '{}' defined ({} fields)", name, fields.size());

        ComponentInfo& ref = *info;
        rt.components.emplace(name, std::move(info));
        rt.push_component_table(S, ref);
        return 1;
    }

    static int api_query(lua_State* S) {
        Impl& rt = self(S);
        const int count = lua_gettop(S);
        if (count < 1 || count > static_cast<int>(max_query_components)) {
            luaL_error(S, "veach.query takes 1 to %d components, got %d", static_cast<int>(max_query_components),
                       count);
        }
        lua_createtable(S, 0, 1);
        lua_createtable(S, count, 0);
        for (int i = 1; i <= count; ++i) {
            static_cast<void>(rt.check_component(S, i));
            lua_pushvalue(S, i);
            lua_rawseti(S, -2, i);
        }
        lua_setfield(S, -2, "components");
        lua_setreadonly(S, -1, 1);
        return 1;
    }

    static int api_system(lua_State* S) {
        Impl& rt = self(S);
        rt.require_loading(S, "veach.system()");
        const std::string name = luaL_checkstring(S, 1);
        const std::string phase_name = luaL_checkstring(S, 2);
        luaL_checktype(S, 3, LUA_TTABLE);
        luaL_checktype(S, 4, LUA_TFUNCTION);

        const auto phase = find_phase(phase_name);
        if (!phase) {
            std::vector<std::string> names;
            for (const PhaseInfo& info : phases()) {
                names.emplace_back(info.name);
            }
            raise(S, ErrorCode::ecs_unknown_phase, std::format("unknown phase '{}'", phase_name),
                  did_you_mean(phase_name, names));
        }
        const auto taken = [&](const auto& list) {
            return std::ranges::any_of(list, [&](const auto& s) {
                return s->name == name && (rt.reloading.empty() || s->owner != rt.reloading);
            });
        };
        if (taken(rt.systems) || taken(rt.pending_systems)) {
            raise(S, ErrorCode::script_duplicate_system, std::format("system '{}' is already defined", name),
                  "System names are global; give this one a unique name.");
        }

        auto system = std::make_unique<ScriptSystem>();
        system->name = name;
        system->phase = *phase;
        lua_getfield(S, 3, "components");
        if (!lua_istable(S, -1)) {
            luaL_error(S, "veach.system: the third argument must be a query from veach.query(...)");
        }
        const int count = lua_objlen(S, -1);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(S, -1, i);
            system->components.push_back(&rt.check_component(S, lua_gettop(S)));
            lua_pop(S, 1);
        }
        lua_pop(S, 1);
        lua_pushvalue(S, 4);
        system->fn_ref = lua_ref(S, -1);
        lua_pop(S, 1);
        rt.pending_systems.push_back(std::move(system));
        return 0;
    }

    /// veach.spawn(name?) or veach.spawn({ name = ..., parent = entity }).
    static int api_spawn(lua_State* S) {
        Impl& rt = self(S);
        CommandBuffer& cmd = rt.require_active(S, "veach.spawn()");
        std::string name;
        ecs::entity_t parent = 0;
        if (lua_istable(S, 1)) {
            lua_getfield(S, 1, "name");
            if (!lua_isnil(S, -1)) {
                name = luaL_checkstring(S, -1);
            }
            lua_getfield(S, 1, "parent");
            if (!lua_isnil(S, -1)) {
                parent = check_entity(S, lua_gettop(S));
                if (!ecs_is_alive(rt.world.c_ptr(), parent)) {
                    raise(S, ErrorCode::script_invalid_value, "veach.spawn: the parent entity is not alive",
                          "Spawn the parent first; a parent spawned in this same invocation is fine.");
                }
            }
            lua_pop(S, 2);
        } else {
            name = luaL_optstring(S, 1, "");
        }
        const Entity e = cmd.spawn(std::move(name), parent);
        rt.push_entity(S, e.id());
        return 1;
    }

    // ---- bindings: veach.input (actions bound in veach.project.json; scripts never see raw keys) ----

    static const InputAction& check_action(lua_State* S, int index) {
        Impl& rt = self(S);
        const std::string name = luaL_checkstring(S, index);
        const Entity e = rt.world.lookup(action_path(name).c_str());
        const InputAction* action = e ? e.try_get<InputAction>() : nullptr;
        if (!action) {
            std::vector<std::string> names;
            rt.world.each([&](Entity a, const InputAction&) { names.emplace_back(a.name().c_str()); });
            std::string hint = did_you_mean(name, names);
            hint += std::format("{}Actions are bound under \"input\" in veach.project.json.", hint.empty() ? "" : " ");
            raise(S, ErrorCode::input_unknown_action, std::format("no input action '{}'", name), hint);
        }
        return *action;
    }

    static int input_down(lua_State* S) {
        lua_pushboolean(S, check_action(S, 1).down ? 1 : 0);
        return 1;
    }
    static int input_pressed(lua_State* S) {
        lua_pushboolean(S, check_action(S, 1).pressed ? 1 : 0);
        return 1;
    }
    static int input_released(lua_State* S) {
        lua_pushboolean(S, check_action(S, 1).released ? 1 : 0);
        return 1;
    }
    static const InputAxis& check_axis(lua_State* S, int index) {
        Impl& rt = self(S);
        const std::string name = luaL_checkstring(S, index);
        const Entity e = rt.world.lookup(action_path(name).c_str());
        const InputAxis* axis = e ? e.try_get<InputAxis>() : nullptr;
        if (!axis) {
            std::vector<std::string> names;
            rt.world.each([&](Entity a, const InputAxis&) { names.emplace_back(a.name().c_str()); });
            std::string hint = did_you_mean(name, names);
            if (e && e.has<InputAction>()) {
                hint += std::format("{}'{}' is a key action: read it with down(), or as axis(negative, positive).",
                                    hint.empty() ? "" : " ", name);
            }
            hint += std::format("{}Analog actions are bound under \"axes\" in veach.project.json.",
                                hint.empty() ? "" : " ");
            raise(S, ErrorCode::input_unknown_action, std::format("no analog input action '{}'", name), hint);
        }
        return *axis;
    }

    /// axis("look_x"): an analog action's value. axis("move_back", "move_forward"): -1, 0 or 1 from two held
    /// key actions.
    static int input_axis(lua_State* S) {
        if (lua_isnoneornil(S, 2)) {
            lua_pushnumber(S, static_cast<double>(check_axis(S, 1).value));
            return 1;
        }
        const bool negative = check_action(S, 1).down;
        const bool positive = check_action(S, 2).down;
        lua_pushnumber(S, (positive ? 1.0 : 0.0) - (negative ? 1.0 : 0.0));
        return 1;
    }
    static int input_gamepads(lua_State* S) {
        lua_pushnumber(S, static_cast<double>(self(S).world.get<Input>().gamepads));
        return 1;
    }

    // ---- bindings: veach.quat (glm underneath, so results match C++ exactly) ----

    static math::quat check_quat(lua_State* S, int index) {
        if (!lua_istable(S, index)) {
            luaL_typeerror(S, index, "quat");
        }
        std::array<float, 4> xyzw{};
        constexpr std::array<const char*, 4> keys{"x", "y", "z", "w"};
        for (std::size_t i = 0; i < keys.size(); ++i) {
            lua_getfield(S, index, keys[i]);
            if (!lua_isnumber(S, -1)) {
                raise(S, ErrorCode::script_invalid_value, std::format("a quat needs a number '{}'", keys[i]),
                      "Quaternions are { x, y, z, w } tables; build them with veach.quat.*.");
            }
            xyzw[i] = static_cast<float>(lua_tonumber(S, -1));
            lua_pop(S, 1);
        }
        return {xyzw[3], xyzw[0], xyzw[1], xyzw[2]};  // glm: (w, x, y, z)
    }

    static math::vec3 check_vector(lua_State* S, int index) {
        const float* v = lua_tovector(S, index);
        if (!v) {
            luaL_typeerror(S, index, "vector");
        }
        return {v[0], v[1], v[2]};
    }

    static int push_quat(lua_State* S, const math::quat& q) {
        lua_createtable(S, 0, 4);
        lua_pushnumber(S, q.x);
        lua_setfield(S, -2, "x");
        lua_pushnumber(S, q.y);
        lua_setfield(S, -2, "y");
        lua_pushnumber(S, q.z);
        lua_setfield(S, -2, "z");
        lua_pushnumber(S, q.w);
        lua_setfield(S, -2, "w");
        return 1;
    }

    static int quat_identity(lua_State* S) { return push_quat(S, math::quat(1.0f, 0.0f, 0.0f, 0.0f)); }
    static int quat_from_axis_angle(lua_State* S) {
        const math::vec3 axis = check_vector(S, 1);
        if (math::length(axis) == 0.0f) {
            raise(S, ErrorCode::script_invalid_value, "veach.quat.from_axis_angle: the axis is zero");
        }
        return push_quat(S, math::angleAxis(static_cast<float>(luaL_checknumber(S, 2)), math::normalize(axis)));
    }
    static int quat_from_euler(lua_State* S) {
        const auto yaw = static_cast<float>(luaL_checknumber(S, 1));
        const auto pitch = static_cast<float>(luaL_checknumber(S, 2));
        const auto roll = static_cast<float>(luaL_optnumber(S, 3, 0.0));
        // Yaw about +Y, then pitch about the turned +X, then roll about the turned -Z: R = yaw * pitch * roll.
        return push_quat(S, math::angleAxis(yaw, math::world_up) * math::angleAxis(pitch, math::world_right) *
                                math::angleAxis(roll, math::vec3(0.0f, 0.0f, 1.0f)));
    }
    static int quat_mul(lua_State* S) { return push_quat(S, check_quat(S, 1) * check_quat(S, 2)); }
    static int quat_rotate(lua_State* S) {
        const math::vec3 v = check_quat(S, 1) * check_vector(S, 2);
        lua_pushvector(S, v.x, v.y, v.z);
        return 1;
    }
    static int quat_inverse(lua_State* S) { return push_quat(S, math::inverse(check_quat(S, 1))); }
    static int quat_normalize(lua_State* S) { return push_quat(S, math::normalize(check_quat(S, 1))); }
    static int quat_look_rotation(lua_State* S) {
        const math::vec3 forward = check_vector(S, 1);
        const math::vec3 up = lua_isnoneornil(S, 2) ? math::world_up : check_vector(S, 2);
        if (math::length(forward) == 0.0f || math::length(math::cross(forward, up)) < 1e-6f) {
            raise(S, ErrorCode::script_invalid_value, "veach.quat.look_rotation: forward is zero or parallel to up",
                  "Pass another up vector when looking straight up or down.");
        }
        return push_quat(S, math::quatLookAtRH(math::normalize(forward), math::normalize(up)));
    }

    // ---- bindings: veach.frames (docs/specs/frames/spec.md) ----------------------

    /// An optional frame argument: nil is the root world.
    Entity check_frame(lua_State* S, int index) {
        if (lua_isnoneornil(S, index)) {
            return Entity{};
        }
        return world.entity(check_entity(S, index));
    }

    [[noreturn]] static void raise_error(lua_State* S, const Error& error) {
        raise(S, error.code, error.message, error.hint);
    }

    static int frames_of(lua_State* S) {
        Impl& rt = self(S);
        const Entity frame = frame_of(rt.world, rt.world.entity(check_entity(S, 1)));
        if (!frame) {
            lua_pushnil(S);
        } else {
            rt.push_entity(S, frame.id());
        }
        return 1;
    }

    static int frames_convert(lua_State* S) {
        Impl& rt = self(S);
        const math::dvec3 p = script::check_position(S, 1, "veach.frames.convert");
        const auto pose = frame_pose(rt.world, rt.check_frame(S, 2), rt.check_frame(S, 3));
        if (!pose) {
            raise_error(S, pose.error());
        }
        script::push_position(S, pose->point(p));
        return 1;
    }

    static int frames_offset(lua_State* S) {
        Impl& rt = self(S);
        const auto d = offset(rt.world, rt.world.entity(check_entity(S, 1)), rt.world.entity(check_entity(S, 2)));
        if (!d) {
            raise_error(S, d.error());
        }
        const math::vec3 v(*d);
        lua_pushvector(S, v.x, v.y, v.z);
        return 1;
    }

    static int frames_up(lua_State* S) {
        Impl& rt = self(S);
        const Entity frame = rt.check_frame(S, 1);
        if (frame && !frame.has<ReferenceFrame>()) {
            raise(S, ErrorCode::ecs_not_a_frame, "veach.frames.up: the entity is not a reference frame",
                  "Pass a frame (veach.frames.of(e) gives an entity's), or nil for the root world.");
        }
        const math::vec3 u(up(rt.world, frame, script::check_position(S, 2, "veach.frames.up")));
        lua_pushvector(S, u.x, u.y, u.z);
        return 1;
    }

    static int api_log(lua_State* S) {
        const auto level = static_cast<log::Level>(lua_tointeger(S, lua_upvalueindex(2)));
        log::write(level, "script", luaL_checkstring(S, 1));
        return 0;
    }

    static int api_time_frame(lua_State* S) {
        const Time* t = self(S).world.try_get<Time>();
        lua_pushnumber(S, t ? static_cast<double>(t->frame) : 0.0);
        return 1;
    }

    static int api_time_delta(lua_State* S) {
        const Time* t = self(S).world.try_get<Time>();
        lua_pushnumber(S, t ? t->delta : 0.0);
        return 1;
    }

    static int api_time_elapsed(lua_State* S) {
        const Time* t = self(S).world.try_get<Time>();
        lua_pushnumber(S, t ? t->elapsed : 0.0);
        return 1;
    }

    static int api_require(lua_State* S) {
        Impl& rt = self(S);
        const std::string name = luaL_checkstring(S, 1);
        if (name != "@veach") {
            raise(S, ErrorCode::script_unknown_module, std::format("unknown module '{}'", name),
                  "The only module available in v0 is \"@veach\".");
        }
        lua_getref(S, rt.veach_ref);
        return 1;
    }

    // ---- bindings: Entity methods --------------------------------------------

    static int entity_id(lua_State* S) {
        lua_pushnumber(S, static_cast<double>(check_entity(S, 1)));
        return 1;
    }

    static int entity_get(lua_State* S) {
        Impl& rt = self(S);
        const ecs::entity_t e = check_entity(S, 1);
        const ComponentInfo& c = rt.check_component(S, 2);
        const void* ptr = ecs_is_alive(rt.world.c_ptr(), e) ? ecs_get_id(rt.world.c_ptr(), e, c.id) : nullptr;
        if (!ptr) {
            lua_pushnil(S);
        } else {
            rt.marshal.push(S, *c.plan, ptr);
        }
        return 1;
    }

    static int entity_set(lua_State* S) {
        Impl& rt = self(S);
        const ecs::entity_t e = check_entity(S, 1);
        const ComponentInfo& c = rt.check_component(S, 2);
        CommandBuffer& cmd = rt.require_active(S, "Entity:set()");
        void* value = cmd.emplace(rt.world.entity(e), c.id);
        // If reading raises (a Luau error unwinds as a C++ exception), drop the half-filled command.
        struct Rollback {
            CommandBuffer& cmd;
            bool armed = true;
            ~Rollback() {
                if (armed) {
                    cmd.pop_back();
                }
            }
        } rollback{cmd};
        ecs_ptr_copy(rt.world.c_ptr(), c.id, value, c.defaults);
        rt.marshal.read(S, 3, *c.plan, value, script::Path{nullptr, c.name});
        rollback.armed = false;
        return 0;
    }

    static int entity_has(lua_State* S) {
        Impl& rt = self(S);
        const ecs::entity_t e = check_entity(S, 1);
        const ComponentInfo& c = rt.check_component(S, 2);
        lua_pushboolean(S, ecs_is_alive(rt.world.c_ptr(), e) && ecs_has_id(rt.world.c_ptr(), e, c.id) ? 1 : 0);
        return 1;
    }

    static int entity_remove(lua_State* S) {
        Impl& rt = self(S);
        const ecs::entity_t e = check_entity(S, 1);
        const ComponentInfo& c = rt.check_component(S, 2);
        rt.require_active(S, "Entity:remove()").remove(rt.world.entity(e), c.id);
        return 0;
    }

    static int entity_despawn(lua_State* S) {
        Impl& rt = self(S);
        const ecs::entity_t e = check_entity(S, 1);
        rt.require_active(S, "Entity:despawn()").despawn(rt.world.entity(e));
        return 0;
    }

    static int entity_parent(lua_State* S) {
        Impl& rt = self(S);
        const ecs::entity_t e = check_entity(S, 1);
        const ecs::entity_t parent =
            ecs_is_alive(rt.world.c_ptr(), e) ? ecs_get_target(rt.world.c_ptr(), e, EcsChildOf, 0) : 0;
        if (parent == 0) {
            lua_pushnil(S);
        } else {
            rt.push_entity(S, parent);
        }
        return 1;
    }

    /// The entity's children now, in id order (a child spawned this invocation appears once it is applied).
    static int entity_children(lua_State* S) {
        Impl& rt = self(S);
        const ecs::entity_t e = check_entity(S, 1);
        std::vector<ecs::entity_t> children;
        if (ecs_is_alive(rt.world.c_ptr(), e)) {
            rt.world.entity(e).children([&](Entity child) { children.push_back(child.id()); });
        }
        std::ranges::sort(children);
        lua_createtable(S, static_cast<int>(children.size()), 0);
        for (std::size_t i = 0; i < children.size(); ++i) {
            rt.push_entity(S, children[i]);
            lua_rawseti(S, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    static int entity_tostring(lua_State* S) {
        Impl& rt = self(S);
        const ecs::entity_t e = check_entity(S, 1);
        const char* name = ecs_is_alive(rt.world.c_ptr(), e) ? ecs_get_name(rt.world.c_ptr(), e) : nullptr;
        const std::string text = name ? std::format("Entity({} '{}')", e, name) : std::format("Entity({})", e);
        lua_pushstring(S, text.c_str());
        return 1;
    }

    static int entity_eq(lua_State* S) {
        lua_pushboolean(S, check_entity(S, 1) == check_entity(S, 2) ? 1 : 0);
        return 1;
    }

    // ---- setup -----------------------------------------------------------------

    void open() {
        L = luaL_newstate();
        luaL_openlibs(L);

        // Entity metatable.
        lua_createtable(L, 0, 4);
        lua_createtable(L, 0, 6);
        push_function(entity_id, "id");
        lua_setfield(L, -2, "id");
        push_function(entity_get, "get");
        lua_setfield(L, -2, "get");
        push_function(entity_set, "set");
        lua_setfield(L, -2, "set");
        push_function(entity_has, "has");
        lua_setfield(L, -2, "has");
        push_function(entity_remove, "remove");
        lua_setfield(L, -2, "remove");
        push_function(entity_despawn, "despawn");
        lua_setfield(L, -2, "despawn");
        push_function(entity_parent, "parent");
        lua_setfield(L, -2, "parent");
        push_function(entity_children, "children");
        lua_setfield(L, -2, "children");
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, "__index");
        push_function(entity_tostring, "__tostring");
        lua_setfield(L, -2, "__tostring");
        push_function(entity_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "Entity");
        lua_setfield(L, -2, "__type");
        lua_setreadonly(L, -1, 1);
        entity_metatable_ref = lua_ref(L, -1);
        lua_setuserdatametatable(L, entity_tag);  // pops; new Entity userdata get it for free
        script::open_position(L);

        // The @veach module.
        lua_createtable(L, 0, static_cast<int>(api_function_names.size()));
        push_function(api_component, "component");
        lua_setfield(L, -2, "component");
        push_function(api_query, "query");
        lua_setfield(L, -2, "query");
        push_function(api_system, "system");
        lua_setfield(L, -2, "system");
        push_function(api_spawn, "spawn");
        lua_setfield(L, -2, "spawn");

        lua_newtable(L);  // metatable that marks explicit field types
        lua_pushstring(L, "TypeMarker");
        lua_setfield(L, -2, "__type");
        lua_setreadonly(L, -1, 1);
        type_marker_ref = lua_ref(L, -1);
        lua_pop(L, 1);
        for (std::size_t i = 0; i < type_markers.size(); ++i) {
            lua_pushlightuserdata(L, this);
            lua_pushinteger(L, static_cast<int>(i));
            lua_pushcclosure(L, api_type_marker, type_markers[i].name, 2);
            lua_setfield(L, -2, type_markers[i].name);
        }
        push_function(api_optional, "optional");
        lua_setfield(L, -2, "optional");

        lua_createtable(L, 0, 4);
        constexpr std::array<std::pair<const char*, log::Level>, 4> levels{{
            {"debug", log::Level::debug},
            {"info", log::Level::info},
            {"warn", log::Level::warn},
            {"error", log::Level::error},
        }};
        for (const auto& [fn_name, level] : levels) {
            lua_pushlightuserdata(L, this);
            lua_pushinteger(L, static_cast<int>(level));
            lua_pushcclosure(L, api_log, fn_name, 2);
            lua_setfield(L, -2, fn_name);
        }
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, "log");

        constexpr std::array<std::pair<const char*, lua_CFunction>, 8> quat_functions{{
            {"identity", quat_identity},
            {"from_axis_angle", quat_from_axis_angle},
            {"from_euler", quat_from_euler},
            {"mul", quat_mul},
            {"rotate", quat_rotate},
            {"inverse", quat_inverse},
            {"normalize", quat_normalize},
            {"look_rotation", quat_look_rotation},
        }};
        lua_createtable(L, 0, static_cast<int>(quat_functions.size()));
        for (const auto& [fn_name, fn] : quat_functions) {
            lua_pushcfunction(L, fn, fn_name);
            lua_setfield(L, -2, fn_name);
        }
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, "quat");

        lua_pushcfunction(L, script::api_position, "position");
        lua_setfield(L, -2, "position");
        constexpr std::array<std::pair<const char*, lua_CFunction>, 4> frame_functions{{
            {"of", frames_of},
            {"convert", frames_convert},
            {"offset", frames_offset},
            {"up", frames_up},
        }};
        lua_createtable(L, 0, static_cast<int>(frame_functions.size()));
        for (const auto& [fn_name, fn] : frame_functions) {
            push_function(fn, fn_name);
            lua_setfield(L, -2, fn_name);
        }
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, "frames");

        constexpr std::array<std::pair<const char*, lua_CFunction>, 5> input_functions{{
            {"down", input_down},
            {"pressed", input_pressed},
            {"released", input_released},
            {"axis", input_axis},
            {"gamepads", input_gamepads},
        }};
        lua_createtable(L, 0, static_cast<int>(input_functions.size()));
        for (const auto& [fn_name, fn] : input_functions) {
            push_function(fn, fn_name);
            lua_setfield(L, -2, fn_name);
        }
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, "input");

        lua_createtable(L, 0, 3);
        push_function(api_time_frame, "frame");
        lua_setfield(L, -2, "frame");
        push_function(api_time_delta, "delta");
        lua_setfield(L, -2, "delta");
        push_function(api_time_elapsed, "elapsed");
        lua_setfield(L, -2, "elapsed");
        lua_setreadonly(L, -1, 1);
        lua_setfield(L, -2, "time");

        lua_newtable(L);
        lua_pushvalue(L, -1);
        components_ref = lua_ref(L, -1);
        lua_pop(L, 1);
        lua_setfield(L, -2, "components");

        lua_setreadonly(L, -1, 1);
        veach_ref = lua_ref(L, -1);
        lua_pop(L, 1);

        push_function(api_require, "require");
        lua_setglobal(L, "require");

        // Dispatcher.
        auto dispatcher = compile_script(dispatcher_source, "veach/dispatch");
        VEACH_ASSERT(dispatcher.has_value(), "dispatcher failed to compile");
        if (luau_load(L, dispatcher->chunk_name.c_str(), dispatcher->bytecode.data(), dispatcher->bytecode.size(), 0) !=
                0 ||
            lua_pcall(L, 0, 1, 0) != 0) {
            VEACH_PANIC("dispatcher failed to load: {}", lua_tostring(L, -1));
        }
        dispatcher_ref = lua_ref(L, -1);
        lua_pop(L, 1);

        luaL_sandbox(L);  // freezes globals and builtin libraries
    }

    void activate(std::unique_ptr<ScriptSystem> system) {
        const std::string path = "scripts::" + system->name;
        // immediate(): the batch is fully collected before Luau runs, and the
        // command buffer is then applied directly (named spawns need a writable world).
        auto builder = world.system<>(path.c_str()).kind(phase_entity(world, system->phase)).immediate();
        for (const ComponentInfo* c : system->components) {
            builder.with(c->id).in();
        }
        ScriptSystem* raw = system.get();
        system->entity = builder.run([this, raw](Iter& it) { run_system(*raw, it); }).id();
        log::debug("script", "system '{}' registered in {}", system->name, phase_info(system->phase).name);
        systems.push_back(std::move(system));
    }

    void run_system(const ScriptSystem& system, Iter& it) {
        VEACH_ZONE_TEXT("script.system", system.name.data(), system.name.size());
        const int top = lua_gettop(L);
        lua_getref(L, dispatcher_ref);
        lua_getref(L, system.fn_ref);
        lua_newtable(L);
        const int entities = lua_gettop(L);
        for (std::size_t k = 0; k < system.components.size(); ++k) {
            lua_newtable(L);
        }

        // One pass per matched table: columns are walked with precomputed plans,
        // and the whole result is handed to Luau in a single call below.
        const std::size_t count = system.components.size();
        int row = 0;
        while (it.next()) {
            std::array<const char*, max_query_components> base{};
            std::array<std::size_t, max_query_components> stride{};
            for (std::size_t k = 0; k < count; ++k) {
                const auto field = static_cast<std::int8_t>(k);
                const std::size_t size = system.components[k]->size;
                base[k] = static_cast<const char*>(ecs_field_w_size(it.c_ptr(), size, field));
                stride[k] = ecs_field_is_self(it.c_ptr(), field) ? size : 0;
            }
            const std::size_t rows = it.count();
            for (std::size_t i = 0; i < rows; ++i) {
                ++row;
                push_entity(L, it.entity(i).id());
                lua_rawseti(L, entities, row);
                for (std::size_t k = 0; k < count; ++k) {
                    marshal.push(L, *system.components[k]->plan, base[k] + i * stride[k]);
                    lua_rawseti(L, entities + 1 + static_cast<int>(k), row);
                }
            }
        }
        if (row == 0) {
            lua_settop(L, top);
            return;
        }

        CommandBuffer cmd(world);
        active = &cmd;
        const int status = lua_pcall(L, 2 + static_cast<int>(system.components.size()), 0, 0);
        active = nullptr;
        if (status != LUA_OK) {
            log::error("script", Error{ErrorCode::script_runtime_error,
                                       std::format("system '{}': {}", system.name, lua_tostring(L, -1)),
                                       "This run's writes were discarded; the system runs again next time."});
            cmd.discard();
            lua_settop(L, top);
            return;
        }
        cmd.apply();
    }

    /// Deletes the systems and top-level entities of `chunk` (a reload that succeeded replaces them).
    void retire_script(const std::string& chunk) {
        std::erase_if(systems, [&](const std::unique_ptr<ScriptSystem>& s) {
            if (s->owner != chunk) {
                return false;
            }
            if (ecs_is_alive(world.c_ptr(), s->entity)) {
                ecs_delete(world.c_ptr(), s->entity);
            }
            lua_unref(L, s->fn_ref);
            return true;
        });
        if (const auto it = spawned_by.find(chunk); it != spawned_by.end()) {
            for (const ecs::entity_t e : it->second) {
                if (ecs_is_alive(world.c_ptr(), e)) {
                    ecs_delete(world.c_ptr(), e);
                }
            }
        }
    }

    Result<> execute(const ScriptAsset& script, std::vector<std::string>* returns = nullptr, bool replace = false) {
        VEACH_ZONE_TEXT("script.execute", script.chunk_name.data(), script.chunk_name.size());
        if (loading) {
            return make_error(ErrorCode::internal, "scripts cannot load other scripts while loading");
        }
        refresh_cpp_components();

        lua_State* T = lua_newthread(L);
        thread_refs.push_back(lua_ref(L, -1));
        lua_pop(L, 1);
        luaL_sandboxthread(T);

        if (luau_load(T, script.chunk_name.c_str(), script.bytecode.data(), script.bytecode.size(), 0) != 0) {
            return make_error(ErrorCode::script_compile_failed, lua_tostring(T, -1));
        }

        CommandBuffer cmd(world);
        active = &cmd;
        loading = true;
        const int status = lua_pcall(T, 0, returns ? LUA_MULTRET : 0, 0);
        loading = false;
        active = nullptr;

        if (status == LUA_OK && returns) {
            for (int i = 1, n = lua_gettop(T); i <= n; ++i) {
                std::size_t len = 0;
                const char* text = luaL_tolstring(T, i, &len);  // pushes; honours __tostring
                returns->emplace_back(text, len);
                lua_pop(T, 1);
            }
            lua_settop(T, 0);
        }

        if (status != LUA_OK) {
            std::string message = lua_tostring(T, -1) ? lua_tostring(T, -1) : "unknown error";
            lua_pop(T, 1);
            for (auto& system : pending_systems) {
                lua_unref(L, system->fn_ref);
            }
            pending_systems.clear();
            cmd.discard();
            return make_error(ErrorCode::script_runtime_error,
                              std::format("{}: {}", script.chunk_name.substr(1), message),
                              "Nothing from this script was applied: its systems and writes were discarded.");
        }
        if (replace) {
            retire_script(script.chunk_name);
        }
        spawned_by[script.chunk_name] = cmd.spawned();
        cmd.apply();
        for (auto& system : pending_systems) {
            system->owner = script.chunk_name;
            activate(std::move(system));
        }
        pending_systems.clear();
        log::debug("script", "{} {}", replace ? "reloaded" : "executed", script.chunk_name.substr(1));
        return {};
    }

    void close() {
        if (!L) {
            return;
        }
        for (const auto& system : systems) {
            if (ecs_is_alive(world.c_ptr(), system->entity)) {
                ecs_delete(world.c_ptr(), system->entity);
            }
        }
        systems.clear();
        pending_systems.clear();
        for (const auto& [name, info] : components) {
            ecs_ptr_free(world.c_ptr(), info->id, info->defaults);
        }
        components.clear();
        lua_close(L);
        L = nullptr;
    }
};

ScriptRuntime::ScriptRuntime(World& world) : impl_(std::make_unique<Impl>(world)) {
    impl_->open();
}

ScriptRuntime::~ScriptRuntime() {
    impl_->close();
}

Result<> ScriptRuntime::execute(const ScriptAsset& script) {
    return impl_->execute(script);
}

Result<> ScriptRuntime::reload(const ScriptAsset& script) {
    if (!impl_->spawned_by.contains(script.chunk_name)) {
        return {};  // never ran: nothing to replace
    }
    impl_->reloading = script.chunk_name;
    auto result = impl_->execute(script, nullptr, true);
    impl_->reloading.clear();
    if (result) {
        log::info("script", "reloaded {} ({} systems)", script.chunk_name.substr(1),
                  std::ranges::count_if(impl_->systems, [&](const auto& s) { return s->owner == script.chunk_name; }));
    }
    return result;
}

Result<> ScriptRuntime::execute_source(std::string_view source, std::string_view chunk_name) {
    auto script = compile_script(source, chunk_name);
    if (!script) {
        return std::unexpected(script.error());
    }
    return impl_->execute(*script);
}

Result<std::vector<std::string>> ScriptRuntime::evaluate(std::string_view source, std::string_view chunk_name) {
    auto script = compile_script(source, chunk_name);
    if (!script) {
        return std::unexpected(script.error());
    }
    std::vector<std::string> returns;
    if (auto executed = impl_->execute(*script, &returns); !executed) {
        return std::unexpected(executed.error());
    }
    return returns;
}

void ScriptRuntime::shutdown() {
    impl_->close();
}

std::size_t ScriptRuntime::system_count() const {
    return impl_->systems.size();
}

std::size_t ScriptRuntime::component_count() const {
    return static_cast<std::size_t>(
        std::ranges::count_if(impl_->components, [](const auto& entry) { return entry.second->script_defined; }));
}

std::span<const std::string_view> ScriptRuntime::api_names() {
    return api_function_names;
}

}  // namespace veach
