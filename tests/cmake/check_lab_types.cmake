# Regenerates the lab's Luau types with `veach-lab --write-types` and compares
# them with the committed file.
execute_process(COMMAND "${COMMAND}" --headless --write-types "${OUT}" --log-level warn RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "veach-lab --write-types failed (exit ${result})")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUT}" "${COMMITTED}" RESULT_VARIABLE different)
if(different)
    message(FATAL_ERROR "${COMMITTED} is out of date. Regenerate it with: "
                        "veach-lab --headless --write-types apps/lab/types/veach.d.luau")
endif()
