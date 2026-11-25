set(_rhi_sources
    src/rhi.cpp
    src/submission_tracker.cpp
    src/types.cpp
    src/null/null_device.cpp)

if(APPLE)
    list(APPEND _rhi_sources src/metal/metal_device.mm)
elseif(WIN32)
    list(APPEND _rhi_sources src/dx12/dx12_device.cpp)
endif()

veach_add_engine_library(rhi
    SOURCES ${_rhi_sources}
    # Only for NativeWindowHandle; the RHI never sees SDL (it is private to veach_platform).
    PUBLIC_DEPS veach::platform)

if(APPLE)
    set_source_files_properties(src/metal/metal_device.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wno-old-style-cast")
    target_link_libraries(veach_rhi PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Foundation")
elseif(WIN32)
    # Agility SDK headers first, so they win over the Windows SDK's older d3d12.h (ADR 0031).
    target_include_directories(veach_rhi SYSTEM BEFORE PRIVATE "${d3d12_agility_SOURCE_DIR}/build/native/include")
    target_compile_definitions(veach_rhi PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(veach_rhi PRIVATE d3d12 dxgi)
endif()
