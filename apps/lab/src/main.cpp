// veach-lab: the prototype/test app.
//   veach-lab --script scripts/hello.luau --frames 3 --headless
//   veach-lab --window

#include "lab_module.hpp"

#include <veach/physics/module.hpp>
#include <veach/space/mcp_tools.hpp>
#include <veach/space/orrery.hpp>
#include <veach/veach.hpp>

int main(int argc, char** argv) {
    veach::Engine engine({.name = "veach-lab", .default_project = VEACH_LAB_PROJECT_DIR}, argc, argv);
    engine.import<veach::space::OrreryModule>();    // the space toolbox's orrery (spike SP-04, VCH-080)
    engine.import<veach::space::SpaceMcpModule>();  // MCP tools orrery_state and generate_system (VCH-100)
    engine.import<veach::PhysicsModule>();          // rigid bodies and characters, inspected with physics_* tools
    engine.import<lab::LabModule>();
    return engine.run();
}
