#include <veach/platform/clock.hpp>
#include <veach/platform/platform.hpp>

#include "headless_platform.hpp"
#if defined(VEACH_HAS_SDL)
#include "sdl_platform.hpp"
#endif

#include <chrono>

namespace veach {

std::string_view to_string(PlatformBackend backend) {
    switch (backend) {
        case PlatformBackend::headless: return "headless";
        case PlatformBackend::sdl: return "sdl3";
    }
    return "?";
}

Result<std::unique_ptr<Platform>> create_platform(PlatformBackend backend) {
    switch (backend) {
        case PlatformBackend::headless: return std::make_unique<HeadlessPlatform>();
        case PlatformBackend::sdl:
#if defined(VEACH_HAS_SDL)
        {
            auto sdl = SdlPlatform::create();
            if (!sdl) {
                return std::unexpected(sdl.error());
            }
            return std::unique_ptr<Platform>(std::move(*sdl));
        }
#else
            return make_error(ErrorCode::platform_init_failed, "this build has no SDL3 backend (VEACH_SDL=OFF)",
                              "Run with --headless, or reconfigure with -DVEACH_SDL=ON.");
#endif
    }
    return make_error(ErrorCode::internal, "unknown platform backend");
}

namespace clock {

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace clock

}  // namespace veach
