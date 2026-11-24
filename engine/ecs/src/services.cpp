#include <veach/ecs/services.hpp>

namespace veach {

void attach_services(World& world, Services* registry) {
    world.set_ctx(registry);
}

Services& services(const World& world) {
    auto* registry = static_cast<Services*>(world.get_ctx());
    VEACH_ASSERT(registry != nullptr, "no Services attached to this world; worlds are created by veach::Engine");
    return *registry;
}

}  // namespace veach
