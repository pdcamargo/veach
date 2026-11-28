#include <veach/script/luau_types.hpp>

#include <veach/ecs/components.hpp>
#include <veach/ecs/phases.hpp>
#include <veach/ecs/reflection.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace veach {

namespace {

// The fixed part of the API. Keep in sync with script_runtime.cpp; the test
// "generated types cover every binding" fails if a binding is missing here.
constexpr std::string_view api_prelude = R"(export type Entity = {
	read id: (self: Entity) -> number,
	read get: <T>(self: Entity, component: Component<T>) -> T?,
	read set: <T>(self: Entity, component: Component<T>, value: T) -> (),
	read has: <T>(self: Entity, component: Component<T>) -> boolean,
	read remove: <T>(self: Entity, component: Component<T>) -> (),
	read despawn: (self: Entity) -> (),
	--- The parent (ChildOf), or nil at the root.
	read parent: (self: Entity) -> Entity?,
	--- The children, in id order. Reads the world now: children spawned in this invocation are not there yet.
	read children: (self: Entity) -> { Entity },
}

--- A component handle. `defaults` holds the value new components start from.
export type Component<T> = { read name: string, read defaults: T }

--- A query over 1 to 4 components. `_row` exists only for the type checker.
export type Query<T...> = { read components: { Component<any> }, read _row: (Entity, T...) -> () }

export type QueryFn =
	(<A>(a: Component<A>) -> Query<A>)
	& (<A, B>(a: Component<A>, b: Component<B>) -> Query<A, B>)
	& (<A, B, C>(a: Component<A>, b: Component<B>, c: Component<C>) -> Query<A, B, C>)
	& (<A, B, C, D>(a: Component<A>, b: Component<B>, c: Component<C>, d: Component<D>) -> Query<A, B, C, D>)

export type LogApi = {
	read debug: (message: string) -> (),
	read info: (message: string) -> (),
	read warn: (message: string) -> (),
	read error: (message: string) -> (),
}

--- Quaternion helpers over { x, y, z, w } tables (ADR 0013: right-handed, +Y up, -Z forward, radians).
export type QuatApi = {
	read identity: () -> quat,
	read from_axis_angle: (axis: vector, angle: number) -> quat,
	--- Yaw about +Y, then pitch about the turned +X, then roll about the turned -Z.
	read from_euler: (yaw: number, pitch: number, roll: number?) -> quat,
	--- a * b: rotates by b first, then by a.
	read mul: (a: quat, b: quat) -> quat,
	read rotate: (q: quat, v: vector) -> vector,
	read inverse: (q: quat) -> quat,
	read normalize: (q: quat) -> quat,
	--- The rotation that turns -Z to `forward`, with +Y as close to `up` (default +Y) as possible.
	read look_rotation: (forward: vector, up: vector?) -> quat,
}

--- A point relative to a reference frame, in double precision (ADR 0046). Position + vector and Position - vector
--- give a Position; Position - Position gives the vector between them. Positions don't add or scale.
type PositionFields = { read x: number, read y: number, read z: number }
type PositionOps = {
	__add: (Position, vector) -> Position,
	__sub: ((Position, Position) -> vector) & ((Position, vector) -> Position),
	__eq: (Position, Position) -> boolean,
}
export type Position = typeof(setmetatable({} :: PositionFields, {} :: PositionOps))

--- Reference frames (docs/specs/frames/spec.md). A frame is an entity with ReferenceFrame; nil is the root world.
export type FramesApi = {
	--- The frame an entity lives in (its nearest ReferenceFrame ancestor; a frame's parent frame), nil for the root.
	read of: (entity: Entity) -> Entity?,
	--- A position given in frame `from`, in frame `to`, through their lowest common ancestor in double.
	read convert: (position: Position, from: Entity?, to: Entity?) -> Position,
	--- Where `to` is relative to `from`, in the frame `from` lives in.
	read offset: (from: Entity, to: Entity) -> vector,
	--- "Up" at a position: +Y in the root world and axis_y frames, away from the origin in radial frames.
	read up: (frame: Entity?, position: Position) -> vector,
}

export type SpawnOptions = { name: string?, parent: Entity? }

--- Input actions, bound in veach.project.json: keys under "input" ({ "jump": ["space"] }), analog actions under
--- "axes" ({ "look_x": { "source": "mouse_x", "sensitivity": 0.003 } }). An unknown name is an error.
--- pressed/released hold for the whole frame: read them in a per-frame phase ("Input") to act once.
export type InputApi = {
	read down: (action: string) -> boolean,
	read pressed: (action: string) -> boolean,
	read released: (action: string) -> boolean,
	--- `axis("look_x")`: an analog action's value (mouse motion since the last fixed step, a stick, a trigger).
	--- `axis("move_back", "move_forward")`: -1, 0 or 1 from two key actions.
	read axis: ((action: string) -> number) & ((negative: string, positive: string) -> number),
	--- How many gamepads are connected.
	read gamepads: () -> number,
}

export type TimeApi = {
	--- Current frame number, starting at 1.
	read frame: () -> number,
	--- Seconds covered by the current phase (the fixed step, 1/60, during simulation phases).
	read delta: () -> number,
	--- Simulated seconds since start.
	read elapsed: () -> number,
}
)";

