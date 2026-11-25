# Slang shaders, compiled offline for every target (docs/adr/0026-shader-compilation.md).
#
#   veach_add_shader(<target> <file.slang> ENTRIES <entry>:<stage> [...] [INCLUDES <dir>...])
#   veach_shader_library(<target> NAMESPACE <name>)
#   veach_add_material(<target> <Name> <file.slang> [EMISSIVE])     (docs/adr/0033-material-model.md)
#   veach_add_scene_shaders(<target> NAMESPACE <name>)                 (docs/adr/0034-...; after the materials)
#
# Stages: vertex, fragment, compute. veach_shader_library() generates
# <name>.hpp/.cpp in the target's build dir with one accessor per shader file:
#   const veach::rhi::ShaderLibrary& <name>::<file stem>();

# 41017 (use of an uninitialized global) is off: veach/scene.slang's `veach_scene` is filled by each entry point
# before any use, which Slang's flow analysis cannot see across the entry point.
set(VEACH_SHADER_FLAGS -matrix-layout-column-major -warnings-as-errors all -warnings-disable 41017)
# Debug shaders check every bindless handle before use (veach/bindless.slang, ADR 0039).
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND VEACH_SHADER_FLAGS -DVEACH_VALIDATE_BINDLESS=1)
endif()

if(APPLE)
    set(VEACH_SLANGC "${slang_SOURCE_DIR}/bin/slangc")
    execute_process(COMMAND xcrun -sdk macosx -f metal RESULT_VARIABLE _metal_missing OUTPUT_QUIET ERROR_QUIET)
    if(_metal_missing)
        message(FATAL_ERROR "The Metal compiler (xcrun metal) was not found. Install Xcode (Command Line Tools alone "
                            "do not ship it) and run: sudo xcode-select -s /Applications/Xcode.app")
    endif()
elseif(WIN32)
    set(VEACH_SLANGC "${slang_SOURCE_DIR}/bin/slangc.exe")
    file(TO_NATIVE_PATH "${dxc_SOURCE_DIR}/bin/x64" VEACH_DXC_DIR)
endif()

