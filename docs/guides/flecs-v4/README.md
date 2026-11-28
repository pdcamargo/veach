# Flecs v4 in Veach

Veach pins **Flecs v4.2.0** ([ADR 0005](../../adr/0005-ecs-flecs-v4.md)). Always write v4 code:
most Flecs snippets online (and in model training data) are v3 and do not compile,
or compile and do the wrong thing. The upstream docs for exactly our version are
vendored in [upstream/](upstream/README.md).

Naming rule ([ADR 0006](../../adr/0006-flecs-barrel-not-wrapper.md)): inside `engine/` Flecs is
used directly. Outside `engine/` (apps, games) write the `veach::` aliases from
`<veach/ecs/flecs.hpp>` — `veach::World`, `veach::Entity`, `veach::Iter`,
`veach::Query<...>`, `veach::OnSet`, ... — and `veach::ecs::` for anything without
an alias. They are the same types; this is a barrel, not a wrapper.

## Idioms we use

### Components and reflection

Every component with data is reflected; the engine refuses to start otherwise
([ADR 0026](../../adr/0026-reflection-everywhere.md)). In v4.2 the member-pointer overload takes
the **pointer first**:

```cpp
struct Spinner {
    float radians_per_second = 1.0f;
    float angle = 0.0f;
};

world.component<Spinner>()
    .member(&Spinner::radians_per_second, "radians_per_second")
    .member(&Spinner::angle, "angle")
    .add<veach::Scriptable>();          // optional: expose to Luau
```

`std::string` members work: `CoreModule` registers `std::string` as an opaque
string type. Enums reflect automatically with `world.component<MyEnum>()`.

### Systems

Attach to a Veach phase with `.kind<veach::phase::X>()`. The engine runs phases;
you never do.

```cpp
world.system<Spinner>("Spin")
    .kind<veach::phase::Simulation>()
    .each([](veach::Iter& it, std::size_t, Spinner& s) {
        s.angle += s.radians_per_second * it.delta_time();   // delta_time() == 1/60 in Simulation
    });
```

`.run()` gives you the iterator; with terms you **must** loop `it.next()`:

```cpp
world.system<const Spinner>("Count").kind<veach::phase::PreExtract>().run([](veach::Iter& it) {
    while (it.next()) {
        auto spinners = it.field<const Spinner>(0);   // fields are 0-based in v4
        for (auto i : it) { /* spinners[i] */ }
    }
});
```

A `.run()` system with no terms runs once per phase invocation (no `it.next()` loop needed).

Within a phase, systems run in declaration order.

### Creating named entities inside a system: `immediate()`

Systems run on a read-only, staged world. Creating an entity with a **name**
(or calling `set_name`) asserts there. Mark such systems `immediate()`:

```cpp
world.system("SpawnSpinner").kind<veach::phase::Startup>().immediate().run([](veach::Iter& it) {
    it.world().entity("spinner").set<Spinner>({});
});
```

Unnamed `world.entity()`, `set`, `add` and `remove` are fine in normal (staged) systems.

### Singletons

Mark the component with the `Singleton` trait and use the world accessors:

```cpp
world.component<veach::Time>() /* .member(...) */ .add(veach::Singleton);
world.set<veach::Time>({});

const veach::Time& t = world.get<veach::Time>();       // reference; asserts it exists
veach::Time& m = world.get_mut<veach::Time>();          // mutable reference
const veach::Time* maybe = world.try_get<veach::Time>(); // nullptr if absent
```

The same `get` / `try_get` / `get_mut` / `try_get_mut` exist on entities.

### Observers

```cpp
world.observer<const veach::Window>("CloseOsWindow")
    .event(veach::OnRemove)
    .each([](veach::Entity e, const veach::Window&) { /* ... */ });
```

### Modules

A Veach module is a Flecs module ([ADR 0019](../../adr/0019-veach-modules-and-services.md)):
a struct whose constructor takes `veach::World&` and calls `world.module<Self>()` first.
Keep module structs empty (state goes in components or engine services); the exception is an engine area
whose state object lives and dies with the world, like `PhysicsModule::physics` (`veach::physics_world(world)`).
Import with `engine.import<MyModule>()`. See [../extending.md](../extending.md).

## v3 → v4 traps

| v3 habit | v4.2 reality |
|---|---|
| `world.filter<...>()`, `flecs::filter` | Filters are gone. Use `world.query<...>()` / `query_builder`; uncached by default. |
| `const T* p = e.get<T>()` | `get<T>()` returns `const T&` and asserts. Use `try_get<T>()` for a pointer that may be null. |
| `e.get_mut<T>()` returning a pointer that adds the component | `get_mut<T>()` returns `T&` (must exist); `ensure<T>()` adds if missing. |
| `it.term<T>(1)`, `term_id`, 1-based terms | `it.field<T>(0)`; field indices are 0-based. |
| `.member<float>("x")` only, or `.member("x", &T::x)` | `.member(&T::x, "x")` (pointer first) or `.member<float>("x")`. |
| `ecs_value_new(world, type)` returning `void*` | In 4.2 `ecs_value_*` works on `ecs_value_t`; raw pointers use `ecs_ptr_new` / `ecs_ptr_copy` / `ecs_ptr_free`. |
| `world.progress()` in your own loop | Never. The engine owns the loop and runs one pipeline per phase ([ADR 0018](../../adr/0018-engine-lifecycle-and-phases.md)). |
| Phases as `flecs::OnUpdate` etc. | Use `veach::phase::*`. They are deliberately not `flecs::Phase` entities, so Flecs's builtin pipeline never runs user systems. |

Reflection traps we hit:

- **Unsigned enums**: for an enum with an unsigned underlying type (`enum class X : std::uint8_t`),
  `ecs_enum_constant_t::value` is 0 and the real value is in `value_unsigned`. Use the
  helpers in `<veach/ecs/reflection.hpp>` (`read_enum`, `write_enum`) instead of reading constants yourself.
- **No enum reflection in anonymous namespaces**: Flecs derives constant names at compile time
  and cannot parse types declared in `namespace { }`; the enum ends up with no constants.
  Put reflected types (including test types) in a named namespace.
- **Module entities** carry the module instance as data; `check_reflection` skips entities
  tagged `flecs::Module`. Do not put data members in module structs.