std::string luau_type(const World& world, ecs::entity_t type) {
    switch (value_kind(world, type)) {
        case ValueKind::boolean: return "boolean";
        case ValueKind::integer:
        case ValueKind::number: return "number";
        case ValueKind::string: return "string";
        case ValueKind::vec3: return "vector";
        case ValueKind::dvec3: return "Position";
        case ValueKind::enumeration:
        case ValueKind::structure: return type_name(world, type);
        case ValueKind::entity: return "Entity?";
        case ValueKind::optional: {
            const std::string inner = luau_type(world, optional_layout(world, type).value_type);
            return inner.ends_with('?') ? inner : inner + "?";
        }
        case ValueKind::unsupported: return "never";
    }
    return "never";
}

/// Collects named enum/struct types reachable from `type`, dependencies first.
void collect(const World& world, ecs::entity_t type, std::vector<ecs::entity_t>& order, std::set<ecs::entity_t>& seen) {
    const ValueKind kind = value_kind(world, type);
    if (kind == ValueKind::optional) {
        collect(world, optional_layout(world, type).value_type, order, seen);
        return;
    }
    if ((kind != ValueKind::structure && kind != ValueKind::enumeration) || seen.contains(type)) {
        return;
    }
    seen.insert(type);
    if (kind == ValueKind::structure) {
        for (const FieldInfo& f : struct_fields(world, type)) {
            collect(world, f.type, order, seen);
        }
    }
    order.push_back(type);
}

}  // namespace

std::string generate_luau_types(const World& world) {
    // Scriptable components, sorted by name for stable output.
    std::map<std::string, ecs::entity_t> components;
    const ecs::entity_t scriptable = world.id<Scriptable>();
    world.each([&](Entity e, const flecs::Component&) {
        if (e.has(scriptable) && value_kind(world, e.id()) == ValueKind::structure) {
            components.emplace(e.name().c_str(), e.id());
        }
    });

    std::vector<ecs::entity_t> order;
    std::set<ecs::entity_t> seen;
    for (const auto& [name, id] : components) {
        collect(world, id, order, seen);
    }

    std::string out;
    out += "--!strict\n";
    out += "-- Generated by `veach types` from Flecs reflection. Do not edit by hand.\n";
    out += "-- Regenerate with: veach types --out types/veach.d.luau\n\n";

    out += "export type Phase =";
    bool first = true;
    for (const PhaseInfo& info : phases()) {
        out += std::format("{}\"{}\"", first ? " " : " | ", info.name);
        first = false;
    }
    out += "\n\n";
    out += api_prelude;

    if (!order.empty()) {
        out += "\n-- Reflected components and their types (C++ components tagged veach::Scriptable).\n";
    }
    for (const ecs::entity_t type : order) {
        const std::string name = type_name(world, type);
        if (value_kind(world, type) == ValueKind::enumeration) {
            out += std::format("export type {} =", name);
            first = true;
            for (const std::string& constant : enum_constants(world, type)) {
                out += std::format("{}\"{}\"", first ? " " : " | ", constant);
                first = false;
            }
            out += "\n";
            continue;
        }
        out += std::format("export type {} = {{\n", name);
        for (const FieldInfo& f : struct_fields(world, type)) {
            if (f.kind != ValueKind::unsupported) {
                out += std::format("\t{}: {},\n", f.name, luau_type(world, f.type));
            }
        }
        out += "}\n";
    }

    out += R"(
export type Veach = {
	--- Defines a script component. Each field's type comes from its default (boolean, number = f64,
	--- string, vector = vec3, Position = dvec3) or from an explicit type: veach.f32(100), veach.entity(),
	--- veach.optional(...).
	read component: <T>(name: string, defaults: T) -> Component<T>,
	--- Explicit field types, for what a default value cannot express. Each one type-checks as the
	--- value it stores, so `{ hp = veach.f32(100) }` is `{ hp: number }`.
	read f32: (default: number?) -> number,
	read f64: (default: number?) -> number,
	read i32: (default: number?) -> number,
	read u32: (default: number?) -> number,
	read vec3: (default: vector?) -> vector,
	--- An entity reference. Starts as nil (no entity); reads as nil once the entity is despawned.
	read entity: () -> Entity?,
	--- An optional field. Starts absent (nil).
	read optional: <T>(inner: T) -> T?,
	read query: QueryFn,
	--- Registers a system. `fn` runs once per matching entity, every time the phase runs.
	read system: <T...>(name: string, phase: Phase, query: Query<T...>, fn: (Entity, T...) -> ()) -> (),
	--- Spawns an entity, optionally as a child of `parent`. It gets its components (and its parent) when the
	--- current invocation succeeds.
	read spawn: ((name: string?) -> Entity) & ((options: SpawnOptions) -> Entity),
	read quat: QuatApi,
	--- A Position from three doubles, or from a vector.
	read position: ((x: number, y: number, z: number) -> Position) & ((v: vector) -> Position),
	read frames: FramesApi,
	read input: InputApi,
	read log: LogApi,
	read time: TimeApi,
	read components: {
)";
    for (const auto& [name, id] : components) {
        out += std::format("\t\tread {}: Component<{}>,\n", name, name);
    }
    out += R"(	},
}

local veach: Veach = (nil :: any)
return veach
)";
    return out;
}

}  // namespace veach
