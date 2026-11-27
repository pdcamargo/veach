// Transforms and hierarchy (docs/specs/transforms/spec.md).

#include "support/test_support.hpp"

#include <veach/render/renderer.hpp>

#include <doctest/doctest.h>

using namespace veach;

namespace {

World make_world() {
    World world;
    world.import<CoreModule>();
    return world;
}

bool near(math::vec3 a, math::vec3 b) {
    return math::distance(a, b) < 1e-4f;
}

}  // namespace

TEST_CASE("[transform.AC1] a Transform brings a GlobalTransform that matches it at the root") {
    World world = make_world();
    const Transform local{{1.0f, 2.0f, 3.0f}, math::angleAxis(0.5f, math::world_up), {2.0f, 2.0f, 2.0f}};
    const Entity e = world.entity().set(local);
    REQUIRE(e.has<GlobalTransform>());
    propagate_transforms(world);
    const math::mat4 global = e.get<GlobalTransform>().matrix;
    const math::mat4 expected = local.matrix();
    for (int c = 0; c < 4; ++c) {
        CHECK(math::distance(global[c], expected[c]) < 1e-5f);
    }
}

TEST_CASE("[transform.AC2] children compose with their parents, any depth, in one pass") {
    World world = make_world();
    // Created child-first, so table order alone would get it wrong.
    const Entity parent = world.entity("parent");
    const Entity child = world.entity("child").child_of(parent);
    const Entity grandchild = world.entity("grandchild").child_of(child);
    grandchild.set(Transform{{0.0f, 0.0f, -1.0f}, math::quat(1, 0, 0, 0), {3.0f, 3.0f, 3.0f}});
    child.set(Transform{{0.0f, 0.0f, -2.0f}});
    parent.set(Transform{{10.0f, 0.0f, 0.0f}, math::angleAxis(math::radians(90.0f), math::world_up)});
    propagate_transforms(world);

    // The parent faces -X: its -Z steps go towards -X.
    CHECK(near(child.get<GlobalTransform>().translation(), {8.0f, 0.0f, 0.0f}));
    CHECK(near(grandchild.get<GlobalTransform>().translation(), {7.0f, 0.0f, 0.0f}));
    CHECK(math::length(math::vec3(grandchild.get<GlobalTransform>().matrix[0])) == doctest::Approx(3.0f));
    CHECK(near(grandchild.get<GlobalTransform>().rotation() * math::world_forward, {-1.0f, 0.0f, 0.0f}));
}

TEST_CASE("[transform.AC3] a rig moved in a per-frame phase carries its children the same frame") {
    test::TempProject project("transform");
    project.write("main.luau", R"(--!strict
local veach = require("@veach")
local Transform = veach.components.Transform
local rig = veach.spawn("rig")
rig:set(Transform, { translation = vector.create(0, 1, 0), rotation = veach.quat.identity(), scale = vector.one })
local eye = veach.spawn({ name = "eye", parent = rig })
eye:set(Transform, { translation = vector.create(0, 0, 2), rotation = veach.quat.identity(), scale = vector.one })
veach.system("MoveRig", "Input", veach.query(Transform), function(e, t)
	if e == rig then
		e:set(Transform, { translation = t.translation + vector.create(1, 0, 0), rotation = t.rotation, scale = t.scale })
	end
	if e == eye and veach.time.frame() == 2 then
		local parent = e:parent()
		local kids = rig:children()
		veach.log.info(`parent is rig: {parent == rig}, rig has {#kids} child, first is eye: {kids[1] == eye}`)
	end
end)
)");
    test::LogCapture logs;
    Engine engine(test::headless_config(project.path(), "main.luau", 2));
    REQUIRE(engine.init().has_value());
    REQUIRE(engine.start().has_value());
    REQUIRE(engine.frame());
    const Entity eye = engine.world().lookup("rig::eye");
    REQUIRE(eye.is_valid());
    // Frame 1: the rig moved once in Input (x = 1); its child follows before extraction.
    CHECK(near(eye.get<GlobalTransform>().translation(), {1.0f, 1.0f, 2.0f}));
    static_cast<void>(engine.frame());
    CHECK(near(eye.get<GlobalTransform>().translation(), {2.0f, 1.0f, 2.0f}));
    engine.shutdown();
    CHECK(logs.contains("parent is rig: true, rig has 1 child, first is eye: true"));
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}

