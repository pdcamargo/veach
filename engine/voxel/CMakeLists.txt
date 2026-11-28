# veach::voxel (docs/adr/0065-engine-areas-for-the-toolbox.md, docs/specs/voxel-terrain/spec.md, guide:
# docs/guides/voxel-terrain.md): density chunks on a body-local lattice, the CPU meshers, octree LOD selection,
# streaming with a budget, brush edits and the edit store, and the LOD0 collider meshes handed to physics. Canonical
# code (ADR 0043). It never links rhi or render (tools/scripts/check_repo.py): the render bridge is in
# engine/runtime (terrain.hpp), and physics implements voxel::ColliderSink.
veach_add_engine_library(voxel
    SOURCES
        src/density.cpp
        src/edit.cpp
        src/lod.cpp
        src/marching_cubes.cpp
        src/octree.cpp
        src/samples.cpp
        src/surface_nets.cpp
        src/terrain.cpp
    PUBLIC_DEPS veach::core veach::procgen)
