# Golden-image test (docs/specs/first-light/spec.md):
#   1. render with the lab:  veach-lab --rhi metal --headless --frames 3 --script <SCRIPT> --screenshot <OUT>
#   2. compare perceptually: veach imgdiff <GOLDEN> <OUT> --max-mean <MAX_MEAN>
# If the GPU cannot run the test (no Metal 3 / no ray tracing, e.g. GitHub's
# virtualized runners), print VEACH_GPU_UNAVAILABLE so CTest reports SKIPPED,
# unless VEACH_REQUIRE_GPU=1, in which case that is a failure.
# On Metal the image is rendered with Metal's shader validation off: with it on, ray queries that read instance data
# after a hit, or trace twice, return slightly different shadows from run to run (seen on the M5 Max, macOS 26).
# A second run with it on (debug builds' default) must still exit 0, so it keeps checking every resource access.
# Goldens are shared across backends (ADR 0028): GOLDEN is tests/golden/<name>.png unless a justified
# per-backend override tests/golden/<name>.<RHI>.png exists.
#   cmake -DLAB=... -DIMGDIFF=... -DRHI=metal -DSCRIPT=... -DGOLDEN=... -DOUT=... -DMAX_MEAN=... [-DFRAMES=3]
#         [-DMAX_P999=...] [-DMAX_OUTLIERS=...] -P golden_test.cmake

if(NOT DEFINED FRAMES)
    set(FRAMES 3)
endif()
set(pixel_limits "")
if(DEFINED MAX_P999)
    list(APPEND pixel_limits --max-p999 ${MAX_P999})
endif()
if(DEFINED MAX_OUTLIERS)
    list(APPEND pixel_limits --max-outliers ${MAX_OUTLIERS})
endif()
string(REGEX REPLACE "\\.png$" ".${RHI}.png" backend_golden "${GOLDEN}")
if(EXISTS "${backend_golden}")
    set(GOLDEN "${backend_golden}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env MTL_SHADER_VALIDATION=0
            "${LAB}" --rhi ${RHI} --headless --frames ${FRAMES} --script "${SCRIPT}" --screenshot "${OUT}"
    RESULT_VARIABLE render_result
    OUTPUT_VARIABLE render_output
    ERROR_VARIABLE render_output)

if(render_output MATCHES "rhi\\.backend_unavailable")
    string(REGEX MATCH "[^\n]*rhi\\.backend_unavailable[^\n]*" reason "${render_output}")
    string(REPLACE "rhi.backend_unavailable" "unsupported GPU" reason "${reason}")
    if("$ENV{VEACH_REQUIRE_GPU}" STREQUAL "1")
        message(FATAL_ERROR "VEACH_REQUIRE_GPU is set, but this GPU cannot run the test:\n${reason}")
    endif()
    message("VEACH_GPU_UNAVAILABLE: skipped. ${reason}")
    return()
endif()
if(NOT render_result EQUAL 0)
    message(FATAL_ERROR "rendering failed (exit ${render_result}):\n${render_output}")
endif()
if(RHI STREQUAL "metal")
    execute_process(
        COMMAND "${LAB}" --rhi ${RHI} --headless --frames ${FRAMES} --script "${SCRIPT}"
        RESULT_VARIABLE validated_result
        OUTPUT_VARIABLE validated_output
        ERROR_VARIABLE validated_output)
    if(NOT validated_result EQUAL 0)
        message(FATAL_ERROR "the run with Metal's shader validation failed (exit ${validated_result}):\n"
                            "${validated_output}")
    endif()
endif()

execute_process(
    COMMAND "${IMGDIFF}" imgdiff "${GOLDEN}" "${OUT}" --max-mean ${MAX_MEAN} ${pixel_limits} --diff "${OUT}.diff.png"
    RESULT_VARIABLE diff_result
    OUTPUT_VARIABLE diff_output
    ERROR_VARIABLE diff_output)
message("${diff_output}")
if(NOT diff_result EQUAL 0)
    message(FATAL_ERROR "The image differs from the golden ${GOLDEN}. Test image: ${OUT}, error map: ${OUT}.diff.png. "
                        "If the change is intended, regenerate with: cmake --build --preset <preset> --target update-goldens")
endif()