TEST_CASE("[transform.AC4] quaternion helpers in Luau follow the conventions") {
    test::TempProject project("quat");
    project.write("main.luau", R"(--!strict
local veach = require("@veach")
local q = veach.quat
local function r(x: number): number
	return math.round(x * 1000) / 1000 + 0 -- + 0 turns -0 into 0
end
local function show(v: vector): string
	return string.format("%.3f %.3f %.3f", r(v.x), r(v.y), r(v.z))
end
local forward = vector.create(0, 0, -1)
veach.log.info("axis_angle " .. show(q.rotate(q.from_axis_angle(vector.create(0, 1, 0), math.pi / 2), forward)))
veach.log.info("euler " .. show(q.rotate(q.from_euler(math.pi / 2, 0), forward)))
veach.log.info("look " .. show(q.rotate(q.look_rotation(vector.create(1, 0, 0)), forward)))
local turn = q.from_euler(0.3, -0.2, 0.1)
local i = q.mul(q.inverse(turn), turn)
veach.log.info(string.format("identity %.3f %.3f %.3f %.3f", r(i.x), r(i.y), r(i.z), r(i.w)))
veach.log.info("pitch up " .. show(q.rotate(q.from_euler(0, math.pi / 2), forward)))
)");
    test::LogCapture logs;
    {
        Engine engine(test::headless_config(project.path(), "main.luau", 1));
        CHECK(engine.run() == 0);
    }
    CHECK(logs.contains("axis_angle -1.000 0.000 0.000"));
    CHECK(logs.contains("euler -1.000 0.000 0.000"));
    CHECK(logs.contains("look 1.000 0.000 0.000"));
    CHECK(logs.contains("identity 0.000 0.000 0.000 1.000"));
    CHECK(logs.contains("pitch up 0.000 1.000 0.000"));  // +pitch looks up

    test::TempProject bad("quat-bad");
    bad.write("main.luau", "local veach = require(\"@veach\")\nveach.quat.rotate({ x = 1 } :: any, vector.zero)\n");
    test::LogCapture bad_logs;
    Engine engine(test::headless_config(bad.path(), "main.luau", 1));
    CHECK(engine.run() == 1);
    CHECK(bad_logs.contains("a quat needs a number 'y'"));
    CHECK(bad_logs.contains("[script.invalid_value]"));
}

TEST_CASE("[transform.AC5] a camera on a rig views from the rig's world pose") {
    World world = make_world();
    world.component<Camera>();
    auto device = std::move(*rhi::create_device({}));
    render::Renderer renderer(*device);
    const Entity rig = world.entity("rig").set(Transform{{0.0f, 2.0f, 5.0f}});
    world.entity("camera").child_of(rig).set(Transform{{1.0f, 0.0f, 0.0f}}).set(Camera{});
    propagate_transforms(world);
    renderer.extract(world);
    REQUIRE(renderer.render_world().views.size() == 1);
    CHECK(near(renderer.render_world().views.front().position, {1.0f, 2.0f, 5.0f}));
}

TEST_CASE("propagate_transforms leaves no query behind") {
    // SP-03 found the helper built a new cached query on every call; a few hundred crashed world teardown.
    World world = make_world();
    world.entity().set(Transform{});
    const auto queries = [&] {
        return ecs_count_id(world.c_ptr(), ecs_pair(ecs_id(EcsPoly), EcsQuery));
    };
    propagate_transforms(world);
    const auto before = queries();
    for (int i = 0; i < 500; ++i) {
        propagate_transforms(world);
    }
    CHECK(queries() == before);
}
