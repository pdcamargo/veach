// DX12 backend (docs/specs/dx12-backend/spec.md). Windows only; hosted CI runs these on WARP (ADR 0033).

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

TEST_CASE("[dx12.AC3] a cleared target reads back with tight rows") {
    test::LogCapture logs;
    {
        const std::unique_ptr<rhi::Device> device = make_device();
        constexpr std::uint32_t width = 100;  // 400-byte rows: D3D12 copies with a 512-byte pitch
        constexpr std::uint32_t height = 10;
        auto target = device->create_texture({width, height, rhi::Format::rgba8_unorm,
                                              rhi::TextureUsage::color_target | rhi::TextureUsage::copy_src, "target"});
        REQUIRE(target.has_value());
        auto readback = device->create_buffer({.size = width * height * 4,
                                               .usage = rhi::BufferUsage::copy_dst,
                                               .memory = rhi::MemoryType::readback,
                                               .debug_name = "readback"});
        REQUIRE(readback.has_value());

        static_cast<void>(device->begin_frame());
        auto cmd = device->begin_commands("clear and read back");
        const std::array<rhi::Barrier, 1> to_target{
            {{*target, rhi::ResourceState::undefined, rhi::ResourceState::color_target}}};
        cmd->barrier(to_target);
        rhi::RenderPassDesc pass;
        pass.colors[0] = {*target, rhi::LoadOp::clear, {1.0f, 0.5f, 0.0f, 1.0f}};
        pass.color_count = 1;
        pass.debug_name = "clear";
        cmd->begin_render_pass(pass);
        cmd->end_render_pass();
        const std::array<rhi::Barrier, 2> to_copy{
            {{*target, rhi::ResourceState::color_target, rhi::ResourceState::copy_src},
             {*readback, rhi::ResourceState::undefined, rhi::ResourceState::copy_dst}}};
        cmd->barrier(to_copy);
        cmd->copy_texture_to_buffer(*target, *readback);
        const rhi::SubmissionId submission = device->submit(std::move(cmd));
        device->end_frame();
        device->wait(submission);

        const auto* pixels = static_cast<const std::uint8_t*>(device->map(*readback));
        REQUIRE(pixels != nullptr);
        int wrong = 0;
        for (std::uint32_t i = 0; i < width * height; ++i) {
            const std::uint8_t* p = pixels + std::size_t{i} * 4;
            const bool orange = p[0] == 255 && (p[1] == 127 || p[1] == 128) && p[2] == 0 && p[3] == 255;
            wrong += orange ? 0 : 1;
        }
        CHECK_MESSAGE(wrong == 0, wrong, " of ", width * height, " pixels are not the clear color");
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}
