// The physical sky (docs/specs/physical-sky/spec.md): the sun's position and the atmosphere's transmittance.

#include "support/gpu_device.hpp"
#include "test_shaders.hpp"

#include <veach/render/atmosphere.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

using namespace veach;

namespace {

constexpr float degrees_per_radian = 57.29578f;

/// Elevation above the horizon, in degrees.
float elevation(math::vec3 direction) {
    return std::asin(std::clamp(direction.y, -1.0f, 1.0f)) * degrees_per_radian;
}

Sky equator_equinox(float time_of_day) {
    Sky sky;
    sky.latitude = 0.0f;
    sky.day_of_year = 81.25f;  // declination 0 in the model's year
    sky.time_of_day = time_of_day;
    return sky;
}

}  // namespace

TEST_CASE("[sky.AC1] the sun is at the zenith at noon and on the horizon at 18:00 on the equator at an equinox") {
    CHECK(elevation(render::sun_direction(equator_equinox(12.0f))) > 89.0f);
    const math::vec3 evening = render::sun_direction(equator_equinox(18.0f));
    CHECK(std::abs(elevation(evening)) < 1.0f);
    CHECK(evening.x < -0.99f);  // west: -X when north is -Z
    CHECK(render::sun_direction(equator_equinox(6.0f)).x > 0.99f);

    // North is -Z turned by `north` about +Y: at 45 degrees north the noon sun is due south (+Z), then south turned.
    Sky sky;
    sky.latitude = 45.0f;
    const math::vec3 noon = render::sun_direction(sky);
    CHECK(noon.z > 0.0f);
    CHECK(std::abs(noon.x) < 1e-4f);
    sky.north = 0.5f * 3.14159265f;  // north is now -X
    const math::vec3 turned = render::sun_direction(sky);
    CHECK(turned.x > 0.0f);
    CHECK(std::abs(turned.z) < 1e-4f);
    CHECK(std::abs(turned.y - noon.y) < 1e-5f);
}

TEST_CASE("[sky.AC2] the sun at sunset is red, at noon nearly white") {
    const render::AtmosphereParams air = render::atmosphere_params(Sky{});
    const math::vec3 zenith = render::atmosphere_transmittance(air, 0.0f, {0.0f, 1.0f, 0.0f});
    CHECK(zenith.x > 0.8f);
    CHECK(zenith.z > 0.6f);
    CHECK(zenith.x > zenith.z);  // Rayleigh scatters blue away even at noon
    const float two_degrees = 2.0f / degrees_per_radian;
    const math::vec3 sunset =
        render::atmosphere_transmittance(air, 0.0f, {std::cos(two_degrees), std::sin(two_degrees), 0.0f});
    CHECK(sunset.x > 3.0f * sunset.z);
    CHECK(render::atmosphere_transmittance(air, 0.0f, {0.0f, -1.0f, 0.0f}) == math::vec3(0.0f));  // the planet
}

// Mirrors AtmosphereProbeParams in tests/shaders/atmosphere_probe.slang.
struct AtmosphereProbeParams {
    rhi::DescriptorHandle medium;
    rhi::DescriptorHandle queries;
    rhi::DescriptorHandle results;
    std::uint32_t count = 0;
};
static_assert(rhi::parameters_match<AtmosphereProbeParams>(test_shaders::layout::atmosphere_probe,
                                                           {VEACH_PARAM(AtmosphereProbeParams, medium),
                                                            VEACH_PARAM(AtmosphereProbeParams, queries),
                                                            VEACH_PARAM(AtmosphereProbeParams, results),
                                                            VEACH_PARAM(AtmosphereProbeParams, count)}));

