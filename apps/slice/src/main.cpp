// veach-slice: the first slice (docs/specs/first-slice/spec.md, VCH-115). A game on veach::veach and the space
// toolbox: a seed-generated star system, a craft in orbit of its planet, and the way down to dig a hole.
//   veach-slice --script scripts/slice_s0.01.luau --window
//   VEACH_SLICE_AUTOPILOT=1 veach-slice --script scripts/slice_s1.luau --rhi metal --headless --frames 20000

#include "slice.hpp"

#include <veach/veach.hpp>

int main(int argc, char** argv) {
    veach::Engine engine({.name = "veach-slice", .default_project = VEACH_SLICE_PROJECT_DIR}, argc, argv);
    slice::import_modules(engine);
    return engine.run();
}
