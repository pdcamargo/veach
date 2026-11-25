#include <veach/ecs/command_buffer.hpp>

#include <veach/core/assert.hpp>

namespace veach {

CommandBuffer::CommandBuffer(World& world) : world_(world.c_ptr()) {}

CommandBuffer::~CommandBuffer() {
    discard();
}

Entity CommandBuffer::spawn(std::string name, ecs::entity_t parent) {
    const Entity entity = world_.entity();
    commands_.emplace_back(Spawn{entity.id(), std::move(name), parent});
    return entity;
}

std::vector<ecs::entity_t> CommandBuffer::spawned() const {
    std::vector<ecs::entity_t> out;
    for (const Command& command : commands_) {
        if (const auto* spawn = std::get_if<Spawn>(&command)) {
            out.push_back(spawn->entity);
        }
    }
    return out;
}

void CommandBuffer::despawn(Entity entity) {
    commands_.emplace_back(Despawn{entity.id()});
}

void CommandBuffer::set(Entity entity, ecs::entity_t component, const void* value) {
    const ecs_type_info_t* info = ecs_get_type_info(world_.c_ptr(), component);
    VEACH_ASSERT(info != nullptr, "set() on '{}', which is not a component with data",
                 world_.entity(component).path().c_str());
    void* copy = ecs_ptr_new(world_.c_ptr(), component);
    ecs_ptr_copy(world_.c_ptr(), component, copy, value);
    commands_.emplace_back(Set{entity.id(), component, copy});
}

void* CommandBuffer::emplace(Entity entity, ecs::entity_t component) {
    VEACH_ASSERT(ecs_get_type_info(world_.c_ptr(), component) != nullptr, "emplace() on a component without data");
    void* value = ecs_ptr_new(world_.c_ptr(), component);
    commands_.emplace_back(Set{entity.id(), component, value});
    return value;
}

void CommandBuffer::pop_back() {
    VEACH_ASSERT(!commands_.empty() && !std::holds_alternative<Spawn>(commands_.back()),
                 "pop_back() needs a non-spawn command");
    if (auto* set = std::get_if<Set>(&commands_.back()); set && set->value) {
        ecs_ptr_free(world_.c_ptr(), set->component, set->value);
    }
    commands_.pop_back();
}

void CommandBuffer::remove(Entity entity, ecs::entity_t component) {
    commands_.emplace_back(Remove{entity.id(), component});
}

void CommandBuffer::apply() {
    ecs_world_t* w = world_.c_ptr();
    for (Command& command : commands_) {
        std::visit(
            [&]<class C>(C& c) {
                if constexpr (std::is_same_v<C, Spawn>) {
                    // Parent first: a child's name lives in its parent's scope, so it may repeat elsewhere.
                    if (c.parent != 0 && ecs_is_alive(w, c.entity) && ecs_is_alive(w, c.parent)) {
                        ecs_add_pair(w, c.entity, EcsChildOf, c.parent);
                    }
                    if (!c.name.empty() && ecs_is_alive(w, c.entity)) {
                        ecs_set_name(w, c.entity, c.name.c_str());
                    }
                } else if constexpr (std::is_same_v<C, Despawn>) {
                    if (ecs_is_alive(w, c.entity)) {
                        ecs_delete(w, c.entity);
                    }
                } else if constexpr (std::is_same_v<C, Set>) {
                    if (ecs_is_alive(w, c.entity)) {
                        const ecs_type_info_t* info = ecs_get_type_info(w, c.component);
                        ecs_set_id(w, c.entity, c.component, static_cast<std::size_t>(info->size), c.value);
                    }
                } else if constexpr (std::is_same_v<C, Remove>) {
                    if (ecs_is_alive(w, c.entity)) {
                        ecs_remove_id(w, c.entity, c.component);
                    }
                }
            },
            command);
    }
    release_values();
    commands_.clear();
}

void CommandBuffer::discard() {
    ecs_world_t* w = world_.c_ptr();
    for (const Command& command : commands_) {
        if (const auto* spawn = std::get_if<Spawn>(&command); spawn && ecs_is_alive(w, spawn->entity)) {
            ecs_delete(w, spawn->entity);
        }
    }
    release_values();
    commands_.clear();
}

void CommandBuffer::release_values() {
    for (Command& command : commands_) {
        if (auto* set = std::get_if<Set>(&command); set && set->value) {
            ecs_ptr_free(world_.c_ptr(), set->component, set->value);
            set->value = nullptr;
        }
    }
}

}  // namespace veach