function(veach_add_shader target file)
    cmake_parse_arguments(ARG "" "" "ENTRIES;INCLUDES" ${ARGN})
    # `import veach.material;` and friends resolve against the engine's shader directory.
    set(include_args -I "${PROJECT_SOURCE_DIR}/engine/render/shaders")
    foreach(dir IN LISTS ARG_INCLUDES)
        list(PREPEND include_args -I "${dir}")
    endforeach()
    get_filename_component(source "${file}" ABSOLUTE)
    get_filename_component(stem "${file}" NAME_WE)
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY "${out_dir}")
    set(outputs "")
    set(entry_specs "")
    set(entry_args "")
    foreach(entry_stage IN LISTS ARG_ENTRIES)
        string(REPLACE ":" ";" pair "${entry_stage}")
        list(GET pair 0 entry)
        list(GET pair 1 stage)
        list(APPEND entry_args -entry ${entry} -stage ${stage})
    endforeach()

    # Parameter-block layout, on every platform (HLSL source needs no DXC). It gives true byte offsets
    # for the C++ checks of VCH-041; embed_shaders.cmake turns it into <ns>::layout::<stem>. Every entry in
    # a file shares the one parameter block, so reflecting the first entry is enough.
    list(SUBLIST entry_args 0 4 first_entry_args)
    set(reflection "${out_dir}/${stem}.reflection.json")
    add_custom_command(
        OUTPUT "${reflection}"
        COMMAND "${VEACH_SLANGC}" "${source}" ${include_args} -target hlsl -profile sm_6_6 ${first_entry_args} ${VEACH_SHADER_FLAGS}
                -reflection-json "${reflection}" -depfile "${reflection}.d" -o "${out_dir}/${stem}.hlsl"
        DEPENDS "${source}"
        DEPFILE "${reflection}.d"
        COMMENT "Slang reflection: ${file}"
        VERBATIM)
    list(APPEND outputs "${reflection}")

    if(APPLE)
        set(msl "${out_dir}/${stem}.metal")
        set(air "${out_dir}/${stem}.air")
        set(metallib "${out_dir}/${stem}.metallib")
        add_custom_command(
            OUTPUT "${metallib}"
            COMMAND "${VEACH_SLANGC}" "${source}" ${include_args} -target metal ${entry_args} ${VEACH_SHADER_FLAGS}
                    -depfile "${msl}.d" -o "${msl}"
            COMMAND "${CMAKE_COMMAND}" -DMSL=${msl} -P "${PROJECT_SOURCE_DIR}/cmake/patch_msl.cmake"
            COMMAND xcrun -sdk macosx metal -std=metal3.1 -mmacosx-version-min=14.0 -c "${msl}" -o "${air}"
            COMMAND xcrun -sdk macosx metallib "${air}" -o "${metallib}"
            DEPENDS "${source}" "${PROJECT_SOURCE_DIR}/cmake/patch_msl.cmake"
            DEPFILE "${msl}.d"
            COMMENT "Slang -> Metal: ${file}"
            VERBATIM)
        list(APPEND outputs "${metallib}")
    else()
        set(metallib "")
    endif()

    foreach(entry_stage IN LISTS ARG_ENTRIES)
        string(REPLACE ":" ";" pair "${entry_stage}")
        list(GET pair 0 entry)
        list(GET pair 1 stage)
        set(dxil "")
        if(WIN32)
            set(dxil "${out_dir}/${stem}.${entry}.dxil")
            add_custom_command(
                OUTPUT "${dxil}"
                # slangc loads dxcompiler.dll (and dxil.dll, for signing) by name, so put the pinned DXC first on PATH.
                COMMAND "${CMAKE_COMMAND}" -E env --modify "PATH=path_list_prepend:${VEACH_DXC_DIR}"
                        "${VEACH_SLANGC}" "${source}" ${include_args} -target dxil -profile sm_6_6 -entry ${entry} -stage ${stage}
                        ${VEACH_SHADER_FLAGS} -depfile "${dxil}.d" -o "${dxil}"
                DEPENDS "${source}"
                DEPFILE "${dxil}.d"
                COMMENT "Slang -> DXIL: ${file} (${entry})"
                VERBATIM)
            list(APPEND outputs "${dxil}")
        endif()
        list(APPEND entry_specs "${stem}|${entry}|${stage}|${dxil}")
    endforeach()

    set_property(TARGET ${target} APPEND PROPERTY VEACH_SHADER_LIBRARIES "${stem}|${metallib}|${reflection}|${file}")
    set_property(TARGET ${target} APPEND PROPERTY VEACH_SHADER_ENTRIES ${entry_specs})
    set_property(TARGET ${target} APPEND PROPERTY VEACH_SHADER_OUTPUTS ${outputs})
endfunction()

function(veach_shader_library target)
    cmake_parse_arguments(ARG "" "NAMESPACE" "" ${ARGN})
    get_property(libraries TARGET ${target} PROPERTY VEACH_SHADER_LIBRARIES)
    get_property(entries TARGET ${target} PROPERTY VEACH_SHADER_ENTRIES)
    get_property(outputs TARGET ${target} PROPERTY VEACH_SHADER_OUTPUTS)
    set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(header "${gen_dir}/${ARG_NAMESPACE}.hpp")
    set(source "${gen_dir}/${ARG_NAMESPACE}.cpp")
    # Lists cross the command line joined with "@@" (";" would need fragile escaping).
    list(JOIN libraries "@@" libraries_arg)
    list(JOIN entries "@@" entries_arg)
    add_custom_command(
        OUTPUT "${header}" "${source}"
        COMMAND "${CMAKE_COMMAND}" "-DNAMESPACE=${ARG_NAMESPACE}" "-DHEADER=${header}" "-DSOURCE=${source}"
                "-DLIBRARIES=${libraries_arg}" "-DENTRIES=${entries_arg}"
                -P "${PROJECT_SOURCE_DIR}/cmake/embed_shaders.cmake"
        DEPENDS ${outputs} "${PROJECT_SOURCE_DIR}/cmake/embed_shaders.cmake"
        COMMENT "Embedding shaders: ${ARG_NAMESPACE}"
        VERBATIM)
    target_sources(${target} PRIVATE "${source}" "${header}")
    target_include_directories(${target} PRIVATE "${gen_dir}")
endfunction()

