#include <veach/render/renderer.hpp>

#include <veach/core/profile.hpp>
#include <veach/ecs/components.hpp>
#include <veach/ecs/phases.hpp>
#include <veach/ecs/services.hpp>
#include <veach/platform/window.hpp>

namespace veach::render {

Renderer::Renderer(rhi::Device& device) : device_(device) {
    add_extractor("frame", [](const World& world, RenderWorld& out) {
        if (const Time* time = world.try_get<Time>()) {
            out.frame = time->frame;
        }
    });
    add_extractor("windows",
                  [](const World& world, RenderWorld& out) { world.each([&](const Window&) { ++out.window_count; }); });
}

void Renderer::add_extractor(std::string name, Extractor extractor) {
    extractors_.push_back({std::move(name), std::move(extractor)});
}

void Renderer::extract(const World& world) {
    VEACH_ZONE("Renderer::extract");
    render_world_ = {};
    for (const NamedExtractor& extractor : extractors_) {
        VEACH_ZONE_TEXT("extractor", extractor.name.data(), extractor.name.size());
        extractor.fn(world, render_world_);
    }
}

void Renderer::render() {
    VEACH_ZONE("Renderer::render");
}

RenderModule::RenderModule(World& world) {
    world.module<RenderModule>("veach::render");
    world.system("Extract").kind<phase::Extract>().run([](Iter& it) {
        const World w = it.world();
        services(w).get<Renderer>().extract(w);
    });
    world.system("Render").kind<phase::Render>().run([](Iter& it) { services(it.world()).get<Renderer>().render(); });
}

}  // namespace veach::render
