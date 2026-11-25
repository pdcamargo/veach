# Golden-image test (docs/specs/first-light/spec.md):
#   1. render with the lab:  veach-lab --rhi metal --headless --frames 3 --script <SCRIPT> --screenshot <OUT>
#   2. compare perceptually: veach imgdiff <GOLDEN> <OUT> --max-mean <MAX_MEAN>
# If the GPU cannot run the test (no Metal 3 / no ray tracing, e.g. GitHub's
# virtualized runners), print VEACH_GPU_UNAVAILABLE so CTest reports SKIPPED,
# unless VEACH_REQUIRE_GPU is set, in which case that is a failure.
#   cmake -DLAB=... -DIMGDIFF=... -DSCRIPT=... -DGOLDEN=... -DOUT=... -DMAX_MEAN=... -P golden_test.cmake

execute_process(
    COMMAND "${LAB}" --rhi metal --headless --frames 3 --script "${SCRIPT}" --screenshot "${OUT}"
    RESULT_VARIABLE render_result
    OUTPUT_VARIABLE render_output
    ERROR_VARIABLE render_output)

if(render_output MATCHES "rhi\\.backend_unavailable")
    string(REGEX MATCH "[^\n]*rhi\\.backend_unavailable[^\n]*" reason "${render_output}")
    string(REPLACE "rhi.backend_unavailable" "unsupported GPU" reason "${reason}")
    if(DEFINED ENV{VEACH_REQUIRE_GPU})
        message(FATAL_ERROR "VEACH_REQUIRE_GPU is set, but this GPU cannot run the test:\n${reason}")
    endif()
    message("VEACH_GPU_UNAVAILABLE: skipped. ${reason}")
    return()
endif()
if(NOT render_result EQUAL 0)
    message(FATAL_ERROR "rendering failed (exit ${render_result}):\n${render_output}")
endif()

execute_process(
    COMMAND "${IMGDIFF}" imgdiff "${GOLDEN}" "${OUT}" --max-mean ${MAX_MEAN} --diff "${OUT}.diff.png"
    RESULT_VARIABLE diff_result
    OUTPUT_VARIABLE diff_output
    ERROR_VARIABLE diff_output)
message("${diff_output}")
if(NOT diff_result EQUAL 0)
    message(FATAL_ERROR "The image differs from the golden ${GOLDEN}. Test image: ${OUT}, error map: ${OUT}.diff.png. "
                        "If the change is intended, regenerate with: cmake --build --preset <preset> --target update-goldens")
endif()
