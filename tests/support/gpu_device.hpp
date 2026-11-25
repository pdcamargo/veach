#pragma once

// The platform's GPU device for tests that need one: Metal on macOS, DX12 on Windows (WARP in hosted CI).
// Without a capable GPU it returns null and the test skips, unless VEACH_REQUIRE_GPU=1 makes that a failure.

#include "support/test_support.hpp"

#include <veach/rhi/rhi.hpp>

#include <doctest/doctest.h>

#include <memory>

namespace veach::test {

inline std::unique_ptr<rhi::Device> gpu_device() {
#if defined(__APPLE__)
    constexpr auto backend = rhi::Backend::metal;
#elif defined(_WIN32)
    constexpr auto backend = rhi::Backend::dx12;
#else
    constexpr auto backend = rhi::Backend::null;
#endif
    auto device = rhi::create_device({.backend = backend, .validation = true});
    if (!device || !(*device)->capabilities().gpu) {
        REQUIRE_MESSAGE(!env_is_one("VEACH_REQUIRE_GPU"),
                        "VEACH_REQUIRE_GPU=1 but no GPU device: ", (device ? "null" : device.error().message));
        return nullptr;
    }
    return std::move(*device);
}

}  // namespace veach::test
