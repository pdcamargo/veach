# Two scripts must render byte-identical images (docs/specs/frames/spec.md, frames.AC8: a scene inside a far frame is
# the scene at the origin, bit for bit). Renders each like golden_test.cmake (shader validation off), then compares the
# files. Skips like a golden test when the GPU cannot run it, unless VEACH_REQUIRE_GPU=1. With MAX_MEAN (and IMGDIFF,
# veach-cli) the images need only agree within that mean FLIP: scenes that differ in float rounding (taa.AC4).
#   cmake -DLAB=... -DRHI=metal -DSCRIPT_A=... -DSCRIPT_B=... -DOUT=<prefix> [-DFRAMES=3] [-DIMGDIFF=... -DMAX_MEAN=x]
#         -P same_image_test.cmake

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
if(DEFINED MAX_MEAN)
    execute_process(COMMAND "${IMGDIFF}" imgdiff "${OUT}.A.png" "${OUT}.B.png" --max-mean ${MAX_MEAN}
                    RESULT_VARIABLE close OUTPUT_VARIABLE diff_output ERROR_VARIABLE diff_output)
    if(NOT close EQUAL 0)
        message(FATAL_ERROR "${SCRIPT_A} and ${SCRIPT_B} rendered different images (${OUT}.A.png, ${OUT}.B.png):\n"
                            "${diff_output}")
    endif()
    message("${SCRIPT_A} and ${SCRIPT_B}: ${diff_output}")
    return()
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}.A.png" "${OUT}.B.png" RESULT_VARIABLE same)
if(NOT same EQUAL 0)
    message(FATAL_ERROR "${SCRIPT_A} and ${SCRIPT_B} rendered different images: ${OUT}.A.png, ${OUT}.B.png")
endif()
message("${SCRIPT_A} and ${SCRIPT_B}: byte-identical")
