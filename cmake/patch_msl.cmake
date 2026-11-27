# Works around two gaps in Slang 2026.22's Metal target (docs/adr/0009-shader-compilation.md, VCH-043). Each
# patch applies only when the MSL still has the gap, so a fixed Slang makes it a no-op.
#   1. It uses `RayDesc` without defining it: add the definition.
#   2. Its ray-flags-to-intersection-params helper ignores RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH (0x04), so a
#      query that should stop at its first hit searches for the closest one: map it to accept_any_intersection.
#   cmake -DMSL=<file.metal> -P patch_msl.cmake

file(READ "${MSL}" msl)
set(changed FALSE)

string(FIND "${msl}" "RayDesc" uses)
string(FIND "${msl}" "struct RayDesc" defines)
if(NOT uses EQUAL -1 AND defines EQUAL -1)
    set(definition "struct RayDesc\n{\n    float3 Origin;\n    float TMin;\n    float3 Direction;\n    float TMax;\n};\n")
    string(FIND "${msl}" "using namespace metal;" anchor)
    if(anchor EQUAL -1)
        message(FATAL_ERROR "patch_msl: no 'using namespace metal;' in ${MSL}; update cmake/patch_msl.cmake")
    endif()
    string(LENGTH "using namespace metal;\n" anchor_length)
    math(EXPR insert_at "${anchor} + ${anchor_length}")
    string(SUBSTRING "${msl}" 0 ${insert_at} head)
    string(SUBSTRING "${msl}" ${insert_at} -1 tail)
    set(msl "${head}${definition}${tail}")
    set(changed TRUE)
endif()

set(force_opaque "if (flags & 0x01) /* RAY_FLAG_FORCE_OPAQUE */")
string(FIND "${msl}" "${force_opaque}" helper)
string(FIND "${msl}" "accept_any_intersection" accepts)
if(NOT helper EQUAL -1 AND accepts EQUAL -1)
    string(REPLACE "${force_opaque}"
           "if (flags & 0x04) /* RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH */\n                    params.accept_any_intersection(true);\n                ${force_opaque}"
           msl "${msl}")
    set(changed TRUE)
endif()

if(changed)
    file(WRITE "${MSL}" "${msl}")
endif()
