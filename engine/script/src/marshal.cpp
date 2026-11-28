#include "marshal.hpp"

#include "position.hpp"

#include <veach/core/math.hpp>
#include <veach/core/suggest.hpp>

#include <lualib.h>

#include <cstring>
#include <format>

namespace veach::script {

namespace {

const char* kind_label(ValueKind kind) {
    switch (kind) {
        case ValueKind::boolean: return "boolean";
        case ValueKind::integer:
        case ValueKind::number: return "number";
        case ValueKind::string: return "string";
        case ValueKind::vec3: return "vector";
        case ValueKind::dvec3: return "Position";
        case ValueKind::enumeration: return "string (enum constant)";
        case ValueKind::structure: return "table";
        case ValueKind::entity: return "Entity or nil";
        case ValueKind::optional: return "value or nil";
        case ValueKind::unsupported: return "unsupported";
    }
    return "?";
}

template <class T>
T load(const void* ptr) {
    T value;
    std::memcpy(&value, ptr, sizeof(T));
    return value;
}

template <class T>
void store(void* ptr, T value) {
    std::memcpy(ptr, &value, sizeof(T));
}

double load_number(ecs_primitive_kind_t kind, const void* ptr) {
    switch (kind) {
        case EcsF32: return static_cast<double>(load<float>(ptr));
        case EcsF64: return load<double>(ptr);
        case EcsU8: return load<std::uint8_t>(ptr);
        case EcsU16: return load<std::uint16_t>(ptr);
        case EcsU32: return load<std::uint32_t>(ptr);
        case EcsU64: return static_cast<double>(load<std::uint64_t>(ptr));
        case EcsI8: return load<std::int8_t>(ptr);
        case EcsI16: return load<std::int16_t>(ptr);
        case EcsI32: return load<std::int32_t>(ptr);
        case EcsI64: return static_cast<double>(load<std::int64_t>(ptr));
        default: return 0.0;
    }
}

void store_number(ecs_primitive_kind_t kind, void* ptr, double value) {
    switch (kind) {
        case EcsF32: store(ptr, static_cast<float>(value)); break;
        case EcsF64: store(ptr, value); break;
        case EcsU8: store(ptr, static_cast<std::uint8_t>(value)); break;
        case EcsU16: store(ptr, static_cast<std::uint16_t>(value)); break;
        case EcsU32: store(ptr, static_cast<std::uint32_t>(value)); break;
        case EcsU64: store(ptr, static_cast<std::uint64_t>(value)); break;
        case EcsI8: store(ptr, static_cast<std::int8_t>(value)); break;
        case EcsI16: store(ptr, static_cast<std::int16_t>(value)); break;
        case EcsI32: store(ptr, static_cast<std::int32_t>(value)); break;
        case EcsI64: store(ptr, static_cast<std::int64_t>(value)); break;
        default: break;
    }
}

ecs_primitive_kind_t primitive_of(const World& world, ecs::entity_t type) {
    if (const EcsEnum* e = ecs_get(world.c_ptr(), type, EcsEnum)) {
        type = e->underlying_type;
    }
    const EcsPrimitive* p = ecs_get(world.c_ptr(), type, EcsPrimitive);
    return p ? p->kind : ecs_primitive_kind_t{};
}

}  // namespace

void push_entity(lua_State* L, const World& world, ecs::entity_t id) {
    if (id == 0 || !ecs_is_alive(world.c_ptr(), id)) {
        lua_pushnil(L);
        return;
    }
    auto* data = static_cast<std::uint64_t*>(lua_newuserdatataggedwithmetatable(L, sizeof(std::uint64_t), entity_tag));
    *data = id;
}

ecs::entity_t to_entity(lua_State* L, int index) {
    const auto* data = static_cast<const std::uint64_t*>(lua_touserdatatagged(L, index, entity_tag));
    return data ? *data : 0;
}

void raise(lua_State* L, ErrorCode code, const std::string& message, const std::string& hint) {
    const Error error{code, message, hint};
    luaL_error(L, "%s", error.to_string().c_str());
}

std::string Path::str() const {
    std::string out = parent ? parent->str() : std::string();
    if (!out.empty()) {
        out += '.';
    }
    out += segment;
    return out;
}

const TypePlan& Marshal::plan(ecs::entity_t type) {
    if (const auto it = plans_.find(type); it != plans_.end()) {
        return *it->second;
    }
    auto plan = std::make_unique<TypePlan>();
    TypePlan& p = *plan;
    plans_.emplace(type, std::move(plan));  // insert first: nested plans may refer back
    p.id = type;
    p.kind = value_kind(world_, type);
    p.name = type_name(world_, type);
    p.primitive = primitive_of(world_, type);
    if (p.kind == ValueKind::structure) {
        for (FieldInfo& f : struct_fields(world_, type)) {
            if (f.kind != ValueKind::unsupported) {
                p.fields.push_back({std::move(f.name), f.offset, &this->plan(f.type)});
            }
        }
    } else if (p.kind == ValueKind::optional) {
        const OptionalLayout layout = optional_layout(world_, type);
        p.inner = &this->plan(layout.value_type);
        p.present_offset = layout.present_offset;
        p.value_offset = layout.value_offset;
    } else if (p.kind == ValueKind::enumeration) {
        // Resolve each constant's stored value once by writing it and reading it back.
        for (const std::string& constant : enum_constants(world_, type)) {
            std::int64_t storage = 0;
            static_cast<void>(write_enum(world_, type, &storage, constant));
            p.constants.emplace_back(static_cast<std::int64_t>(load_number(p.primitive, &storage)), constant);
        }
    }
    return p;
}

void Marshal::push(lua_State* L, const TypePlan& plan, const void* ptr) const {
    switch (plan.kind) {
        case ValueKind::boolean: lua_pushboolean(L, *static_cast<const bool*>(ptr) ? 1 : 0); return;
        case ValueKind::integer:
        case ValueKind::number: lua_pushnumber(L, load_number(plan.primitive, ptr)); return;
        case ValueKind::string: {
            const auto& s = *static_cast<const std::string*>(ptr);
            lua_pushlstring(L, s.data(), s.size());
            return;
        }
        case ValueKind::vec3: {
            const auto& v = *static_cast<const math::vec3*>(ptr);
            lua_pushvector(L, v.x, v.y, v.z);
            return;
        }
        case ValueKind::dvec3: push_position(L, *static_cast<const math::dvec3*>(ptr)); return;
        case ValueKind::enumeration: {
            const auto value = static_cast<std::int64_t>(load_number(plan.primitive, ptr));
            for (const auto& [constant, name] : plan.constants) {
                if (constant == value) {
                    lua_pushlstring(L, name.data(), name.size());
                    return;
                }
            }
            lua_pushstring(L, "");
            return;
        }
        case ValueKind::structure:
            lua_createtable(L, 0, static_cast<int>(plan.fields.size()));
            for (const FieldPlan& f : plan.fields) {
                push(L, *f.type, static_cast<const char*>(ptr) + f.offset);
                lua_setfield(L, -2, f.name.c_str());
            }
            return;
        case ValueKind::entity: push_entity(L, world_, *static_cast<const ecs::entity_t*>(ptr)); return;
        case ValueKind::optional: {
            const auto* base = static_cast<const char*>(ptr);
            if (*static_cast<const bool*>(static_cast<const void*>(base + plan.present_offset))) {
                push(L, *plan.inner, base + plan.value_offset);
            } else {
                lua_pushnil(L);
            }
            return;
        }
        case ValueKind::unsupported: lua_pushnil(L); return;
    }
}

void Marshal::read(lua_State* L, int index, const TypePlan& plan, void* ptr, const Path& path) const {
    const auto mismatch = [&] {
        raise(L, ErrorCode::script_invalid_value,
              std::format("'{}' expects a {}, got {}", path.str(), kind_label(plan.kind), luaL_typename(L, index)));
    };
    switch (plan.kind) {
        case ValueKind::boolean:
            if (!lua_isboolean(L, index)) {
                mismatch();
            }
            *static_cast<bool*>(ptr) = lua_toboolean(L, index) != 0;
            return;
        case ValueKind::integer:
        case ValueKind::number:
            if (lua_type(L, index) != LUA_TNUMBER) {
                mismatch();
            }
            store_number(plan.primitive, ptr, lua_tonumber(L, index));
            return;
        case ValueKind::string: {
            if (lua_type(L, index) != LUA_TSTRING) {
                mismatch();
            }
            std::size_t len = 0;
            const char* s = lua_tolstring(L, index, &len);
            static_cast<std::string*>(ptr)->assign(s, len);
            return;
        }
        case ValueKind::vec3: {
            const float* v = lua_tovector(L, index);
            if (!v) {
                mismatch();
            }
            *static_cast<math::vec3*>(ptr) = math::vec3(v[0], v[1], v[2]);
            return;
        }
        case ValueKind::dvec3: {
            const math::dvec3* p = to_position(L, index);
            if (!p) {
                raise(L, ErrorCode::script_invalid_value,
                      std::format("'{}' expects a Position, got {}", path.str(), luaL_typename(L, index)),
                      "Positions are doubles relative to a frame: write veach.position(x, y, z), or "
                      "veach.position(v) for a vector v.");
            }
            *static_cast<math::dvec3*>(ptr) = *p;
            return;
        }
        case ValueKind::enumeration: {
            if (lua_type(L, index) != LUA_TSTRING) {
                mismatch();
            }
            const std::string_view name = lua_tostring(L, index);
            for (const auto& [constant, constant_name] : plan.constants) {
                if (constant_name == name) {
                    store_number(plan.primitive, ptr, static_cast<double>(constant));
                    return;
                }
            }
            std::vector<std::string> names;
            for (const auto& [constant, constant_name] : plan.constants) {
                names.push_back(constant_name);
            }
            raise(L, ErrorCode::script_invalid_value, std::format("'{}' has no constant '{}'", path.str(), name),
                  did_you_mean(name, names));
        }
        case ValueKind::structure: {
            if (!lua_istable(L, index)) {
                mismatch();
            }
            const int table = lua_absindex(L, index);
            lua_pushnil(L);
            while (lua_next(L, table) != 0) {
                std::size_t len = 0;
                const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tolstring(L, -2, &len) : nullptr;
                const FieldPlan* field = nullptr;
                for (const FieldPlan& f : plan.fields) {
                    if (key && f.name.size() == len && std::memcmp(f.name.data(), key, len) == 0) {
                        field = &f;
                        break;
                    }
                }
                if (!field) {
                    if (!key) {
                        raise(L, ErrorCode::script_invalid_value, std::format("'{}' has a non-string key", path.str()));
                    }
                    std::vector<std::string> names;
                    for (const FieldPlan& f : plan.fields) {
                        names.push_back(f.name);
                    }
                    raise(L, ErrorCode::script_invalid_value, std::format("unknown field '{}' in {}", key, path.str()),
                          did_you_mean(key, names));
                }
                const Path child{&path, field->name};
                read(L, -1, *field->type, static_cast<char*>(ptr) + field->offset, child);
                lua_pop(L, 1);
            }
            return;
        }
        case ValueKind::entity: {
            if (lua_isnil(L, index)) {
                *static_cast<ecs::entity_t*>(ptr) = 0;
                return;
            }
            const ecs::entity_t id = to_entity(L, index);
            if (id == 0) {
                mismatch();
            }
            *static_cast<ecs::entity_t*>(ptr) = id;
            return;
        }
        case ValueKind::optional: {
            auto* base = static_cast<char*>(ptr);
            bool& present = *static_cast<bool*>(static_cast<void*>(base + plan.present_offset));
            if (lua_isnil(L, index)) {
                present = false;
                return;
            }
            read(L, index, *plan.inner, base + plan.value_offset, path);
            present = true;
            return;
        }
        case ValueKind::unsupported:
            raise(L, ErrorCode::script_unsupported_field, std::format("'{}' cannot be written from Luau", path.str()));
    }
}

}  // namespace veach::script
