// Texture upload on the platform's GPU backend (docs/specs/first-real-scene/spec.md, scene.AC9): Metal on
// macOS, DX12 on Windows (WARP in hosted CI). Without a capable GPU the test is skipped, unless
// VEACH_REQUIRE_GPU=1 makes that a failure.

#include "support/test_support.hpp"

#include <veach/rhi/rhi.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstdlib>
#include <numeric>
#include <vector>

using namespace veach;

namespace {

std::unique_ptr<rhi::Device> gpu_device() {
#if defined(__APPLE__)
    constexpr auto backend = rhi::Backend::metal;
#elif defined(_WIN32)
    constexpr auto backend = rhi::Backend::dx12;
#else
    constexpr auto backend = rhi::Backend::null;
#endif
    auto device = rhi::create_device({.backend = backend, .validation = true});
    if (!device || !(*device)->capabilities().gpu) {
        const char* require = std::getenv("VEACH_REQUIRE_GPU");  // NOLINT(concurrency-mt-unsafe)
        REQUIRE_MESSAGE((require == nullptr || std::string_view(require) != "1"),
                        "VEACH_REQUIRE_GPU=1 but no GPU device: ", (device ? "null" : device.error().message));
        return nullptr;
    }
    return std::move(*device);
}

/// Uploads `format` with a full mip chain of recognizable bytes and checks mip 0 reads back unchanged.
void round_trip(rhi::Device& device, rhi::Format format, std::uint32_t size) {
    std::uint32_t levels = 1;
    while ((size >> levels) > 0) {
        ++levels;
    }
    std::vector<std::vector<std::byte>> data;
    std::vector<std::span<const std::byte>> mips;
    for (std::uint32_t level = 0; level < levels; ++level) {
        const std::uint32_t s = rhi::mip_size(size, level);
        std::vector<std::byte> bytes(rhi::row_bytes(format, s) * rhi::row_count(format, s));
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::byte>((i * 7 + level * 31) & 0xFF);
        }
        data.push_back(std::move(bytes));
    }
    for (const auto& d : data) {
        mips.emplace_back(d);
    }
    auto texture = device.create_texture(
        {size, size, format, rhi::TextureUsage::sampled | rhi::TextureUsage::copy_src, "uploaded", levels}, mips);
    REQUIRE_MESSAGE(texture.has_value(), (texture ? std::string() : texture.error().message));
    auto readback = device.create_buffer({.size = data[0].size(),
                                          .usage = rhi::BufferUsage::copy_dst,
                                          .memory = rhi::MemoryType::readback,
                                          .debug_name = "readback"});
    REQUIRE(readback.has_value());

    static_cast<void>(device.begin_frame());
    auto cmd = device.begin_commands("read back");
    const std::array<rhi::Barrier, 2> barriers{
        {{*texture, rhi::ResourceState::shader_read, rhi::ResourceState::copy_src},
         {*readback, rhi::ResourceState::undefined, rhi::ResourceState::copy_dst}}};
    cmd->barrier(barriers);
    cmd->copy_texture_to_buffer(*texture, *readback);
    const rhi::SubmissionId submission = device.submit(std::move(cmd));
    device.end_frame();
    device.wait(submission);

    const auto* bytes = static_cast<const std::byte*>(device.map(*readback));
    REQUIRE(bytes != nullptr);
    std::size_t different = 0;
    for (std::size_t i = 0; i < data[0].size(); ++i) {
        different += bytes[i] == data[0][i] ? 0 : 1;
    }
    CHECK_MESSAGE(different == 0, different, " of ", data[0].size(), " bytes differ for ", rhi::to_string(format));
    device.destroy(*texture);
    device.destroy(*readback);
}

}  // namespace

TEST_CASE("[scene.AC9] RGBA8 and BC7 textures with mips upload and read back unchanged") {
    test::LogCapture logs;
    {
        const std::unique_ptr<rhi::Device> device = gpu_device();
        if (!device) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        round_trip(*device, rhi::Format::rgba8_unorm, 64);
        round_trip(*device, rhi::Format::rgba8_srgb, 100);  // rows not 256-byte aligned on DX12
        round_trip(*device, rhi::Format::bc7_srgb, 64);
        round_trip(*device, rhi::Format::bc7_unorm, 6);  // partial blocks at every level
        device->wait_idle();
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}