# A material kind (ADR 0033): <Name> is a Slang type in <file.slang> conforming to IMaterial. Kind 0 is always
# StandardMaterial; these follow in call order. EMISSIVE: its evalEmission may glow where the record's emissive
# factor is zero, so the path tracer samples its triangles as lights.
function(veach_add_material target name file)
    cmake_parse_arguments(ARG "EMISSIVE" "" "" ${ARGN})
    get_filename_component(source "${file}" ABSOLUTE)
    set_property(TARGET ${target} APPEND PROPERTY VEACH_MATERIALS "${name}|${source}|${ARG_EMISSIVE}")
endfunction()

# Compiles the reference path tracer into <target>'s shader library with its materials: generates
# veach/materials_generated.slang (the evaluateSurface switch) and <NAMESPACE>_materials.hpp (the kind table
# for render::SceneRenderer). Call after veach_add_material() and before veach_shader_library().
function(veach_add_scene_shaders target)
    cmake_parse_arguments(ARG "" "NAMESPACE" "" ${ARGN})
    get_property(materials TARGET ${target} PROPERTY VEACH_MATERIALS)
    set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(imports "")
    set(cases "")
    set(alpha_cases "")
    set(kinds "        {\"StandardMaterial\", false},\n")
    set(include_dirs "${gen_dir}")
    set(kind 1)
    foreach(material IN LISTS materials)
        string(REPLACE "|" ";" parts "${material}")
        list(GET parts 0 name)
        list(GET parts 1 source)
        list(GET parts 2 emissive)
        get_filename_component(stem "${source}" NAME_WE)
        get_filename_component(dir "${source}" DIRECTORY)
        list(APPEND include_dirs "${dir}")
        string(APPEND imports "import ${stem};\n")
        string(APPEND cases "    case ${kind}: return evaluate(${name}(), c);\n")
        string(APPEND alpha_cases "    case ${kind}: return ${name}().evalBaseColor(c).a;\n")
        if(emissive)
            string(APPEND kinds "        {\"${name}\", true},\n")
        else()
            string(APPEND kinds "        {\"${name}\", false},\n")
        endif()
        math(EXPR kind "${kind} + 1")
    endforeach()
    list(REMOVE_DUPLICATES include_dirs)
    file(CONFIGURE OUTPUT "${gen_dir}/veach/materials_generated.slang" CONTENT
"// Generated by veach_add_scene_shaders() (cmake/VeachShaders.cmake). Do not edit.
import veach.material;
${imports}
SurfaceSample evaluateSurface(uint kind, MaterialContext c) {
    switch (kind) {
${cases}    default: return evaluate(StandardMaterial(), c);
    }
}

/// Coverage only (alpha testing runs per candidate hit, so it must stay cheap).
float evaluateAlpha(uint kind, MaterialContext c) {
    switch (kind) {
${alpha_cases}    default: return StandardMaterial().evalBaseColor(c).a;
    }
}
" @ONLY)
    file(CONFIGURE OUTPUT "${gen_dir}/${ARG_NAMESPACE}_materials.hpp" CONTENT
"// Generated by veach_add_scene_shaders() (cmake/VeachShaders.cmake). Do not edit.
#pragma once

#include <veach/render/scene_renderer.hpp>

#include <array>

namespace ${ARG_NAMESPACE} {

/// Material kinds, in kind order: what render::SceneRenderer needs to resolve glTF material names.
inline constexpr std::array<veach::render::MaterialKind, ${kind}> material_kinds{{
${kinds}}};

}  // namespace ${ARG_NAMESPACE}
" @ONLY)
    veach_add_shader(${target} "${PROJECT_SOURCE_DIR}/engine/render/shaders/path_tracer.slang"
        ENTRIES trace_pixels:compute INCLUDES ${include_dirs})
    # The real-time renderer with the same materials (ADR 0037).
    veach_add_shader(${target} "${PROJECT_SOURCE_DIR}/engine/render/shaders/realtime_visibility.slang"
        ENTRIES vis_vs:vertex vis_fs:fragment INCLUDES ${include_dirs})
    veach_add_shader(${target} "${PROJECT_SOURCE_DIR}/engine/render/shaders/realtime_shade.slang"
        ENTRIES surface:compute restir_temporal:compute restir_spatial:compute shade:compute
        accumulate:compute INCLUDES ${include_dirs})
endfunction()
