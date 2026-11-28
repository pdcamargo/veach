#include <veach/ecs/reflection.hpp>

#include <veach/core/math.hpp>

#include <cstring>
#include <format>
#include <string>

namespace veach {

namespace {

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

const EcsPrimitive* primitive_of(const World& world, ecs::entity_t type) {
    return ecs_get(world.c_ptr(), type, EcsPrimitive);
}

ecs::entity_t enum_underlying(const World& world, ecs::entity_t type) {
    const EcsEnum* e = ecs_get(world.c_ptr(), type, EcsEnum);
    return e ? e->underlying_type : 0;
}

/// Flecs stores constants of unsigned enums in value_unsigned, others in value.
double constant_value(const World& world, ecs::entity_t type, const ecs_enum_constant_t& constant) {
    const EcsPrimitive* p = primitive_of(world, enum_underlying(world, type));
    const bool is_unsigned = p && ((p->kind >= EcsU8 && p->kind <= EcsU64) || p->kind == EcsUPtr);
    return is_unsigned ? static_cast<double>(constant.value_unsigned) : static_cast<double>(constant.value);
}

}  // namespace

void register_builtin_reflection(World& world) {
    world.component<std::string>()
        .opaque(flecs::String)
        .serialize([](const flecs::serializer* s, const std::string* data) {
            const char* str = data->c_str();
            return s->value(flecs::String, &str);
        })
        .assign_string([](std::string* data, const char* value) { *data = value; });

    world.component<math::vec2>("veach::math::vec2").member(&math::vec2::x, "x").member(&math::vec2::y, "y");
    world.component<math::vec3>("veach::math::vec3")
        .member(&math::vec3::x, "x")
        .member(&math::vec3::y, "y")
        .member(&math::vec3::z, "z");
    world.component<math::vec4>("veach::math::vec4")
        .member(&math::vec4::x, "x")
        .member(&math::vec4::y, "y")
        .member(&math::vec4::z, "z")
        .member(&math::vec4::w, "w");
    world.component<math::dvec3>("veach::math::dvec3")
        .member(&math::dvec3::x, "x")
        .member(&math::dvec3::y, "y")
        .member(&math::dvec3::z, "z");
    world.component<math::mat3>("veach::math::mat3")
        .member<math::vec3>("c0", 1, 0)
        .member<math::vec3>("c1", 1, sizeof(math::vec3))
        .member<math::vec3>("c2", 1, 2 * sizeof(math::vec3));
    // glm keeps a matrix's columns private, so its members are declared by offset: c0..c3, column-major.
    world.component<math::mat4>("veach::math::mat4")
        .member<math::vec4>("c0", 1, 0)
        .member<math::vec4>("c1", 1, sizeof(math::vec4))
        .member<math::vec4>("c2", 1, 2 * sizeof(math::vec4))
        .member<math::vec4>("c3", 1, 3 * sizeof(math::vec4));
    world.component<math::quat>("veach::math::quat")
        .member(&math::quat::x, "x")
        .member(&math::quat::y, "y")
        .member(&math::quat::z, "z")
        .member(&math::quat::w, "w");
    world.component<math::dquat>("veach::math::dquat")
        .member(&math::dquat::x, "x")
        .member(&math::dquat::y, "y")
        .member(&math::dquat::z, "z")
        .member(&math::dquat::w, "w");
}

ValueKind value_kind(const World& world, ecs::entity_t type) {
    if (type == world.id<std::string>()) {
        return ValueKind::string;
    }
    if (type == flecs::Entity) {
        return ValueKind::entity;
    }
    if (type == world.id<math::vec3>()) {
        return ValueKind::vec3;
    }
    if (type == world.id<math::dvec3>()) {
        return ValueKind::dvec3;
    }
    const EcsType* t = ecs_get(world.c_ptr(), type, EcsType);
    if (!t) {
        return ValueKind::unsupported;
    }
    switch (t->kind) {
        case EcsPrimitiveType: {
            const EcsPrimitive* p = primitive_of(world, type);
            switch (p->kind) {
                case EcsBool: return ValueKind::boolean;
                case EcsF32:
                case EcsF64: return ValueKind::number;
                case EcsU8:
                case EcsU16:
                case EcsU32:
                case EcsU64:
                case EcsI8:
                case EcsI16:
                case EcsI32:
                case EcsI64: return ValueKind::integer;
                default: return ValueKind::unsupported;
            }
        }
        case EcsEnumType: return ValueKind::enumeration;
        case EcsStructType:
            return ecs_has_id(world.c_ptr(), type, world.id<Optional>()) ? ValueKind::optional : ValueKind::structure;
        default: return ValueKind::unsupported;
    }
}

std::vector<FieldInfo> struct_fields(const World& world, ecs::entity_t type) {
    std::vector<FieldInfo> fields;
    const EcsStruct* s = ecs_get(world.c_ptr(), type, EcsStruct);
    if (!s) {
        return fields;
    }
    const auto* members = static_cast<const ecs_member_t*>(s->members.array);
    for (std::int32_t i = 0; i < s->members.count; ++i) {
        const ecs_member_t& m = members[i];
        ValueKind kind = value_kind(world, m.type);
        if (m.count > 1) {
            kind = ValueKind::unsupported;  // inline arrays: not exposed yet
        }
        fields.push_back({m.name, m.type, m.offset, kind});
    }
    return fields;
}

ecs::entity_t optional_type(World& world, ecs::entity_t value_type) {
    const std::string name = "veach::optional::" + type_name(world, value_type);
    if (const Entity existing = world.lookup(name.c_str())) {
        return existing.id();
    }
    auto type = world.component(name.c_str());
    type.member(world.id<bool>(), "present");
    type.member(value_type, "value");
    type.add<Optional>();
    return type.id();
}

OptionalLayout optional_layout(const World& world, ecs::entity_t optional) {
    OptionalLayout layout;
    for (const FieldInfo& f : struct_fields(world, optional)) {
        if (f.name == "present") {
            layout.present_offset = f.offset;
        } else if (f.name == "value") {
            layout.value_offset = f.offset;
            layout.value_type = f.type;
        }
    }
    return layout;
}

std::vector<std::string> enum_constants(const World& world, ecs::entity_t type) {
    std::vector<std::string> names;
    const EcsConstants* c = ecs_get(world.c_ptr(), type, EcsConstants);
    if (!c) {
        return names;
    }
    const auto* constants = static_cast<const ecs_enum_constant_t*>(c->ordered_constants.array);
    for (std::int32_t i = 0; i < c->ordered_constants.count; ++i) {
        names.emplace_back(constants[i].name);
    }
    return names;
}

std::string type_name(const World& world, ecs::entity_t type) {
    switch (value_kind(world, type)) {
        case ValueKind::string: return "string";
        case ValueKind::vec3: return "vec3";
        case ValueKind::dvec3: return "dvec3";
        case ValueKind::entity: return "entity";
        default: break;
    }
    const char* name = ecs_get_name(world.c_ptr(), type);
    return name ? name : std::format("#{}", type);
}

double read_number(const World& world, ecs::entity_t type, const void* ptr) {
    if (const ecs::entity_t underlying = enum_underlying(world, type)) {
        type = underlying;
    }
    const EcsPrimitive* p = primitive_of(world, type);
    switch (p ? p->kind : EcsBool) {
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

void write_number(const World& world, ecs::entity_t type, void* ptr, double value) {
    if (const ecs::entity_t underlying = enum_underlying(world, type)) {
        type = underlying;
    }
    const EcsPrimitive* p = primitive_of(world, type);
    switch (p ? p->kind : EcsBool) {
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

std::optional<std::string> read_enum(const World& world, ecs::entity_t type, const void* ptr) {
    const EcsConstants* c = ecs_get(world.c_ptr(), type, EcsConstants);
    if (!c) {
        return std::nullopt;
    }
    const double value = read_number(world, type, ptr);
    const auto* constants = static_cast<const ecs_enum_constant_t*>(c->ordered_constants.array);
    for (std::int32_t i = 0; i < c->ordered_constants.count; ++i) {
        if (constant_value(world, type, constants[i]) == value) {
            return std::string(constants[i].name);
        }
    }
    return std::nullopt;
}

bool write_enum(const World& world, ecs::entity_t type, void* ptr, std::string_view name) {
    const EcsConstants* c = ecs_get(world.c_ptr(), type, EcsConstants);
    if (!c) {
        return false;
    }
    const auto* constants = static_cast<const ecs_enum_constant_t*>(c->ordered_constants.array);
    for (std::int32_t i = 0; i < c->ordered_constants.count; ++i) {
        if (name == constants[i].name) {
            write_number(world, type, ptr, constant_value(world, type, constants[i]));
            return true;
        }
    }
    return false;
}

bool is_reflected(const World& world, ecs::entity_t component) {
    const EcsComponent* c = ecs_get(world.c_ptr(), component, EcsComponent);
    if (!c || c->size == 0) {
        return true;  // tags carry no data
    }
    return ecs_has(world.c_ptr(), component, EcsType);
}

Result<> check_reflection(const World& world) {
    std::vector<std::string> missing;
    world.each([&](Entity e, const flecs::Component&) {
        const std::string path = e.path(".", "").c_str();
        // Module entities hold the module instance; they are not data components.
        if (path.starts_with("flecs.") || e.has(flecs::Module) || is_reflected(world, e.id())) {
            return;
        }
        missing.push_back(path);
    });
    if (missing.empty()) {
        return {};
    }
    std::string list;
    for (const std::string& name : missing) {
        list += list.empty() ? name : ", " + name;
    }
    return make_error(ErrorCode::ecs_unreflected_component, std::format("components without reflection: {}", list),
                      std::format("Reflect every member, e.g. world.component<T>().member(&T::field, \"field\"). "
                                  "Tools, scripts and schemas cannot see unreflected components."));
}

}  // namespace veach
