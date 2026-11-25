# Slang shaders, compiled offline for every target (docs/adr/0026-shader-compilation.md).
#
#   veach_add_shader(<target> <file.slang> ENTRIES <entry>:<stage> [...])
#   veach_shader_library(<target> NAMESPACE <name>)
#
# Stages: vertex, fragment, compute. veach_shader_library() generates
# <name>.hpp/.cpp in the target's build dir with one accessor per shader file:
#   const veach::rhi::ShaderLibrary& <name>::<file stem>();

set(VEACH_SHADER_FLAGS -matrix-layout-column-major -warnings-as-errors all)

if(APPLE)
    set(VEACH_SLANGC "${slang_SOURCE_DIR}/bin/slangc")
    execute_process(COMMAND xcrun -sdk macosx -f metal RESULT_VARIABLE _metal_missing OUTPUT_QUIET ERROR_QUIET)
    if(_metal_missing)
        message(FATAL_ERROR "The Metal compiler (xcrun metal) was not found. Install Xcode (Command Line Tools alone "
                            "do not ship it) and run: sudo xcode-select -s /Applications/Xcode.app")
    endif()
elseif(WIN32)
    set(VEACH_SLANGC "${slang_SOURCE_DIR}/bin/slangc.exe")
    set(VEACH_DXCOMPILER "${dxc_SOURCE_DIR}/bin/x64/dxcompiler.dll")
endif()

function(veach_add_shader target file)
    cmake_parse_arguments(ARG "" "" "ENTRIES" ${ARGN})
    get_filename_component(source "${file}" ABSOLUTE)
    get_filename_component(stem "${file}" NAME_WE)
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY "${out_dir}")
    set(outputs "")
    set(entry_specs "")

    if(APPLE)
        set(entry_args "")
        foreach(entry_stage IN LISTS ARG_ENTRIES)
            string(REPLACE ":" ";" pair "${entry_stage}")
            list(GET pair 0 entry)
            list(GET pair 1 stage)
            list(APPEND entry_args -entry ${entry} -stage ${stage})
        endforeach()
        set(msl "${out_dir}/${stem}.metal")
        set(air "${out_dir}/${stem}.air")
        set(metallib "${out_dir}/${stem}.metallib")
        add_custom_command(
            OUTPUT "${metallib}"
            COMMAND "${VEACH_SLANGC}" "${source}" -target metal ${entry_args} ${VEACH_SHADER_FLAGS}
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
                COMMAND "${VEACH_SLANGC}" "${source}" -target dxil -profile sm_6_6 -entry ${entry} -stage ${stage}
                        ${VEACH_SHADER_FLAGS} -dxc-path "${VEACH_DXCOMPILER}" -depfile "${dxil}.d" -o "${dxil}"
                DEPENDS "${source}"
                DEPFILE "${dxil}.d"
                COMMENT "Slang -> DXIL: ${file} (${entry})"
                VERBATIM)
            list(APPEND outputs "${dxil}")
        endif()
        list(APPEND entry_specs "${stem}|${entry}|${stage}|${dxil}")
    endforeach()

    set_property(TARGET ${target} APPEND PROPERTY VEACH_SHADER_LIBRARIES "${stem}|${metallib}")
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
