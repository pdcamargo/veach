# Windowed DX12 run on a hosted Windows runner (docs/specs/dx12-backend/spec.md, dx12.AC8):
#   veach-lab --window --rhi dx12 --frames <FRAMES> --script scripts/window_resize.luau --log-level debug
# Requires: the swapchain is created, the window is resized mid-run, shutdown is clean, and no error is logged,
# with one exception. The runner's display session makes some Present calls fail with E_INVALIDARG, which
# neither the D3D12 nor the DXGI debug layer explains. Those are tolerated while most presents succeed. Presenting
# on a real display is VCH-050 (blocked, ADR 0030).
#   cmake -DLAB=... -DFRAMES=30 -P windowed_test.cmake

execute_process(
    COMMAND "${LAB}" --window --rhi dx12 --frames ${FRAMES} --script scripts/window_resize.luau --log-level debug
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)

foreach(required "RHI surface 1 created" "resizing the window to 960x540" "shutdown core")
    string(FIND "${output}" "${required}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "missing '${required}' in the output:\n${output}")
    endif()
endforeach()

# CMake lists split on ";", which log lines may contain.
string(REPLACE ";" "," lines "${output}")
string(REGEX MATCHALL "[^\n]*\\] error [^\n]*" errors "${lines}")
set(tolerated 0)
foreach(line IN LISTS errors)
    if(line MATCHES "DXGI: Present failed \\(HRESULT 0x80070057\\)")
        math(EXPR tolerated "${tolerated} + 1")
    else()
        message(FATAL_ERROR "unexpected error:\n${line}\n\nfull output:\n${output}")
    endif()
endforeach()
math(EXPR limit "${FRAMES} / 2")
if(tolerated GREATER limit)
    message(FATAL_ERROR "${tolerated} of ${FRAMES} presents failed (more than half):\n${output}")
endif()
message("windowed DX12 run ok: ${tolerated} of ${FRAMES} presents failed with E_INVALIDARG (runner display session)")
