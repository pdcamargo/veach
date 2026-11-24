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
        case ValueKind::vec3: return "an array of 3 numbers";
        case ValueKind::enumeration: return "one of the enum constants";
        case ValueKind::structure: return "an object";
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
        case ValueKind::enumeration: return read_enum(world, type, ptr).value_or("");
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
            schema = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}};
            break;
        case ValueKind::enumeration: schema["enum"] = enum_constants(world, type); break;
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