TEST_CASE("[sky.AC2] the GPU's transmittance integrator agrees with the CPU's within 1%") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    Sky hazy;
    hazy.mie = 3.0f;
    hazy.rayleigh = 1.5f;
    for (const Sky& sky : {Sky{}, hazy}) {
        const render::AtmosphereParams air = render::atmosphere_params(sky);
        // Altitudes from the ground to near the top, zenith cosines from straight up to just above the horizon.
        std::array<math::vec4, 24> queries{};
        constexpr std::array<float, 4> altitudes{0.0f, 1.5f, 12.0f, 60.0f};
        constexpr std::array<float, 6> cosines{1.0f, 0.7f, 0.3f, 0.1f, 0.03f, 0.005f};
        for (std::size_t i = 0; i < queries.size(); ++i) {
            queries[i] = {air.planet.x + altitudes[i / cosines.size()], cosines[i % cosines.size()], 0.0f, 0.0f};
        }
        auto medium = device->create_buffer({.size = sizeof(air), .debug_name = "probe medium", .stride = sizeof(air)},
                                            std::as_bytes(std::span(&air, 1)));
        auto query_buffer = device->create_buffer(
            {.size = sizeof(queries), .debug_name = "probe queries", .stride = sizeof(math::vec4)},
            std::as_bytes(std::span(queries)));
        auto results = device->create_buffer({.size = sizeof(queries),
                                              .usage = rhi::BufferUsage::storage | rhi::BufferUsage::copy_src,
                                              .debug_name = "probe results",
                                              .stride = sizeof(math::vec4)});
        auto readback = device->create_buffer({.size = sizeof(queries),
                                               .usage = rhi::BufferUsage::copy_dst,
                                               .memory = rhi::MemoryType::readback,
                                               .debug_name = "probe readback"});
        auto pipeline = device->create_compute_pipeline(
            {&test_shaders::atmosphere_probe(), "transmittance_probe", {64, 1, 1}, "transmittance probe"});
        REQUIRE(medium.has_value());
        REQUIRE(query_buffer.has_value());
        REQUIRE(results.has_value());
        REQUIRE(readback.has_value());
        REQUIRE_MESSAGE(pipeline.has_value(), (pipeline ? std::string() : pipeline.error().message));

        AtmosphereProbeParams params;
        params.medium = device->descriptor(*medium);
        params.queries = device->descriptor(*query_buffer);
        params.results = device->descriptor(*results, rhi::View::storage);
        params.count = static_cast<std::uint32_t>(queries.size());
        static_cast<void>(device->begin_frame());
        auto cmd = device->begin_commands("transmittance probe");
        const std::array<rhi::Barrier, 1> to_write{
            {{*results, rhi::ResourceState::undefined, rhi::ResourceState::shader_write}}};
        cmd->barrier(to_write);
        cmd->begin_compute_pass("transmittance probe");
        cmd->use(*medium, rhi::ResourceState::shader_read);  // residency on Metal
        cmd->use(*query_buffer, rhi::ResourceState::shader_read);
        cmd->use(*results, rhi::ResourceState::shader_write);
        cmd->set_pipeline(*pipeline);
        cmd->set_parameters(params);
        cmd->dispatch(1, 1);
        cmd->end_compute_pass();
        const std::array<rhi::Barrier, 2> to_copy{
            {{*results, rhi::ResourceState::shader_write, rhi::ResourceState::copy_src},
             {*readback, rhi::ResourceState::undefined, rhi::ResourceState::copy_dst}}};
        cmd->barrier(to_copy);
        cmd->copy_buffer(*results, *readback, sizeof(queries));
        const rhi::SubmissionId submission = device->submit(std::move(cmd));
        device->end_frame();
        device->wait(submission);

        const auto* gpu = static_cast<const math::vec4*>(device->map(*readback));
        REQUIRE(gpu != nullptr);
        for (std::size_t i = 0; i < queries.size(); ++i) {
            const float mu = queries[i].y;
            const math::vec3 direction(std::sqrt(std::max(1.0f - mu * mu, 0.0f)), mu, 0.0f);
            const math::vec3 cpu = render::atmosphere_transmittance(air, queries[i].x - air.planet.x, direction);
            for (int c = 0; c < 3; ++c) {
                // 1% relative, or 1e-4 absolute where almost nothing gets through.
                CHECK_MESSAGE(std::abs(gpu[i][c] - cpu[c]) <= std::max(0.01f * cpu[c], 1e-4f), "query ", i, " channel ",
                              c, ": GPU ", gpu[i][c], ", CPU ", cpu[c]);
            }
        }
        for (const rhi::BufferHandle b : {*medium, *query_buffer, *results, *readback}) {
            device->destroy(b);
        }
        device->destroy(*pipeline);
    }
    device->wait_idle();
}
