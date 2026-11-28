# veach::voxel, spike SP-08 (VCH-084), throwaway: density chunks, octree LOD selection and the IsoMesher interface
# with two CPU meshers (docs/research/spikes/sp-08-mesher.md). ADR 0065's area, reduced to what the spike needs: it
# links core only (no ecs, procgen or physics yet) and never rhi or render. Canonical code (ADR 0043).
veach_add_engine_library(voxel
    SOURCES
        src/marching_cubes.cpp
        src/octree.cpp
        src/samples.cpp
        src/surface_nets.cpp
    PUBLIC_DEPS veach::core)
