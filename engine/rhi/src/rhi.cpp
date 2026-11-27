#include <veach/rhi/rhi.hpp>

#include <veach/core/assert.hpp>
#include <veach/rhi/command_list.hpp>

#include <cstdlib>
#include <format>
#include <string_view>

#include "null/null_device.hpp"
#include "ray_tracing.hpp"
#if defined(__APPLE__)
#include "metal/metal_device.hpp"
#endif
#if defined(_WIN32)
#include "dx12/dx12_device.hpp"
#endif

namespace veach::rhi {

std::string_view to_string(Backend backend) {
    switch (backend) {
        case Backend::null: return "null";
        case Backend::metal: return "metal";
        case Backend::dx12: return "dx12";
    }
    return "?";
}

bool ray_tracing_disabled() {
    constexpr const char* name = "VEACH_DISABLE_RAY_TRACING";
#if defined(_MSC_VER)  // MSVC deprecates std::getenv
    char* value = nullptr;
    std::size_t length = 0;
    const bool one = _dupenv_s(&value, &length, name) == 0 && value != nullptr && std::string_view(value) == "1";
    std::free(value);  // NOLINT(cppcoreguidelines-no-malloc)
    return one;
#else
    const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read only
    return value != nullptr && std::string_view(value) == "1";
#endif
}

Result<std::unique_ptr<Device>> create_device(const DeviceDesc& desc) {
    switch (desc.backend) {
        case Backend::null: return std::unique_ptr<Device>(std::make_unique<NullDevice>(desc.frames_in_flight));
        case Backend::metal:
#if defined(__APPLE__)
            return create_metal_device(desc);
#else
            break;
#endif
        case Backend::dx12:
#if defined(_WIN32)
            return create_dx12_device(desc);
#else
            break;
#endif
    }
    return make_error(ErrorCode::rhi_backend_unavailable,
                      std::format("the '{}' RHI backend is not available on this platform", to_string(desc.backend)),
                      "Use \"rhi\": \"metal\" on macOS, \"dx12\" on Windows, or \"null\" anywhere.");
}

}  // namespace veach::rhi
