#include <veach/ecs/json.hpp>

#include <veach/core/math.hpp>
#include <veach/core/suggest.hpp>
#include <veach/ecs/reflection.hpp>

#include <nlohmann/json.hpp>

#include <format>
#include <string>

namespace veach {

namespace {

std::string_view json_type_name(const nlohmann::json& j) {
    return j.type_name();
}

std::string_view kind_name(ValueKind kind) {
    switch (kind) {
        case ValueKind::boolean: return "a boolean";
        case ValueKind::integer: return "an integer";
        case ValueKind::number: return "a number";
        case ValueKind::string: return "a string";
        case ValueKind::vec3:
        case ValueKind::dvec3: return "an array of 3 numbers";
        case ValueKind::enumeration: return "one of the enum constants";
        case ValueKind::structure: return "an object";
        case ValueKind::entity: return "an entity path string or null";
        case ValueKind::optional: return "a value or null";
        case ValueKind::unsupported: return "an unsupported type";
    }
    return "?";
}

std::unexpected<Error> mismatch(std::string_view context, std::string_view path, ValueKind expected,
                                const nlohmann::json& got) {
    return make_error(
        ErrorCode::data_type_mismatch,
        std::format("{}: field '{}' expects {}, got {}", context, path, kind_name(expected), json_type_name(got)),
        "Fix the value, or check the generated JSON Schema in schemas/.");
}

Result<> assign(const World& world, ecs::entity_t type, void* ptr, const nlohmann::json& j, std::string_view context,
                const std::string& path) {
    const ValueKind kind = value_kind(world, type);
    switch (kind) {
        case ValueKind::boolean:
            if (!j.is_boolean()) {
                return mismatch(context, path, kind, j);
            }
            *static_cast<bool*>(ptr) = j.get<bool>();
            return {};
        case ValueKind::integer:
            if (!j.is_number_integer()) {
                return mismatch(context, path, kind, j);
            }
            write_number(world, type, ptr, j.get<double>());
            return {};
        case ValueKind::number:
            if (!j.is_number()) {
                return mismatch(context, path, kind, j);
            }
            write_number(world, type, ptr, j.get<double>());
            return {};
        case ValueKind::string:
            if (!j.is_string()) {
                return mismatch(context, path, kind, j);
            }
            *static_cast<std::string*>(ptr) = j.get<std::string>();
            return {};
        case ValueKind::vec3: {
            if (!j.is_array() || j.size() != 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number()) {
                return mismatch(context, path, kind, j);
            }
            *static_cast<math::vec3*>(ptr) = math::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
            return {};
        }
        case ValueKind::dvec3: {
            if (!j.is_array() || j.size() != 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number()) {
                return mismatch(context, path, kind, j);
            }
            *static_cast<math::dvec3*>(ptr) = math::dvec3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>());
            return {};
        }
        case ValueKind::enumeration: {
            if (!j.is_string()) {
                return mismatch(context, path, kind, j);
            }
            const auto name = j.get<std::string>();
            if (!write_enum(world, type, ptr, name)) {
                const auto constants = enum_constants(world, type);
                std::string options;
                for (const auto& c : constants) {
                    options += options.empty() ? c : ", " + c;
                }
                std::string hint = did_you_mean(name, constants);
                hint += std::format("{}Valid values: {}.", hint.empty() ? "" : " ", options);
                return make_error(ErrorCode::data_type_mismatch,
                                  std::format("{}: field '{}' has unknown value '{}'", context, path, name), hint);
            }
            return {};
        }
        case ValueKind::structure: {
            if (!j.is_object()) {
                return mismatch(context, path, kind, j);
            }
            const auto fields = struct_fields(world, type);
            for (const auto& [key, value] : j.items()) {
                if (path.empty() && key == "$schema") {
                    continue;
                }
                const std::string field_path = path.empty() ? key : path + "." + key;
                const FieldInfo* field = nullptr;
                for (const auto& f : fields) {
                    if (f.name == key) {
                        field = &f;
                    }
                }
                if (!field) {
                    std::vector<std::string> names;
                    for (const auto& f : fields) {
                        names.push_back(f.name);
                    }
                    return make_error(
                        ErrorCode::data_unknown_field,
                        std::format("{}: unknown field '{}' in {}", context, field_path, type_name(world, type)),
                        did_you_mean(key, names));
                }
                if (auto r =
                        assign(world, field->type, static_cast<char*>(ptr) + field->offset, value, context, field_path);
                    !r) {
                    return r;
                }
            }
            return {};
        }
        case ValueKind::entity: {
            if (j.is_null()) {
                *static_cast<ecs::entity_t*>(ptr) = 0;
                return {};
            }
            if (!j.is_string()) {
                return mismatch(context, path, kind, j);
            }
            const Entity e = world.lookup(j.get<std::string>().c_str());
            if (!e) {
                return make_error(
                    ErrorCode::data_type_mismatch,
                    std::format("{}: field '{}' names no entity '{}'", context, path, j.get<std::string>()),
                    "Use the entity's full path, e.g. \"level::player\", or null.");
            }
            *static_cast<ecs::entity_t*>(ptr) = e.id();
            return {};
        }
        case ValueKind::optional: {
            const OptionalLayout layout = optional_layout(world, type);
            auto* present = static_cast<bool*>(static_cast<void*>(static_cast<char*>(ptr) + layout.present_offset));
            if (j.is_null()) {
                *present = false;
                return {};
            }
            *present = true;
            return assign(world, layout.value_type, static_cast<char*>(ptr) + layout.value_offset, j, context, path);
        }
        case ValueKind::unsupported: break;
    }
    return make_error(ErrorCode::data_type_mismatch,
                      std::format("{}: field '{}' has a type ({}) that JSON data files do not support yet", context,
                                  path, type_name(world, type)));
}

}  // namespace

