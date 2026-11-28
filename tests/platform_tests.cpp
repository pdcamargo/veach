#include "support/test_support.hpp"

#include <veach/platform/platform.hpp>
#include <veach/platform/platform_module.hpp>

#include <doctest/doctest.h>

using namespace veach;

namespace {

/// Records every call so tests can see what the platform module asked for.
class FakePlatform final : public Platform {
public:
    [[nodiscard]] PlatformBackend backend() const override { return PlatformBackend::headless; }
    [[nodiscard]] std::string_view name() const override { return "fake"; }
    [[nodiscard]] Result<std::uint32_t> create_window(const Window& desc) override {
        created.push_back(desc.title);
        return next_id++;
    }
    void destroy_window(std::uint32_t id) override { destroyed.push_back(id); }
    void update_window(std::uint32_t, const Window& desc, const Window&) override { updated.push_back(desc); }
    [[nodiscard]] NativeWindowHandle native_handle(std::uint32_t) const override { return {}; }
    void poll_events(std::vector<PlatformEvent>& out) override {
        out.insert(out.end(), pending.begin(), pending.end());
        pending.clear();
    }

    std::vector<std::string> created;
    std::vector<std::uint32_t> destroyed;
    std::vector<Window> updated;
    std::vector<PlatformEvent> pending;
    std::uint32_t next_id = 7;
};

struct PlatformFixture {
    PlatformFixture() {
        services.provide<Platform>(platform);
        attach_services(world, &services);
        world.import<CoreModule>();
        runner.register_phases(world);
        world.import<PlatformModule>();
    }
    // Member order matters: the world is destroyed first, while its services still exist.
    FakePlatform platform;
    Services services;
    World world;
    PhaseRunner runner;
};

}  // namespace

TEST_CASE("[platform.AC1] the Window component drives the OS window") {
    PlatformFixture f;
    const Entity window = f.world.entity().set<Window>({.title = "First"});
    sync_windows(f.world);
    REQUIRE(f.platform.created == std::vector<std::string>{"First"});
    CHECK(window.get<WindowPlatformState>().id == 7);

    window.set<Window>({.title = "Renamed", .width = 800});
    sync_windows(f.world);
    REQUIRE(f.platform.updated.size() == 1);
    CHECK(f.platform.updated[0].title == "Renamed");
    CHECK(f.platform.updated[0].width == 800);

    sync_windows(f.world);  // unchanged: no further OS calls
    CHECK(f.platform.updated.size() == 1);

    window.destruct();
    CHECK(f.platform.destroyed == std::vector<std::uint32_t>{7});
}

TEST_CASE("[platform.AC2] closing the primary window quits, OS resizes flow back into Window") {
    PlatformFixture f;
    const Entity window = f.world.entity().set<Window>({.title = "Main"}).add<PrimaryWindow>();
    sync_windows(f.world);

    f.platform.pending.push_back(
        {.type = PlatformEvent::Type::window_resized, .window = 7, .width = 640, .height = 360});
    f.runner.run(f.world, PhaseId::input, 0.0f);
    CHECK(window.get<Window>().width == 640);
    CHECK(f.platform.updated.empty());  // the OS-initiated change is not echoed back
    CHECK_FALSE(f.world.should_quit());

    f.platform.pending.push_back({.type = PlatformEvent::Type::window_close_requested, .window = 7});
    f.runner.run(f.world, PhaseId::input, 0.0f);
    CHECK(f.world.should_quit());
}

TEST_CASE("[platform.AC3] key events become input actions") {
    PlatformFixture f;
    bind_action(f.world, "jump", Key::space, Key::w);

    f.platform.pending.push_back({.type = PlatformEvent::Type::key_down, .key = Key::space});
    f.runner.run(f.world, PhaseId::input, 0.0f);
    CHECK(action_pressed(f.world, "jump"));
    CHECK(action_down(f.world, "jump"));

    f.runner.run(f.world, PhaseId::input, 0.0f);
    CHECK_FALSE(action_pressed(f.world, "jump"));  // only on the frame it went down
    CHECK(action_down(f.world, "jump"));

    f.platform.pending.push_back({.type = PlatformEvent::Type::key_up, .key = Key::space});
    f.runner.run(f.world, PhaseId::input, 0.0f);
    CHECK(action_released(f.world, "jump"));
    CHECK_FALSE(action_down(f.world, "jump"));
    CHECK_FALSE(action_pressed(f.world, "missing"));
}
