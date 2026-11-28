# veach::procgen (docs/adr/0065-engine-areas-for-the-toolbox.md, docs/specs/procgen/spec.md): the generation stage
# runtime and cache, field graphs (IR, JSON loader, Luau builder, the canonical CPU interpreter) and the Luau
# generator sandbox. Canonical code (ADR 0043). It never links rhi or render (tools/scripts/check_repo.py): the GPU
# evaluator for field graphs, when it comes, is a render feature in engine/render that reads this area's op lists.
veach_add_engine_library(procgen
    SOURCES
        src/field_eval.cpp
        src/field_graph.cpp
        src/sandbox.cpp
    PUBLIC_DEPS veach::core
    PRIVATE_DEPS Luau.VM Luau.Compiler nlohmann_json::nlohmann_json)
