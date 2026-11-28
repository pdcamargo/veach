#include "position.hpp"

#include "marshal.hpp"

#include <lualib.h>

#include <format>
#include <iterator>
#include <string>

namespace veach::script {

namespace {

constexpr const char* use_position_hint = "Positions are doubles: build one with veach.position(x, y, z).";

void append_number(std::string& out, double value) {
    std::format_to(std::back_inserter(out), "{}", value);  // the shortest digits that read back to the same double
}

math::dvec3 vector_at(lua_State* L, int index) {
    const float* v = lua_tovector(L, index);
    return {static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2])};
}

int position_index(lua_State* L) {
    const math::dvec3& p = *to_position(L, 1);
    std::size_t len = 0;
    const char* key = lua_tolstring(L, 2, &len);
    if (key != nullptr && len == 1 && key[0] >= 'x' && key[0] <= 'z') {
        lua_pushnumber(L, p[key[0] - 'x']);
        return 1;
    }
    raise(L, ErrorCode::script_invalid_value,
          std::format("Position has no field '{}'", key != nullptr ? key : luaL_typename(L, 2)),
          "A Position has the fields x, y and z.");
}

int position_add(lua_State* L) {
    const math::dvec3* a = to_position(L, 1);
    const math::dvec3* b = to_position(L, 2);
    if (a != nullptr && lua_isvector(L, 2)) {
        push_position(L, *a + vector_at(L, 2));
        return 1;
    }
    if (b != nullptr && lua_isvector(L, 1)) {
        push_position(L, *b + vector_at(L, 1));
        return 1;
    }
    raise(L, ErrorCode::script_invalid_value,
          std::format("cannot add {} and {}", a != nullptr ? "Position" : luaL_typename(L, 1),
                      b != nullptr ? "Position" : luaL_typename(L, 2)),
          "Add a vector to a Position (an offset). Two positions don't add: subtract them to get the vector between.");
}

int position_sub(lua_State* L) {
    const math::dvec3* a = to_position(L, 1);
    const math::dvec3* b = to_position(L, 2);
    if (a != nullptr && b != nullptr) {
        // The difference in double, then float32: relative precision survives, however far both are from the origin.
        const math::vec3 d(*a - *b);
        lua_pushvector(L, d.x, d.y, d.z);
        return 1;
    }
    if (a != nullptr && lua_isvector(L, 2)) {
        push_position(L, *a - vector_at(L, 2));
        return 1;
    }
    raise(L, ErrorCode::script_invalid_value,
          std::format("cannot subtract {} from {}", b != nullptr ? "a Position" : luaL_typename(L, 2),
                      a != nullptr ? "a Position" : luaL_typename(L, 1)),
          "Position - Position gives a vector, Position - vector a Position.");
}

int position_eq(lua_State* L) {
    const math::dvec3* a = to_position(L, 1);
    const math::dvec3* b = to_position(L, 2);
    lua_pushboolean(L, a != nullptr && b != nullptr && *a == *b ? 1 : 0);
    return 1;
}

int position_tostring(lua_State* L) {
    const math::dvec3& p = *to_position(L, 1);
    std::string out = "Position(";
    append_number(out, p.x);
    out += ", ";
    append_number(out, p.y);
    out += ", ";
    append_number(out, p.z);
    out += ")";
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

}  // namespace

void open_position(lua_State* L) {
    lua_createtable(L, 0, 6);
    lua_pushcfunction(L, position_index, "__index");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, position_add, "__add");
    lua_setfield(L, -2, "__add");
    lua_pushcfunction(L, position_sub, "__sub");
    lua_setfield(L, -2, "__sub");
    lua_pushcfunction(L, position_eq, "__eq");
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, position_tostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    lua_pushstring(L, "Position");
    lua_setfield(L, -2, "__type");
    lua_setreadonly(L, -1, 1);
    lua_setuserdatametatable(L, position_tag);  // pops; new Position userdata get it for free
}

void push_position(lua_State* L, const math::dvec3& p) {
    auto* data = static_cast<double*>(lua_newuserdatataggedwithmetatable(L, 3 * sizeof(double), position_tag));
    data[0] = p.x;
    data[1] = p.y;
    data[2] = p.z;
}

const math::dvec3* to_position(lua_State* L, int index) {
    // dvec3 is three tightly packed doubles (glm), so the payload is one.
    static_assert(sizeof(math::dvec3) == 3 * sizeof(double));
    return static_cast<const math::dvec3*>(lua_touserdatatagged(L, index, position_tag));
}

math::dvec3 check_position(lua_State* L, int index, const char* what) {
    const math::dvec3* p = to_position(L, index);
    if (p == nullptr) {
        raise(L, ErrorCode::script_invalid_value,
              std::format("{} expects a Position, got {}", what, luaL_typename(L, index)), use_position_hint);
    }
    return *p;
}

int api_position(lua_State* L) {
    if (lua_isvector(L, 1)) {
        push_position(L, vector_at(L, 1));
        return 1;
    }
    const double x = luaL_checknumber(L, 1);
    const double y = luaL_checknumber(L, 2);
    const double z = luaL_checknumber(L, 3);
    push_position(L, {x, y, z});
    return 1;
}

}  // namespace veach::script