Result<> from_json(const World& world, ecs::entity_t type, void* ptr, const nlohmann::json& json,
                   std::string_view context) {
    return assign(world, type, ptr, json, context, "");
}

nlohmann::json to_json(const World& world, ecs::entity_t type, const void* ptr) {
    switch (value_kind(world, type)) {
        case ValueKind::boolean: return *static_cast<const bool*>(ptr);
        case ValueKind::integer: return static_cast<std::int64_t>(read_number(world, type, ptr));
        case ValueKind::number: return read_number(world, type, ptr);
        case ValueKind::string: return *static_cast<const std::string*>(ptr);
        case ValueKind::vec3: {
            const auto& v = *static_cast<const math::vec3*>(ptr);
            return nlohmann::json::array({v.x, v.y, v.z});
        }
        case ValueKind::dvec3: {
            // nlohmann writes doubles with the shortest digits that read back to the same bits (ADR 0046).
            const auto& v = *static_cast<const math::dvec3*>(ptr);
            return nlohmann::json::array({v.x, v.y, v.z});
        }
        case ValueKind::enumeration: return read_enum(world, type, ptr).value_or("");
        case ValueKind::entity: {
            const auto id = *static_cast<const ecs::entity_t*>(ptr);
            if (id == 0 || !ecs_is_alive(world.c_ptr(), id)) {
                return nullptr;
            }
            return std::string(world.entity(id).path("::", "").c_str());
        }
        case ValueKind::optional: {
            const OptionalLayout layout = optional_layout(world, type);
            if (!*static_cast<const bool*>(
                    static_cast<const void*>(static_cast<const char*>(ptr) + layout.present_offset))) {
                return nullptr;
            }
            return to_json(world, layout.value_type, static_cast<const char*>(ptr) + layout.value_offset);
        }
        case ValueKind::structure: {
            nlohmann::json object = nlohmann::json::object();
            for (const auto& f : struct_fields(world, type)) {
                if (f.kind != ValueKind::unsupported) {
                    object[f.name] = to_json(world, f.type, static_cast<const char*>(ptr) + f.offset);
                }
            }
            return object;
        }
        case ValueKind::unsupported: break;
    }
    return nullptr;
}

nlohmann::json json_schema(const World& world, ecs::entity_t type, const void* defaults) {
    nlohmann::json schema;
    switch (value_kind(world, type)) {
        case ValueKind::boolean: schema["type"] = "boolean"; break;
        case ValueKind::integer: schema["type"] = "integer"; break;
        case ValueKind::number: schema["type"] = "number"; break;
        case ValueKind::string: schema["type"] = "string"; break;
        case ValueKind::vec3:
        case ValueKind::dvec3:
            schema = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}};
            break;
        case ValueKind::enumeration: schema["enum"] = enum_constants(world, type); break;
        case ValueKind::entity: schema["type"] = nlohmann::json::array({"string", "null"}); break;
        case ValueKind::optional: {
            const OptionalLayout layout = optional_layout(world, type);
            schema["anyOf"] = nlohmann::json::array({json_schema(world, layout.value_type), {{"type", "null"}}});
            break;
        }
        case ValueKind::structure: {
            schema["type"] = "object";
            schema["title"] = type_name(world, type);
            schema["additionalProperties"] = false;
            nlohmann::json properties = nlohmann::json::object();
            for (const auto& f : struct_fields(world, type)) {
                if (f.kind == ValueKind::unsupported) {
                    continue;
                }
                const void* field_default = defaults ? static_cast<const char*>(defaults) + f.offset : nullptr;
                properties[f.name] = json_schema(world, f.type, field_default);
            }
            schema["properties"] = properties;
            return schema;  // nested defaults are already on the properties
        }
        case ValueKind::unsupported: return schema;
    }
    if (defaults) {
        schema["default"] = to_json(world, type, defaults);
    }
    return schema;
}

}  // namespace veach
