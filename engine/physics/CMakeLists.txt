# veach::physics (docs/specs/physics/spec.md, docs/guides/physics.md; ADR 0058 with 0075, 0077, 0080, 0083): rigid
# bodies, characters and terrain colliders on Jolt Physics, one scene per reference frame. Jolt is PRIVATE: no public
# header names a Jolt type, and no file outside engine/physics includes one (tools/scripts/check_repo.py). Depends on
# core and ecs only and never links rhi or render (ADR 0065): it runs headless on any server.
veach_add_engine_library(physics
    SOURCES
        src/colliders.cpp
        src/jobs.cpp
        src/module.cpp
        src/runtime.cpp
        src/shape.cpp
        src/world.cpp
    PUBLIC_DEPS veach::core veach::ecs
    PRIVATE_DEPS Jolt)
# Jolt builds without RTTI (VeachDependencies.cmake), so a class derived from one of its classes with out-of-line
# virtuals (JobSystemWithBarrier) has no base typeinfo to link against: compile that file without RTTI too.
set_source_files_properties(src/jobs.cpp PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>")
