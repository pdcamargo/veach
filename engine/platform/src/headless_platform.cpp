#include "headless_platform.hpp"

namespace veach {

Result<std::uint32_t> HeadlessPlatform::create_window(const Window& /*desc*/) {
    return next_id_++;
}

void HeadlessPlatform::destroy_window(std::uint32_t /*id*/) {}

void HeadlessPlatform::update_window(std::uint32_t /*id*/, const Window& /*desc*/, const Window& /*previous*/) {}

NativeWindowHandle HeadlessPlatform::native_handle(std::uint32_t /*id*/) const {
    return {};
}

void HeadlessPlatform::poll_events(std::vector<PlatformEvent>& /*out*/) {}

}  // namespace veach
