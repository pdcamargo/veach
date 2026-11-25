# Works around Slang 2026.22's Metal target using `RayDesc` without defining it
# (docs/adr/0026-shader-compilation.md, VCH-043). Adds the definition only when
# the MSL uses RayDesc and does not define it, so a fixed Slang makes this a no-op.
#   cmake -DMSL=<file.metal> -P patch_msl.cmake

file(READ "${MSL}" msl)
string(FIND "${msl}" "RayDesc" uses)
string(FIND "${msl}" "struct RayDesc" defines)
if(uses EQUAL -1 OR NOT defines EQUAL -1)
    return()
endif()
set(definition "struct RayDesc\n{\n    float3 Origin;\n    float TMin;\n    float3 Direction;\n    float TMax;\n};\n")
string(FIND "${msl}" "using namespace metal;" anchor)
if(anchor EQUAL -1)
    message(FATAL_ERROR "patch_msl: no 'using namespace metal;' in ${MSL}; update cmake/patch_msl.cmake")
endif()
string(LENGTH "using namespace metal;\n" anchor_length)
math(EXPR insert_at "${anchor} + ${anchor_length}")
string(SUBSTRING "${msl}" 0 ${insert_at} head)
string(SUBSTRING "${msl}" ${insert_at} -1 tail)
file(WRITE "${MSL}" "${head}${definition}${tail}")
