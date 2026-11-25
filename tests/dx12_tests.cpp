// DX12 backend (docs/specs/dx12-backend/spec.md). Windows only; hosted CI runs these on WARP (ADR 0031).

#include "support/test_support.hpp"

#include <veach/rhi/rhi.hpp>

#include <doctest/doctest.h>

using namespace veach;

namespace {

std::unique_ptr<rhi::Device> make_device() {
    auto device = rhi::create_device({.backend = rhi::Backend::dx12, .validation = true, .frames_in_flight = 2});
    REQUIRE_MESSAGE(device.has_value(), (device ? std::string() : device.error().message));
    return std::move(*device);
}

}  // namespace

TEST_CASE("[dx12.AC1] the DX12 device meets the feature floor and completes frames") {
    test::LogCapture logs;
    {
        const std::unique_ptr<rhi::Device> device = make_device();
        const rhi::Capabilities& caps = device->capabilities();
        MESSAGE("device '", caps.device_name, "', hardware RT: ", caps.hardware_ray_tracing);
        CHECK(caps.gpu);
        CHECK(caps.bindless);
        CHECK(caps.hardware_ray_tracing);  // WARP has DXR 1.1
        CHECK(device->frames_in_flight() == 2);

        rhi::SubmissionId last;
        for (int i = 0; i < 5; ++i) {
            const rhi::FrameInfo frame = device->begin_frame();
            CHECK(frame.index == static_cast<std::uint64_t>(i + 1));
            auto cmd = device->begin_commands("empty frame");
            REQUIRE(cmd != nullptr);
            const rhi::SubmissionId submission = device->submit(std::move(cmd));
            CHECK(submission.value > last.value);
            last = submission;
            device->end_frame();
        }
        device->wait_idle();
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}
