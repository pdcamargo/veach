// DX12 backend (docs/specs/dx12-backend/spec.md). Windows only; hosted CI runs these on WARP (ADR 0031).

#include "support/test_support.hpp"

#include <veach/rhi/rhi.hpp>

#include <doctest/doctest.h>

#include <array>
#include <span>

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

TEST_CASE("[dx12.AC2] descriptor slots are recycled once the GPU is done with them") {
    test::LogCapture logs;
    {
        const std::unique_ptr<rhi::Device> device = make_device();
        // 70,000 storage buffers with an SRV and a UAV each: 140,000 slots through a 65,536-slot heap.
        constexpr int frames = 70;
        constexpr int buffers_per_frame = 1'000;
        for (int f = 0; f < frames; ++f) {
            static_cast<void>(device->begin_frame());
            for (int i = 0; i < buffers_per_frame; ++i) {
                auto buffer = device->create_buffer({.size = 256, .debug_name = "churn", .stride = 16});
                REQUIRE_MESSAGE(buffer.has_value(), (buffer ? std::string() : buffer.error().message));
                const rhi::DescriptorHandle srv = device->descriptor(*buffer, rhi::View::read);
                const rhi::DescriptorHandle uav = device->descriptor(*buffer, rhi::View::storage);
                CHECK(srv.value != 0);
                CHECK(uav.value != 0);
                CHECK(srv.value != uav.value);
                device->destroy(*buffer);
            }
            // Textures with every view, a sampler, and a buffer with initial data (the upload path).
            auto texture = device->create_texture({64, 64, rhi::Format::rgba8_unorm,
                                                   rhi::TextureUsage::sampled | rhi::TextureUsage::storage |
                                                       rhi::TextureUsage::color_target | rhi::TextureUsage::copy_src,
                                                   "churn texture"});
            REQUIRE(texture.has_value());
            CHECK(device->texture_desc(*texture)->width == 64);
            auto sampler = device->create_sampler({});
            REQUIRE(sampler.has_value());
            const std::array<std::uint32_t, 4> data{1, 2, 3, 4};
            auto uploaded =
                device->create_buffer({.size = sizeof(data), .debug_name = "uploaded"}, std::as_bytes(std::span(data)));
            REQUIRE(uploaded.has_value());
            static_cast<void>(device->submit(device->begin_commands("churn frame")));
            device->destroy(*texture);
            device->destroy(*sampler);
            device->destroy(*uploaded);
            device->end_frame();
        }
        device->wait_idle();
        auto readback = device->create_buffer({.size = 64, .memory = rhi::MemoryType::readback, .debug_name = "rb"});
        REQUIRE(readback.has_value());
        CHECK(device->map(*readback) != nullptr);
        CHECK(device->descriptor(*readback).value == 0);  // readback buffers have no views
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}
