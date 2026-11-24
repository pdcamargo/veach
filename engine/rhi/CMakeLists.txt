set(_rhi_sources
    src/rhi.cpp
    src/null/null_device.cpp)

if(APPLE)
    list(APPEND _rhi_sources src/metal/metal_device.mm)
endif()
if(WIN32)
    list(APPEND _rhi_sources src/dx12/dx12_device.cpp)
endif()

veach_add_engine_library(rhi
    SOURCES ${_rhi_sources}
    # Only for NativeWindowHandle; the RHI never sees SDL (it is private to veach_platform).
    PUBLIC_DEPS veach::platform)

if(APPLE)
    set_source_files_properties(src/metal/metal_device.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wno-old-style-cast")
    target_link_libraries(veach_rhi PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Foundation")
endif()
