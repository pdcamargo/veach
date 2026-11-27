// veach-lab: the prototype/test app.
//   veach-lab --script scripts/hello.luau --frames 3 --headless
//   veach-lab --window

#include "lab_module.hpp"

#include <veach/space/orrery.hpp>
#include <veach/veach.hpp>

int main(int argc, char** argv) {
    veach::Engine engine({.name = "veach-lab", .default_project = VEACH_LAB_PROJECT_DIR}, argc, argv);
    engine.import<veach::space::OrreryModule>();  // the space toolbox's orrery (spike SP-04, VCH-080)
    engine.import<lab::LabModule>();
    return engine.run();
}
