#pragma once

// A transactional list of world writes (ADR 0006). Scripts never write the world
// directly: each script invocation records into a CommandBuffer, which is
// applied if the invocation succeeds and discarded if it raises an error.
// The same mechanism will back MCP `eval` and record/replay.
//
//   veach::CommandBuffer cmd(world);
//   auto e = cmd.spawn("Player");
//   cmd.set(e, Health{100});
//   cmd.apply();   // or cmd.discard(); the destructor discards

#include <veach/ecs/flecs.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace veach {

class CommandBuffer {
public:
    explicit CommandBuffer(World& world);
    ~CommandBuffer();
    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;
    CommandBuffer(CommandBuffer&&) = delete;
    CommandBuffer& operator=(CommandBuffer&&) = delete;

    /// Reserves an entity id immediately so callers can keep writing to it. The
    /// entity has no components (and no name) until apply(); discard() deletes it.
    Entity spawn(std::string name = {});
    void despawn(Entity entity);

    /// Records a write of a type-erased value. `value` must point to a valid
    /// instance of `component`'s type; it is copied now.
    void set(Entity entity, ecs::entity_t component, const void* value);
    void remove(Entity entity, ecs::entity_t component);

    template <class T>
    void set(Entity entity, const T& value) {
        set(entity, world_.id<T>(), &value);
    }
    template <class T>
    void remove(Entity entity) {
        remove(entity, world_.id<T>());
    }

    /// Applies all commands in recording order, then clears the buffer.
    void apply();
    /// Drops all commands and deletes entities reserved by spawn().
    void discard();

    [[nodiscard]] std::size_t size() const { return commands_.size(); }
    [[nodiscard]] bool empty() const { return commands_.empty(); }

private:
    struct Spawn {
        ecs::entity_t entity;
        std::string name;
    };
    struct Despawn {
        ecs::entity_t entity;
    };
    struct Set {
        ecs::entity_t entity;
        ecs::entity_t component;
        void* value;  ///< Owned; allocated with ecs_ptr_new, freed with ecs_ptr_free.
    };
    struct Remove {
        ecs::entity_t entity;
        ecs::entity_t component;
    };
    using Command = std::variant<Spawn, Despawn, Set, Remove>;

    void release_values();

    World world_;
    std::vector<Command> commands_;
};

}  // namespace veach
