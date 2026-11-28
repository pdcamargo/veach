# Two scripts must render byte-identical images (docs/specs/frames/spec.md, frames.AC8: a scene inside a far frame is
# the scene at the origin, bit for bit). Renders each like golden_test.cmake (shader validation off), then compares the
# files. Skips like a golden test when the GPU cannot run it, unless VEACH_REQUIRE_GPU=1.
#   cmake -DLAB=... -DRHI=metal -DSCRIPT_A=... -DSCRIPT_B=... -DOUT=<prefix> [-DFRAMES=3] -P same_image_test.cmake

if(NOT DEFINED FRAMES)
    set(FRAMES 3)
endif()
set(ENV{MTL_SHADER_VALIDATION} 0)
foreach(which A B)
    execute_process(
        COMMAND "${LAB}" --rhi ${RHI} --headless --frames ${FRAMES} --script "${SCRIPT_${which}}"
                --screenshot "${OUT}.${which}.png"
        TIMEOUT 100
        RESULT_VARIABLE render_result
        OUTPUT_VARIABLE render_output
        ERROR_VARIABLE render_output)
    if(render_output MATCHES "rhi\\.backend_unavailable")
        if("$ENV{VEACH_REQUIRE_GPU}" STREQUAL "1")
            message(FATAL_ERROR "VEACH_REQUIRE_GPU is set, but this GPU cannot run the test")
        endif()
        message("VEACH_GPU_UNAVAILABLE: skipped.")
        return()
    endif()
    if(NOT render_result EQUAL 0)
        message(FATAL_ERROR "rendering ${SCRIPT_${which}} failed (exit ${render_result}):\n${render_output}")
    endif()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}.A.png" "${OUT}.B.png" RESULT_VARIABLE same)
if(NOT same EQUAL 0)
    message(FATAL_ERROR "${SCRIPT_A} and ${SCRIPT_B} rendered different images: ${OUT}.A.png, ${OUT}.B.png")
endif()
message("${SCRIPT_A} and ${SCRIPT_B}: byte-identical")
