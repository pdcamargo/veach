# Golden-image test (docs/specs/first-light/spec.md):
#   1. render with the lab:  veach-lab --rhi metal --headless --frames 3 --script <SCRIPT> --screenshot <OUT>
#   2. compare perceptually: veach imgdiff <GOLDEN> <OUT> --max-mean <MAX_MEAN>
# If the GPU cannot run the test (no Metal 3, e.g. GitHub's virtualized runners, or no ray tracing for photo
# mode), print VEACH_GPU_UNAVAILABLE so CTest reports SKIPPED, unless VEACH_REQUIRE_GPU=1, in which case that is a
# failure. NO_RAY_TRACING=1 renders with VEACH_DISABLE_RAY_TRACING=1 (the real-time renderer never needs it).
# On Metal the image is rendered with Metal's shader validation off: with it on, ray queries that read instance data
# after a hit, or trace twice, return slightly different shadows from run to run (seen on the M5 Max, macOS 26).
# A second run with it on (debug builds' default) must still exit 0, so it keeps checking every resource access.
# Goldens are shared across backends (ADR 0031): GOLDEN is tests/golden/<name>.png unless a justified
# per-backend override tests/golden/<name>.<RHI>.png exists.
# LAB_ENV is a |-separated list of VAR=value the lab runs with (both runs).
#   cmake -DLAB=... -DIMGDIFF=... -DRHI=metal -DSCRIPT=... -DGOLDEN=... -DOUT=... -DMAX_MEAN=... [-DFRAMES=3]
#         [-DMAX_P999=...] [-DMAX_OUTLIERS=...] [-DNO_RAY_TRACING=1] [-DLAB_TIMEOUT=105] [-DLAB_ENV=A=1|B=2]
#         -P golden_test.cmake

if(NOT DEFINED LAB_TIMEOUT)
    set(LAB_TIMEOUT 105)
endif()
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

# The lab is started directly (the environment is set here, not with `cmake -E env`), at debug log level, and given
# less time than CTest gives the whole test: if it hangs, it is the one process killed, and its log so far (each
# frame's render graph, the RHI's "still waiting for the GPU") is printed below instead of being lost (VCH-110).
if(NO_RAY_TRACING)
    set(ENV{VEACH_DISABLE_RAY_TRACING} 1)
endif()
if(DEFINED LAB_ENV)
    string(REPLACE "|" ";" env_pairs "${LAB_ENV}")
    foreach(pair IN LISTS env_pairs)
        string(FIND "${pair}" "=" eq)
        string(SUBSTRING "${pair}" 0 ${eq} var)
        math(EXPR value_start "${eq} + 1")
        string(SUBSTRING "${pair}" ${value_start} -1 value)
        set(ENV{${var}} "${value}")
    endforeach()
endif()
set(ENV{MTL_SHADER_VALIDATION} 0)
execute_process(
    COMMAND "${LAB}" --rhi ${RHI} --headless --frames ${FRAMES} --log-level debug --script "${SCRIPT}"
            --screenshot "${OUT}"
    TIMEOUT ${LAB_TIMEOUT}
    RESULT_VARIABLE render_result
    OUTPUT_VARIABLE render_output
    ERROR_VARIABLE render_output)
unset(ENV{MTL_SHADER_VALIDATION})

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
        TIMEOUT ${LAB_TIMEOUT}
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
