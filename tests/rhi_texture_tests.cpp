// Texture kinds, texture updates and raster state (docs/specs/rhi-textures/spec.md, ADR 0090): validation on the null
// device, and the platform's GPU (Metal on macOS, DX12 on WARP in hosted CI) for what the GPU does. Without a capable
// GPU the GPU cases skip, unless VEACH_REQUIRE_GPU=1 makes that a failure.

#include "support/gpu_device.hpp"
#include "support/test_support.hpp"
#include "test_shaders.hpp"

#include <veach/render/render_graph.hpp>
#include <veach/render/upload.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <span>
#include <vector>

using namespace veach;
using rhi::ResourceState;

namespace {

using Float4 = std::array<float, 4>;

/// Mirrors `TextureKindsParams` in tests/shaders/texture_kinds.slang.
struct TextureKindsParams {
    rhi::DescriptorHandle volume_out;
    rhi::DescriptorHandle volume;
    rhi::DescriptorHandle layers;
    rhi::DescriptorHandle cube;
    rhi::DescriptorHandle faces_out;
    rhi::DescriptorHandle sampler;
    rhi::DescriptorHandle queries;
    rhi::DescriptorHandle results;
    std::uint32_t size = 0;
    std::uint32_t count = 0;
    std::uint32_t unused0 = 0;
    std::uint32_t unused1 = 0;
};
static_assert(rhi::parameters_match<TextureKindsParams>(
    test_shaders::layout::texture_kinds,
    {VEACH_PARAM(TextureKindsParams, volume_out), VEACH_PARAM(TextureKindsParams, volume),
     VEACH_PARAM(TextureKindsParams, layers), VEACH_PARAM(TextureKindsParams, cube),
     VEACH_PARAM(TextureKindsParams, faces_out), VEACH_PARAM(TextureKindsParams, sampler),
     VEACH_PARAM(TextureKindsParams, queries), VEACH_PARAM(TextureKindsParams, results),
     VEACH_PARAM(TextureKindsParams, size), VEACH_PARAM(TextureKindsParams, count),
     VEACH_PARAM(TextureKindsParams, unused0), VEACH_PARAM(TextureKindsParams, unused1)}));

/// Mirrors `SamplerStateParams` in tests/shaders/sampler_states.slang.
struct SamplerStateParams {
    rhi::DescriptorHandle depth;
    rhi::DescriptorHandle compare;
    rhi::DescriptorHandle mips;
    rhi::DescriptorHandle sampler;
    rhi::DescriptorHandle queries;
    rhi::DescriptorHandle results;
    std::uint32_t count = 0;
    std::uint32_t unused0 = 0;
    std::uint32_t unused1 = 0;
    std::uint32_t unused2 = 0;
};
static_assert(rhi::parameters_match<SamplerStateParams>(
    test_shaders::layout::sampler_states,
    {VEACH_PARAM(SamplerStateParams, depth), VEACH_PARAM(SamplerStateParams, compare),
     VEACH_PARAM(SamplerStateParams, mips), VEACH_PARAM(SamplerStateParams, sampler),
     VEACH_PARAM(SamplerStateParams, queries), VEACH_PARAM(SamplerStateParams, results),
     VEACH_PARAM(SamplerStateParams, count), VEACH_PARAM(SamplerStateParams, unused0),
     VEACH_PARAM(SamplerStateParams, unused1), VEACH_PARAM(SamplerStateParams, unused2)}));

/// Mirrors `BlendStateParams` in tests/shaders/blend_states.slang.
struct alignas(16) BlendStateParams {
    Float4 color{};
    Float4 rect{-1.0f, -1.0f, 1.0f, 1.0f};
    float depth = 0.5f;
    float slope = 0.0f;
    std::uint32_t unused0 = 0;
    std::uint32_t unused1 = 0;
};
static_assert(rhi::parameters_match<BlendStateParams>(
    test_shaders::layout::blend_states,
    {VEACH_PARAM(BlendStateParams, color), VEACH_PARAM(BlendStateParams, rect), VEACH_PARAM(BlendStateParams, depth),
     VEACH_PARAM(BlendStateParams, slope), VEACH_PARAM(BlendStateParams, unused0),
     VEACH_PARAM(BlendStateParams, unused1)}));

std::vector<std::byte> bytes_of(std::span<const std::uint8_t> values) {
    std::vector<std::byte> out(values.size());
    std::memcpy(out.data(), values.data(), values.size());
    return out;
}

/// `count` RGBA8 texels of one color.
std::vector<std::uint8_t> fill(std::size_t count, std::array<std::uint8_t, 4> color) {
    std::vector<std::uint8_t> out;
    out.reserve(count * 4);
    for (std::size_t i = 0; i < count; ++i) {
        out.insert(out.end(), color.begin(), color.end());
    }
    return out;
}

/// A device, a transient pool and an upload ring, with the state each texture was left in (to import it with).
class Rig {
public:
    explicit Rig(std::unique_ptr<rhi::Device> d) : device(std::move(d)), pool(*device), ring(*device, 1u << 20) {}

    std::unique_ptr<rhi::Device> device;
    render::TransientPool pool;
    render::UploadRing ring;
    std::map<rhi::TextureHandle, ResourceState> states;

    rhi::TextureHandle texture(const rhi::TextureDesc& desc, std::span<const std::span<const std::byte>> mips = {}) {
        auto t = device->create_texture(desc, mips);
        REQUIRE_MESSAGE(t.has_value(), (t ? std::string() : t.error().message));
        states[*t] = mips.empty() ? ResourceState::undefined : ResourceState::shader_read;
        return *t;
    }
    rhi::PipelineHandle compute(const rhi::ShaderLibrary& library, std::string_view entry,
                                std::array<std::uint32_t, 3> group) {
        auto p = device->create_compute_pipeline({&library, entry, group, std::string(entry)});
        REQUIRE_MESSAGE(p.has_value(), (p ? std::string() : p.error().message));
        return *p;
    }
    rhi::SamplerHandle sampler(const rhi::SamplerDesc& desc) {
        auto s = device->create_sampler(desc);
        REQUIRE_MESSAGE(s.has_value(), (s ? std::string() : s.error().message));
        return *s;
    }

    /// Imports `t` into the frame's graph in the state it was left in.
    render::GraphResource use(render::RenderGraph& graph, rhi::TextureHandle t) {
        const render::GraphResource r = graph.import(t, "texture", states[t]);
        imported_.emplace_back(t, r);
        return r;
    }

    /// One frame: `record` adds passes, then the graph runs and the GPU finishes it.
    void frame(const std::function<void(render::RenderGraph&)>& record) {
        static_cast<void>(device->begin_frame());
        render::RenderGraph graph(*device, &pool);
        imported_.clear();
        record(graph);
        const auto compiled = graph.compile();
        REQUIRE_MESSAGE(compiled.has_value(), (compiled ? std::string() : compiled.error().message));
        auto cmd = device->begin_commands("rhi texture test");
        graph.execute(*cmd);
        static_cast<void>(device->submit(std::move(cmd)));
        device->end_frame();
        device->wait_idle();
        for (const auto& [t, r] : imported_) {
            states[t] = graph.final_state(r);
        }
    }

    /// Runs `pipeline` over `queries` (one thread each, 64 per group), with `reads` in shader_read, and returns
    /// the results it wrote. `params` gets the queries, results and count filled in.
    template <class P>
    std::vector<Float4> query(rhi::PipelineHandle pipeline, P params, std::span<const Float4> queries,
                              std::vector<rhi::TextureHandle> reads) {
        const std::uint64_t size = queries.size() * sizeof(Float4);
        auto in = device->create_buffer({.size = size, .debug_name = "queries", .stride = sizeof(Float4)},
                                        std::as_bytes(queries));
        auto out = device->create_buffer({.size = size, .debug_name = "results", .stride = sizeof(Float4)});
        auto back = device->create_buffer({.size = size,
                                           .usage = rhi::BufferUsage::copy_dst,
                                           .memory = rhi::MemoryType::readback,
                                           .debug_name = "results readback"});
        REQUIRE((in && out && back));
        frame([&](render::RenderGraph& graph) {
            const auto q = graph.import(*in, "queries", ResourceState::shader_read);
            const auto r = graph.import(*out, "results", ResourceState::undefined);
            const auto b = graph.import(*back, "readback", ResourceState::undefined);
            std::vector<render::GraphResource> textures;
            for (const rhi::TextureHandle t : reads) {
                textures.push_back(use(graph, t));
            }
            graph.add_compute_pass(
                "query",
                [&](render::PassBuilder& p) {
                    p.read(q);
                    for (const render::GraphResource t : textures) {
                        p.read(t);
                    }
                    p.write(r);
                },
                [&](render::PassContext& ctx) {
                    params.queries = ctx.device.descriptor(*in);
                    params.results = ctx.device.descriptor(*out, rhi::View::storage);
                    params.count = static_cast<std::uint32_t>(queries.size());
                    ctx.cmd.set_pipeline(pipeline);
                    ctx.cmd.set_parameters(params);
                    ctx.cmd.dispatch((params.count + 63) / 64, 1, 1);
                });
            graph.add_transfer_pass(
                "read back",
                [&](render::PassBuilder& p) {
                    p.read(r, ResourceState::copy_src);
                    p.write(b, ResourceState::copy_dst);
                },
                [&](render::PassContext& ctx) { ctx.cmd.copy_buffer(*out, *back, size); });
        });
        const auto* results = static_cast<const Float4*>(device->map(*back));
        REQUIRE(results != nullptr);
        std::vector<Float4> values(results, results + queries.size());
        device->destroy(*in);
        device->destroy(*out);
        device->destroy(*back);
        return values;
    }

    /// TextureKindsParams with every handle pointing at a null descriptor of its kind (ADR 0027).
    TextureKindsParams kinds() const {
        TextureKindsParams p;
        p.volume_out = device->null_descriptor(rhi::DescriptorKind::storage_texture_3d);
        p.volume = device->null_descriptor(rhi::DescriptorKind::texture_3d);
        p.layers = device->null_descriptor(rhi::DescriptorKind::texture_2d_array);
        p.cube = device->null_descriptor(rhi::DescriptorKind::texture_cube);
        p.faces_out = device->null_descriptor(rhi::DescriptorKind::storage_texture_2d_array);
        p.sampler = device->null_descriptor(rhi::DescriptorKind::sampler);
        p.queries = device->null_descriptor(rhi::DescriptorKind::buffer);
        p.results = device->null_descriptor(rhi::DescriptorKind::storage_buffer);
        return p;
    }

private:
    std::vector<std::pair<rhi::TextureHandle, render::GraphResource>> imported_;
};

bool near(const Float4& a, const Float4& b, float tolerance = 1e-3f) {
    for (std::size_t i = 0; i < 4; ++i) {
        if (std::abs(a.at(i) - b.at(i)) > tolerance) {
            return false;
        }
    }
    return true;
}

std::string str(const Float4& v) {
    return std::format("({}, {}, {}, {})", v[0], v[1], v[2], v[3]);
}

Float4 unorm(std::array<std::uint8_t, 4> c) {
    return {c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f};
}

}  // namespace

// ---- validation on every backend (the null device) ------------------------------------------------------------

TEST_CASE("[rhi.AC8] texture kinds are validated the same on every backend") {
    auto device = std::move(*rhi::create_device({}));
    const auto rejected = [&](rhi::TextureDesc desc) {
        const auto t = device->create_texture(desc);
        return !t.has_value() && t.error().code == ErrorCode::rhi_invalid_texture;
    };
    using rhi::TextureType;
    rhi::TextureDesc cube{.width = 16, .height = 16, .debug_name = "cube", .type = TextureType::cube, .layers = 6};
    CHECK(device->create_texture(cube).has_value());
    CHECK(rejected({.width = 16, .height = 8, .debug_name = "c", .type = TextureType::cube, .layers = 6}));
    CHECK(rejected({.width = 16, .height = 16, .debug_name = "c", .type = TextureType::cube, .layers = 1}));
    CHECK(rejected({.width = 16, .height = 16, .debug_name = "c", .type = TextureType::cube_array, .layers = 8}));
    CHECK(
        device->create_texture({.width = 16, .height = 16, .type = TextureType::cube_array, .layers = 12}).has_value());
    CHECK(rejected({.width = 16, .height = 16, .debug_name = "2d", .layers = 4}));
    CHECK(rejected({.width = 16, .height = 16, .debug_name = "3d", .type = TextureType::d3, .layers = 2}));
    CHECK(rejected({.width = 16, .height = 16, .format = rhi::Format::bc7_unorm, .type = TextureType::d3, .depth = 4}));
    CHECK(rejected({.width = 16,
                    .height = 16,
                    .format = rhi::Format::rgba8_srgb,
                    .usage = rhi::TextureUsage::storage,
                    .debug_name = "srgb storage"}));
    CHECK(rejected({.width = 8, .height = 8, .mip_levels = 5}));  // 8, 4, 2, 1: four levels
    CHECK(device->create_texture({.width = 8, .height = 8, .mip_levels = 6, .type = TextureType::d3, .depth = 32})
              .has_value());

    // Initial data holds every layer of each mip.
    const rhi::TextureDesc array{
        .width = 4, .height = 4, .debug_name = "array", .type = TextureType::d2_array, .layers = 3};
    CHECK(rhi::mip_bytes(array, 0) == 4 * 4 * 4 * 3);
    const std::vector<std::byte> one_layer(4 * 4 * 4);
    const std::array<std::span<const std::byte>, 1> short_data{one_layer};
    const auto short_texture = device->create_texture(array, short_data);
    REQUIRE_FALSE(short_texture.has_value());
    CHECK(short_texture.error().code == ErrorCode::rhi_invalid_texture);

    const rhi::TextureDesc volume{.width = 8, .height = 4, .type = TextureType::d3, .depth = 16};
    CHECK(rhi::layer_count(volume) == 1);
    CHECK(rhi::depth_of_mip(volume, 2) == 4);
    CHECK(rhi::mip_bytes(volume, 1) == 4 * 2 * 4 * 8);
    CHECK(rhi::layer_count(cube) == 6);
}

TEST_CASE("[rhi.AC9] views of parts of a texture are checked, and transients are pooled by their whole shape") {
    test::LogCapture logs;
    auto device = std::move(*rhi::create_device({}));
    auto array = device->create_texture(
        {.width = 8, .height = 8, .mip_levels = 2, .type = rhi::TextureType::d2_array, .layers = 4});
    REQUIRE(array.has_value());
    CHECK(device->descriptor(*array, rhi::View::read, {.base_mip = 1, .base_layer = 3}).value != 0);
    CHECK(device->descriptor(*array, rhi::View::read, {.base_mip = 2}).value == 0);  // no third mip
    CHECK(device->descriptor(*array, rhi::View::read, {.base_layer = 4}).value == 0);
    CHECK(std::ranges::count(logs.errors(), '\n') == 2);
    CHECK(logs.errors().find("a view from mip 2") != std::string::npos);

    render::TransientPool pool(*device);
    const rhi::TextureDesc four{.width = 8,
                                .height = 8,
                                .usage = rhi::TextureUsage::color_target,
                                .type = rhi::TextureType::d2_array,
                                .layers = 4};
    rhi::TextureDesc two = four;
    two.layers = 2;
    const auto a = pool.acquire(four);
    const auto b = pool.acquire(two);
    REQUIRE((a && b));
    CHECK(*a != *b);
    pool.release_all();
    CHECK(*pool.acquire(two) == *b);
}

TEST_CASE("[rhi.AC12] copy regions resolve against the mip and are rejected outside it") {
    const rhi::TextureDesc array{
        .width = 16, .height = 8, .mip_levels = 2, .type = rhi::TextureType::d2_array, .layers = 3};
    const auto whole = rhi::resolve_region(array, {.mip = 1, .base_layer = 1, .layer_count = 2});
    REQUIRE(whole.has_value());
    CHECK(whole->width == 8);
    CHECK(whole->height == 4);
    CHECK(whole->depth == 1);
    CHECK_FALSE(rhi::resolve_region(array, {.mip = 2}).has_value());
    CHECK_FALSE(rhi::resolve_region(array, {.base_layer = 2, .layer_count = 2}).has_value());
    CHECK_FALSE(rhi::resolve_region(array, {.x = 10, .width = 8}).has_value());
    const rhi::TextureDesc bc{.width = 12, .height = 12, .format = rhi::Format::bc7_unorm, .mip_levels = 2};
    CHECK(rhi::resolve_region(bc, {.x = 4, .y = 8, .width = 4, .height = 4}).has_value());
    CHECK_FALSE(rhi::resolve_region(bc, {.x = 2, .width = 4, .height = 4}).has_value());  // cuts a block
    CHECK(rhi::resolve_region(bc, {.mip = 1, .x = 4}).has_value());                       // 6x6: ends at the edge
    CHECK(rhi::texture_copy_pitch(rhi::Format::rgba8_unorm, 100) == 512);
    CHECK(rhi::texture_copy_pitch(rhi::Format::bc7_unorm, 12) == 256);
    CHECK(rhi::texture_copy_layer_stride(rhi::Format::rgba8_unorm, {.width = 8, .height = 3, .depth = 1}, 256) == 1024);
}

TEST_CASE("[rhi.AC13] the upload ring aligns, reuses a frame's share only when its slot comes round, and overflows") {
    auto device = std::move(*rhi::create_device({.frames_in_flight = 2}));
    render::UploadRing ring(*device, 4096);
    static_cast<void>(device->begin_frame());
    const auto a = ring.allocate(100);
    const auto b = ring.allocate(100);
    REQUIRE((a && b));
    CHECK(a->offset % rhi::texture_copy_offset_alignment == 0);
    CHECK(b->offset == a->offset + 512);
    CHECK(a->buffer == b->buffer);
    CHECK(a->data != nullptr);
    CHECK(ring.used() == 612);
    const auto big = ring.allocate(8192);  // beyond the frame's share: a buffer of its own
    REQUIRE(big.has_value());
    CHECK(big->buffer != a->buffer);
    CHECK(big->offset == 0);
    CHECK(ring.overflow() == 8192);
    device->end_frame();

    static_cast<void>(device->begin_frame());  // the other slot: another share
    const auto c = ring.allocate(16);
    REQUIRE(c.has_value());
    CHECK(c->offset / 4096 != a->offset / 4096);
    CHECK(ring.overflow() == 0);
    device->end_frame();

    static_cast<void>(device->begin_frame());  // the first slot again: its share is reused
    const auto d = ring.allocate(16);
    REQUIRE(d.has_value());
    CHECK(d->offset == a->offset);
    CHECK(ring.used() == 16);
    device->end_frame();
}

TEST_CASE("[rhi.AC12] upload_texture adds a transfer pass and checks its region and data") {
    auto device = std::move(*rhi::create_device({}));
    render::UploadRing ring(*device);
    auto texture = device->create_texture({.width = 16,
                                           .height = 16,
                                           .usage = rhi::TextureUsage::sampled | rhi::TextureUsage::copy_dst,
                                           .debug_name = "heights",
                                           .type = rhi::TextureType::d2_array,
                                           .layers = 2});
    REQUIRE(texture.has_value());
    static_cast<void>(device->begin_frame());
    render::RenderGraph graph(*device);
    const auto t = graph.import(*texture, "heights", ResourceState::shader_read, ResourceState::shader_read);
    const std::vector<std::byte> data(4 * 4 * 4 * 2);
    CHECK(render::upload_texture(graph, ring, t, {.layer_count = 2, .x = 4, .y = 4, .width = 4, .height = 4}, data,
                                 "upload heights")
              .has_value());
    const auto wrong_size = render::upload_texture(graph, ring, t, {.width = 4, .height = 4}, data);
    REQUIRE_FALSE(wrong_size.has_value());
    CHECK(wrong_size.error().code == ErrorCode::rhi_invalid_texture);
    CHECK_FALSE(render::upload_texture(graph, ring, t, {.base_layer = 2}, data).has_value());
    REQUIRE(graph.compile().has_value());
    const std::string plan = graph.describe();
    CHECK(plan.find("upload heights [transfer]") != std::string::npos);
    CHECK(plan.find("barrier heights: shader_read -> copy_dst") != std::string::npos);
    CHECK(plan.find("final barrier heights: copy_dst -> shader_read") != std::string::npos);
    device->end_frame();
}

TEST_CASE("[rhi.AC15] blend, depth bias and sampler states are validated the same on every backend") {
    auto device = std::move(*rhi::create_device({}));
    rhi::GraphicsPipelineDesc desc;
    desc.color_formats = {rhi::Format::rgba8_unorm, rhi::Format::rg32_uint};
    desc.blend = {rhi::BlendMode::alpha};
    CHECK(device->create_graphics_pipeline(desc).has_value());
    desc.blend = {rhi::BlendMode::opaque, rhi::BlendMode::additive};  // an integer target
    const auto integer = device->create_graphics_pipeline(desc);
    REQUIRE_FALSE(integer.has_value());
    CHECK(integer.error().code == ErrorCode::rhi_invalid_pipeline);
    desc.blend = {rhi::BlendMode::opaque, rhi::BlendMode::opaque, rhi::BlendMode::alpha};  // no third target
    CHECK_FALSE(device->create_graphics_pipeline(desc).has_value());
    desc.blend = {};
    desc.depth_bias = {.constant = -2, .slope = -1.5f};
    CHECK_FALSE(device->create_graphics_pipeline(desc).has_value());  // a bias needs a depth format
    desc.depth_format = rhi::Format::depth32_float;
    CHECK(device->create_graphics_pipeline(desc).has_value());

    CHECK(device->create_sampler({.max_anisotropy = rhi::terrain_anisotropy}).has_value());
    CHECK(device->create_sampler({.compare = rhi::CompareOp::greater_equal}).has_value());
    for (const rhi::SamplerDesc bad : {rhi::SamplerDesc{.max_anisotropy = 0}, rhi::SamplerDesc{.max_anisotropy = 17},
                                       rhi::SamplerDesc{.filter = rhi::Filter::nearest, .max_anisotropy = 4}}) {
        const auto s = device->create_sampler(bad);
        REQUIRE_FALSE(s.has_value());
        CHECK(s.error().code == ErrorCode::rhi_invalid_sampler);
    }
}

// ---- on the platform's GPU -------------------------------------------------------------------------------------

TEST_CASE("[rhi.AC8] a 3D texture written in compute samples back exactly, per mip") {
    test::LogCapture logs;
    {
        std::unique_ptr<rhi::Device> gpu = test::gpu_device();
        if (!gpu) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        Rig rig(std::move(gpu));
        constexpr std::uint32_t size = 8;
        const rhi::TextureHandle volume = rig.texture({.width = size,
                                                       .height = size,
                                                       .format = rhi::Format::rgba16_float,
                                                       .usage = rhi::TextureUsage::storage | rhi::TextureUsage::sampled,
                                                       .debug_name = "volume",
                                                       .mip_levels = 2,
                                                       .type = rhi::TextureType::d3,
                                                       .depth = size});
        const rhi::PipelineHandle write = rig.compute(test_shaders::texture_kinds(), "write_volume", {4, 4, 4});
        const rhi::PipelineHandle sample = rig.compute(test_shaders::texture_kinds(), "sample_volume", {64, 1, 1});
        const rhi::SamplerHandle nearest = rig.sampler({.filter = rhi::Filter::nearest});
        const rhi::SamplerHandle linear = rig.sampler({.filter = rhi::Filter::linear});

        // Mip 0 through the whole-texture storage view, mip 1 (4x4x4) through a view of that mip.
        rig.frame([&](render::RenderGraph& graph) {
            const auto v = rig.use(graph, volume);
            for (std::uint32_t mip = 0; mip < 2; ++mip) {
                graph.add_compute_pass(
                    std::format("write mip {}", mip), [&](render::PassBuilder& b) { b.write(v); },
                    [&, mip](render::PassContext& ctx) {
                        TextureKindsParams p = rig.kinds();
                        p.volume_out = mip == 0 ? ctx.device.descriptor(volume, rhi::View::storage)
                                                : ctx.device.descriptor(volume, rhi::View::storage, {.base_mip = 1});
                        p.size = size >> mip;
                        ctx.cmd.set_pipeline(write);
                        ctx.cmd.set_parameters(p);
                        ctx.cmd.dispatch(p.size / 4, p.size / 4, p.size / 4);
                    });
            }
        });

        std::vector<Float4> queries;
        std::vector<Float4> expected;
        for (std::uint32_t mip = 0; mip < 2; ++mip) {
            const std::uint32_t n = size >> mip;
            for (const auto& [x, y, z] : {std::array<std::uint32_t, 3>{0, 0, 0},
                                          {1, 2, 3},
                                          {n - 1, 0, n - 1},
                                          {n - 1, n - 1, n - 1},
                                          {2, n - 1, 1}}) {
                const auto at = [n](std::uint32_t i) {
                    return (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
                };
                queries.push_back({at(x), at(y), at(z), static_cast<float>(mip)});
                expected.push_back({static_cast<float>(x), static_cast<float>(y), static_cast<float>(z),
                                    static_cast<float>(x + n * (y + n * z))});
            }
        }
        TextureKindsParams p = rig.kinds();
        p.volume = rig.device->descriptor(volume);
        p.sampler = rig.device->descriptor(nearest);
        const std::vector<Float4> got = rig.query(sample, p, queries, {volume});
        for (std::size_t i = 0; i < got.size(); ++i) {
            CHECK_MESSAGE(near(got[i], expected[i]), "query ", i, ": ", str(got[i]), " expected ", str(expected[i]));
        }

        // Linear filtering between two depth slices of mip 0 blends them.
        p.sampler = rig.device->descriptor(linear);
        const std::array<Float4, 1> between{{{2.5f / size, 2.5f / size, 4.0f / size, 0.0f}}};  // z 3.5: slices 3, 4
        const std::vector<Float4> blended = rig.query(sample, p, between, {volume});
        CHECK_MESSAGE(near(blended[0], {2.0f, 2.0f, 3.5f, 2.0f + 8.0f * (2.0f + 8.0f * 3.5f)}, 1e-2f), str(blended[0]));
        rig.device->wait_idle();
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}

TEST_CASE("[rhi.AC9] a texture array samples per layer and per mip, through views, and renders into one layer") {
    test::LogCapture logs;
    {
        std::unique_ptr<rhi::Device> gpu = test::gpu_device();
        if (!gpu) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        Rig rig(std::move(gpu));
        constexpr std::uint32_t layers = 4;
        const std::array<std::array<std::uint8_t, 4>, layers> mip0{
            {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}}};
        const std::array<std::array<std::uint8_t, 4>, layers> mip1{
            {{10, 20, 30, 40}, {50, 60, 70, 80}, {90, 100, 110, 120}, {130, 140, 150, 160}}};
        std::vector<std::uint8_t> level0;
        std::vector<std::uint8_t> level1;
        for (std::uint32_t l = 0; l < layers; ++l) {
            const auto a = fill(8 * 8, mip0.at(l));
            const auto b = fill(4 * 4, mip1.at(l));
            level0.insert(level0.end(), a.begin(), a.end());
            level1.insert(level1.end(), b.begin(), b.end());
        }
        const auto b0 = bytes_of(level0);
        const auto b1 = bytes_of(level1);
        const std::array<std::span<const std::byte>, 2> mips{b0, b1};
        const rhi::TextureHandle array =
            rig.texture({.width = 8,
                         .height = 8,
                         .usage = rhi::TextureUsage::sampled | rhi::TextureUsage::color_target,
                         .debug_name = "layers",
                         .mip_levels = 2,
                         .type = rhi::TextureType::d2_array,
                         .layers = layers},
                        mips);
        const rhi::PipelineHandle sample = rig.compute(test_shaders::texture_kinds(), "sample_layers", {64, 1, 1});
        const rhi::SamplerHandle nearest = rig.sampler({.filter = rhi::Filter::nearest});
        TextureKindsParams p = rig.kinds();
        p.sampler = rig.device->descriptor(nearest);

        std::vector<Float4> queries;
        for (std::uint32_t l = 0; l < layers; ++l) {
            queries.push_back({0.3f, 0.6f, static_cast<float>(l), 0.0f});
            queries.push_back({0.7f, 0.2f, static_cast<float>(l), 1.0f});
        }
        p.layers = rig.device->descriptor(array);
        std::vector<Float4> got = rig.query(sample, p, queries, {array});
        for (std::uint32_t l = 0; l < layers; ++l) {
            CHECK_MESSAGE(near(got[2 * l], unorm(mip0.at(l))), "layer ", l, " mip 0: ", str(got[2 * l]));
            CHECK_MESSAGE(near(got[2 * l + 1], unorm(mip1.at(l))), "layer ", l, " mip 1: ", str(got[2 * l + 1]));
        }

        // A view of layers 2 and 3 starts at layer 2; a view of mip 1 starts at mip 1.
        const std::array<Float4, 2> first{{{0.5f, 0.5f, 0.0f, 0.0f}, {0.5f, 0.5f, 1.0f, 0.0f}}};
        p.layers = rig.device->descriptor(array, rhi::View::read, {.base_layer = 2, .layer_count = 2});
        got = rig.query(sample, p, first, {array});
        CHECK(near(got[0], unorm(mip0.at(2))));
        CHECK(near(got[1], unorm(mip0.at(3))));
        p.layers = rig.device->descriptor(array, rhi::View::read, {.base_mip = 1});
        got = rig.query(sample, p, first, {array});
        CHECK(near(got[0], unorm(mip1.at(0))));
        CHECK(near(got[1], unorm(mip1.at(1))));

        // A render pass cleared into layer 3, mip 1 changes only that slice.
        const Float4 cleared{0.0f, 1.0f, 1.0f, 1.0f};
        rig.frame([&](render::RenderGraph& graph) {
            const auto t = rig.use(graph, array);
            graph.add_graphics_pass(
                "clear one layer",
                [&](render::PassBuilder& b) { b.color_target(t, rhi::LoadOp::clear, cleared, {.mip = 1, .layer = 3}); },
                [](render::PassContext&) {});
        });
        p.layers = rig.device->descriptor(array);
        got = rig.query(sample, p, queries, {array});
        for (std::uint32_t l = 0; l < layers; ++l) {
            CHECK(near(got[2 * l], unorm(mip0.at(l))));
            CHECK_MESSAGE(near(got[2 * l + 1], l == 3 ? cleared : unorm(mip1.at(l))), "layer ", l,
                          " mip 1: ", str(got[2 * l + 1]));
        }
        rig.device->wait_idle();
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}

TEST_CASE("[rhi.AC10] a cube map samples each face by direction, and compute writes its faces as layers") {
    test::LogCapture logs;
    {
        std::unique_ptr<rhi::Device> gpu = test::gpu_device();
        if (!gpu) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        Rig rig(std::move(gpu));
        const std::array<std::array<std::uint8_t, 4>, 6> faces{{{255, 0, 0, 255},
                                                                {0, 255, 0, 255},
                                                                {0, 0, 255, 255},
                                                                {255, 255, 0, 255},
                                                                {255, 0, 255, 255},
                                                                {0, 255, 255, 255}}};
        std::vector<std::uint8_t> texels;
        for (const auto& c : faces) {
            const auto face = fill(4 * 4, c);
            texels.insert(texels.end(), face.begin(), face.end());
        }
        const auto data = bytes_of(texels);
        const std::array<std::span<const std::byte>, 1> mips{data};
        const rhi::TextureHandle cube = rig.texture(
            {.width = 4, .height = 4, .debug_name = "cube", .type = rhi::TextureType::cube, .layers = 6}, mips);
        const rhi::PipelineHandle sample = rig.compute(test_shaders::texture_kinds(), "sample_cube", {64, 1, 1});
        const rhi::SamplerHandle linear = rig.sampler({});
        const std::array<Float4, 6> directions{{{1, 0.1f, -0.2f, 0},
                                                {-1, 0.2f, 0.1f, 0},
                                                {0.1f, 1, 0.2f, 0},
                                                {-0.2f, -1, 0.1f, 0},
                                                {0.2f, -0.1f, 1, 0},
                                                {0.1f, 0.2f, -1, 0}}};
        TextureKindsParams p = rig.kinds();
        p.sampler = rig.device->descriptor(linear);
        p.cube = rig.device->descriptor(cube);
        std::vector<Float4> got = rig.query(sample, p, directions, {cube});
        for (std::size_t f = 0; f < 6; ++f) {
            CHECK_MESSAGE(near(got[f], unorm(faces.at(f))), "face ", f, ": ", str(got[f]));
        }

        // A storage cube: write_faces puts (x, y, face, 1) in every texel through the RWTexture2DArray view.
        constexpr std::uint32_t size = 4;
        const rhi::TextureHandle written =
            rig.texture({.width = size,
                         .height = size,
                         .format = rhi::Format::rgba16_float,
                         .usage = rhi::TextureUsage::storage | rhi::TextureUsage::sampled,
                         .debug_name = "written cube",
                         .type = rhi::TextureType::cube,
                         .layers = 6});
        const rhi::PipelineHandle write = rig.compute(test_shaders::texture_kinds(), "write_faces", {4, 4, 1});
        rig.frame([&](render::RenderGraph& graph) {
            const auto t = rig.use(graph, written);
            graph.add_compute_pass(
                "write faces", [&](render::PassBuilder& b) { b.write(t); },
                [&](render::PassContext& ctx) {
                    TextureKindsParams w = rig.kinds();
                    w.faces_out = ctx.device.descriptor(written, rhi::View::storage);
                    w.size = size;
                    ctx.cmd.set_pipeline(write);
                    ctx.cmd.set_parameters(w);
                    ctx.cmd.dispatch(1, 1, 6);
                });
        });
        const rhi::SamplerHandle nearest = rig.sampler({.filter = rhi::Filter::nearest});
        p.sampler = rig.device->descriptor(nearest);
        p.cube = rig.device->descriptor(written);
        got = rig.query(sample, p, directions, {written});
        for (std::size_t f = 0; f < 6; ++f) {
            CHECK_MESSAGE(near(got[f], {got[f][0], got[f][1], static_cast<float>(f), 1.0f}), "face ", f, ": ",
                          str(got[f]));
            CHECK((got[f][0] >= 0.0f && got[f][0] <= 3.0f && got[f][1] >= 0.0f && got[f][1] <= 3.0f));
        }
        rig.device->wait_idle();
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}

TEST_CASE("[rhi.AC11] null descriptors of every texture kind read zero") {
    test::LogCapture logs;
    {
        std::unique_ptr<rhi::Device> gpu = test::gpu_device();
        if (!gpu) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        Rig rig(std::move(gpu));
        const rhi::SamplerHandle linear = rig.sampler({});
        TextureKindsParams p = rig.kinds();
        p.sampler = rig.device->descriptor(linear);
        const std::array<Float4, 1> one{{{0.5f, 0.5f, 0.5f, 0.0f}}};
        for (const char* entry : {"sample_volume", "sample_layers", "sample_cube"}) {
            const rhi::PipelineHandle pipeline = rig.compute(test_shaders::texture_kinds(), entry, {64, 1, 1});
            const std::vector<Float4> got = rig.query(pipeline, p, one, {});
            CHECK_MESSAGE(near(got[0], {0, 0, 0, 0}), entry, ": ", str(got[0]));
        }
        rig.device->wait_idle();
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}

TEST_CASE("[rhi.AC12] partial uploads through the graph change only their region: array layers, mips, 3D boxes") {
    test::LogCapture logs;
    {
        std::unique_ptr<rhi::Device> gpu = test::gpu_device();
        if (!gpu) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        Rig rig(std::move(gpu));
        // A 16x16 array of 3 layers and 2 mips, every texel gray; then a 4x3 box at (2, 3) of mip 1 in layers 1-2.
        constexpr std::array<std::uint8_t, 4> gray{100, 100, 100, 255};
        const auto level0 = bytes_of(fill(16 * 16 * 3, gray));
        const auto level1 = bytes_of(fill(8 * 8 * 3, gray));
        const std::array<std::span<const std::byte>, 2> mips{level0, level1};
        const rhi::TextureHandle array = rig.texture({.width = 16,
                                                      .height = 16,
                                                      .usage = rhi::TextureUsage::sampled | rhi::TextureUsage::copy_dst,
                                                      .debug_name = "streamed layers",
                                                      .mip_levels = 2,
                                                      .type = rhi::TextureType::d2_array,
                                                      .layers = 3},
                                                     mips);
        const rhi::TextureRegion box{
            .mip = 1, .base_layer = 1, .layer_count = 2, .x = 2, .y = 3, .width = 4, .height = 3};
        std::vector<std::uint8_t> patch;
        for (std::uint32_t l = 0; l < 2; ++l) {
            for (std::uint32_t y = 0; y < 3; ++y) {
                for (std::uint32_t x = 0; x < 4; ++x) {
                    patch.insert(patch.end(), {static_cast<std::uint8_t>(x * 60), static_cast<std::uint8_t>(y * 80),
                                               static_cast<std::uint8_t>(200 + l * 50), 255});
                }
            }
        }
        rig.frame([&](render::RenderGraph& graph) {
            const auto t = rig.use(graph, array);
            const auto uploaded = render::upload_texture(graph, rig.ring, t, box, bytes_of(patch), "upload patch");
            REQUIRE_MESSAGE(uploaded.has_value(), (uploaded ? std::string() : uploaded.error().message));
        });

        const rhi::PipelineHandle sample = rig.compute(test_shaders::texture_kinds(), "sample_layers", {64, 1, 1});
        const rhi::SamplerHandle nearest = rig.sampler({.filter = rhi::Filter::nearest});
        TextureKindsParams p = rig.kinds();
        p.sampler = rig.device->descriptor(nearest);
        p.layers = rig.device->descriptor(array);
        std::vector<Float4> queries;
        std::vector<Float4> expected;
        for (std::uint32_t l = 0; l < 3; ++l) {
            for (std::uint32_t y = 0; y < 8; ++y) {
                for (std::uint32_t x = 0; x < 8; ++x) {
                    queries.push_back({(x + 0.5f) / 8, (y + 0.5f) / 8, static_cast<float>(l), 1.0f});
                    const bool inside = l >= 1 && x >= 2 && x < 6 && y >= 3 && y < 6;
                    expected.push_back(inside ? unorm({static_cast<std::uint8_t>((x - 2) * 60),
                                                       static_cast<std::uint8_t>((y - 3) * 80),
                                                       static_cast<std::uint8_t>(200 + (l - 1) * 50), 255})
                                              : unorm(gray));
                }
            }
            queries.push_back({0.5f, 0.5f, static_cast<float>(l), 0.0f});  // mip 0 untouched
            expected.push_back(unorm(gray));
        }
        const std::vector<Float4> got = rig.query(sample, p, queries, {array});
        std::size_t wrong = 0;
        for (std::size_t i = 0; i < got.size(); ++i) {
            if (!near(got[i], expected[i])) {
                ++wrong;
                MESSAGE("query ", i, ": ", str(got[i]), " expected ", str(expected[i]));
            }
        }
        CHECK(wrong == 0);

        // A 3D box: (2, 1, 3) to (4, 3, 5) of an 8x8x8 volume that starts with no data (copy_dst from undefined).
        constexpr std::uint32_t n = 8;
        const rhi::TextureHandle volume =
            rig.texture({.width = n,
                         .height = n,
                         .usage = rhi::TextureUsage::sampled | rhi::TextureUsage::copy_dst,
                         .debug_name = "streamed volume",
                         .type = rhi::TextureType::d3,
                         .depth = n});
        const auto zeros = bytes_of(fill(n * n * n, {0, 0, 0, 0}));
        const rhi::TextureRegion cube{.x = 2, .y = 1, .z = 3, .width = 3, .height = 3, .depth = 3};
        std::vector<std::uint8_t> block;
        for (std::uint32_t z = 0; z < 3; ++z) {
            for (std::uint32_t y = 0; y < 3; ++y) {
                for (std::uint32_t x = 0; x < 3; ++x) {
                    block.insert(block.end(), {static_cast<std::uint8_t>(10 + x), static_cast<std::uint8_t>(20 + y),
                                               static_cast<std::uint8_t>(30 + z), 255});
                }
            }
        }
        rig.frame([&](render::RenderGraph& graph) {
            const auto t = rig.use(graph, volume);
            REQUIRE(render::upload_texture(graph, rig.ring, t, {}, zeros, "clear volume").has_value());
            REQUIRE(render::upload_texture(graph, rig.ring, t, cube, bytes_of(block), "upload box").has_value());
        });
        const rhi::PipelineHandle sample_volume =
            rig.compute(test_shaders::texture_kinds(), "sample_volume", {64, 1, 1});
        p = rig.kinds();
        p.sampler = rig.device->descriptor(nearest);
        p.volume = rig.device->descriptor(volume);
        queries.clear();
        expected.clear();
        for (std::uint32_t z = 0; z < n; ++z) {
            for (std::uint32_t y = 0; y < n; ++y) {
                for (std::uint32_t x = 0; x < n; ++x) {
                    queries.push_back({(x + 0.5f) / n, (y + 0.5f) / n, (z + 0.5f) / n, 0.0f});
                    const bool inside = x >= 2 && x < 5 && y >= 1 && y < 4 && z >= 3 && z < 6;
                    expected.push_back(
                        inside ? unorm({static_cast<std::uint8_t>(10 + x - 2), static_cast<std::uint8_t>(20 + y - 1),
                                        static_cast<std::uint8_t>(30 + z - 3), 255})
                               : Float4{0, 0, 0, 0});
                }
            }
        }
        const std::vector<Float4> texels = rig.query(sample_volume, p, queries, {volume});
        wrong = 0;
        for (std::size_t i = 0; i < texels.size(); ++i) {
            wrong += near(texels[i], expected[i]) ? 0 : 1;
        }
        CHECK_MESSAGE(wrong == 0, wrong, " of ", texels.size(), " texels differ");
        rig.device->wait_idle();
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}

TEST_CASE("[rhi.AC14] a comparison sampler compares depth per texel and averages 2x2 when linear (PCF)") {
    test::LogCapture logs;
    {
        std::unique_ptr<rhi::Device> gpu = test::gpu_device();
        if (!gpu) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        Rig rig(std::move(gpu));
        // A 4x4 depth texture whose columns hold 0.2, 0.4, 0.6 and 0.8 (reverse-Z: larger is nearer the light).
        std::vector<float> depths;
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                depths.push_back(0.2f * static_cast<float>(x + 1));
            }
        }
        const auto bytes = std::as_bytes(std::span(depths));
        const std::array<std::span<const std::byte>, 1> mips{bytes};
        const rhi::TextureHandle shadow = rig.texture(
            {.width = 4, .height = 4, .format = rhi::Format::depth32_float, .debug_name = "shadow map"}, mips);
        const rhi::PipelineHandle compare = rig.compute(test_shaders::sampler_states(), "compare_depth", {64, 1, 1});
        const rhi::SamplerHandle point =
            rig.sampler({.filter = rhi::Filter::nearest, .compare = rhi::CompareOp::greater_equal});
        const rhi::SamplerHandle pcf =
            rig.sampler({.filter = rhi::Filter::linear, .compare = rhi::CompareOp::greater_equal});
        SamplerStateParams p;
        p.depth = rig.device->descriptor(shadow);
        p.mips = rig.device->null_descriptor(rhi::DescriptorKind::texture);
        p.sampler = rig.device->null_descriptor(rhi::DescriptorKind::sampler);
        p.compare = rig.device->descriptor(point);
        // Reference 0.5 at each column's center: lit (1) where 0.5 >= the stored depth.
        const std::array<Float4, 4> centers{
            {{0.125f, 0.5f, 0.5f, 0}, {0.375f, 0.5f, 0.5f, 0}, {0.625f, 0.5f, 0.5f, 0}, {0.875f, 0.5f, 0.5f, 0}}};
        std::vector<Float4> got = rig.query(compare, p, centers, {shadow});
        const std::array<float, 4> lit{1, 1, 0, 0};
        for (std::size_t i = 0; i < 4; ++i) {
            CHECK_MESSAGE(got[i][0] == lit.at(i), "column ", i, ": ", got[i][0]);
        }
        // Linear: on the edge between columns 1 (lit) and 2 (shadowed) half of the 2x2 footprint passes.
        p.compare = rig.device->descriptor(pcf);
        const std::array<Float4, 3> edges{{{0.5f, 0.5f, 0.5f, 0}, {0.25f, 0.5f, 0.5f, 0}, {0.75f, 0.5f, 0.5f, 0}}};
        got = rig.query(compare, p, edges, {shadow});
        CHECK_MESSAGE(std::abs(got[0][0] - 0.5f) < 0.02f, got[0][0]);
        CHECK_MESSAGE(std::abs(got[1][0] - 1.0f) < 0.02f, got[1][0]);
        CHECK_MESSAGE(std::abs(got[2][0]) < 0.02f, got[2][0]);
        rig.device->wait_idle();
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}

TEST_CASE("[rhi.AC15] anisotropic filtering samples a finer mip than trilinear for a stretched footprint") {
    test::LogCapture logs;
    {
        std::unique_ptr<rhi::Device> gpu = test::gpu_device();
        if (!gpu) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        Rig rig(std::move(gpu));
        // 64x64 with 7 mips; mip n is uniformly red = 30 n, so the red channel says which mip was read.
        constexpr std::uint32_t size = 64;
        constexpr std::uint32_t levels = 7;
        std::vector<std::vector<std::byte>> data;
        for (std::uint32_t level = 0; level < levels; ++level) {
            const std::uint32_t s = rhi::mip_size(size, level);
            data.push_back(bytes_of(fill(std::size_t{s} * s, {static_cast<std::uint8_t>(30 * level), 0, 0, 255})));
        }
        std::vector<std::span<const std::byte>> mips(data.begin(), data.end());
        const rhi::TextureHandle texture =
            rig.texture({.width = size, .height = size, .debug_name = "mip ladder", .mip_levels = levels}, mips);
        const rhi::PipelineHandle sample = rig.compute(test_shaders::sampler_states(), "sample_grad", {64, 1, 1});
        SamplerStateParams p;
        p.depth = rig.device->null_descriptor(rhi::DescriptorKind::texture);
        p.compare = rig.device->null_descriptor(rhi::DescriptorKind::sampler);
        p.mips = rig.device->descriptor(texture);
        // 8 texels along u per step, 1 along v: trilinear reads near mip 3, 8x anisotropic near mip 0.
        const std::array<Float4, 1> stretched{{{0.5f, 0.5f, 8.0f / size, 1.0f / size}}};
        const auto mip_read = [&](const rhi::SamplerDesc& desc) {
            p.sampler = rig.device->descriptor(rig.sampler(desc));
            return rig.query(sample, p, stretched, {texture})[0][0] * 255.0f / 30.0f;
        };
        const float trilinear = mip_read({.address = rhi::AddressMode::repeat});
        const float anisotropic =
            mip_read({.address = rhi::AddressMode::repeat, .max_anisotropy = rhi::terrain_anisotropy});
        MESSAGE("mip read: trilinear ", trilinear, ", anisotropic x", rhi::terrain_anisotropy, " ", anisotropic);
        CHECK(trilinear > 2.5f);
        CHECK(trilinear < 3.5f);
        CHECK(anisotropic < trilinear - 1.5f);
        rig.device->wait_idle();
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}

namespace {

constexpr std::uint32_t target_size = 64;

/// Draws `draws` (pipeline, params) into a 64x64 rgba8 target cleared to black (and a depth target cleared to 0,
/// the far plane, when `depth`), and reads the target back.
std::vector<std::uint8_t> draw(Rig& rig, std::span<const std::pair<rhi::PipelineHandle, BlendStateParams>> draws,
                               bool depth) {
    const rhi::TextureHandle target =
        rig.texture({.width = target_size,
                     .height = target_size,
                     .usage = rhi::TextureUsage::color_target | rhi::TextureUsage::copy_src,
                     .debug_name = "blend target"});
    const std::uint64_t bytes = std::uint64_t{target_size} * target_size * 4;
    auto readback = rig.device->create_buffer({.size = bytes,
                                               .usage = rhi::BufferUsage::copy_dst,
                                               .memory = rhi::MemoryType::readback,
                                               .debug_name = "blend readback"});
    REQUIRE(readback.has_value());
    rig.frame([&](render::RenderGraph& graph) {
        const auto color = rig.use(graph, target);
        const auto out = graph.import(*readback, "readback", ResourceState::undefined);
        const auto depth_buffer = depth ? graph.create_texture({.width = target_size,
                                                                .height = target_size,
                                                                .format = rhi::Format::depth32_float,
                                                                .usage = rhi::TextureUsage::depth_target,
                                                                .debug_name = "depth"})
                                        : render::GraphResource{};
        graph.add_graphics_pass(
            "draw",
            [&](render::PassBuilder& b) {
                b.color_target(color, rhi::LoadOp::clear, {0, 0, 0, 0});
                if (depth) {
                    b.depth_target(depth_buffer, rhi::LoadOp::clear, 0.0f);
                }
            },
            [&](render::PassContext& ctx) {
                for (const auto& [pipeline, params] : draws) {
                    ctx.cmd.set_pipeline(pipeline);
                    ctx.cmd.set_parameters(params);
                    ctx.cmd.draw(6);
                }
            });
        graph.add_transfer_pass(
            "read back",
            [&](render::PassBuilder& b) {
                b.read(color, ResourceState::copy_src);
                b.write(out, ResourceState::copy_dst);
            },
            [&](render::PassContext& ctx) { ctx.cmd.copy_texture_to_buffer(target, *readback); });
    });
    const auto* pixels = static_cast<const std::uint8_t*>(rig.device->map(*readback));
    REQUIRE(pixels != nullptr);
    std::vector<std::uint8_t> out(pixels, pixels + bytes);
    rig.device->destroy(*readback);
    rig.device->destroy(target);
    return out;
}

rhi::PipelineHandle blend_pipeline(Rig& rig, rhi::BlendMode mode, bool depth, rhi::CompareOp compare,
                                   rhi::DepthBias bias = {}) {
    rhi::GraphicsPipelineDesc desc;
    desc.library = &test_shaders::blend_states();
    desc.vertex_entry = "vs_main";
    desc.fragment_entry = "fs_main";
    desc.color_formats = {rhi::Format::rgba8_unorm};
    desc.blend = {mode};
    if (depth) {
        desc.depth_format = rhi::Format::depth32_float;
        desc.depth_test = true;
        desc.depth_write = true;
        desc.depth_compare = compare;
        desc.depth_bias = bias;
    }
    desc.debug_name = "blend states";
    auto pipeline = rig.device->create_graphics_pipeline(desc);
    REQUIRE_MESSAGE(pipeline.has_value(), (pipeline ? std::string() : pipeline.error().message));
    return *pipeline;
}

/// The pixel at the center of column `column` of `columns`, as 0-1 floats.
Float4 pixel(const std::vector<std::uint8_t>& image, std::uint32_t column, std::uint32_t columns) {
    const std::uint32_t x = (2 * column + 1) * target_size / (2 * columns);
    const std::size_t i = (std::size_t{target_size / 2} * target_size + x) * 4;
    return unorm({image[i], image[i + 1], image[i + 2], image[i + 3]});
}

}  // namespace

TEST_CASE("[rhi.AC16] opaque, alpha, premultiplied and additive blending give their formulas' colors") {
    test::LogCapture logs;
    {
        std::unique_ptr<rhi::Device> gpu = test::gpu_device();
        if (!gpu) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        Rig rig(std::move(gpu));
        const Float4 dst{0.2f, 0.4f, 0.6f, 0.5f};
        const Float4 src{0.8f, 0.6f, 0.2f, 0.25f};
        const Float4 premultiplied{0.2f, 0.15f, 0.05f, 0.25f};
        const std::array<std::pair<rhi::BlendMode, Float4>, 4> modes{{
            {rhi::BlendMode::opaque, src},
            {rhi::BlendMode::alpha, src},
            {rhi::BlendMode::premultiplied, premultiplied},
            {rhi::BlendMode::additive, src},
        }};
        const Float4 over{0.35f, 0.45f, 0.5f, 0.625f};  // src * a + dst * (1 - a); a = a + dst.a * (1 - a)
        const std::array<Float4, 4> expected{src, over, over, Float4{1.0f, 1.0f, 0.8f, 0.5f}};
        std::vector<std::pair<rhi::PipelineHandle, BlendStateParams>> draws;
        draws.emplace_back(blend_pipeline(rig, rhi::BlendMode::opaque, false, {}), BlendStateParams{.color = dst});
        for (std::size_t i = 0; i < modes.size(); ++i) {
            const float x0 = -1.0f + 0.5f * static_cast<float>(i);
            draws.emplace_back(blend_pipeline(rig, modes.at(i).first, false, {}),
                               BlendStateParams{.color = modes.at(i).second, .rect = {x0, -1.0f, x0 + 0.5f, 1.0f}});
        }
        const std::vector<std::uint8_t> image = draw(rig, draws, false);
        for (std::uint32_t i = 0; i < 4; ++i) {
            const Float4 got = pixel(image, i, 4);
            CHECK_MESSAGE(near(got, expected.at(i), 2.5f / 255.0f), "mode ", i, ": ", str(got), " expected ",
                          str(expected.at(i)));
        }
        rig.device->wait_idle();
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}

TEST_CASE("[rhi.AC17] constant and slope depth bias move a coplanar draw in front, and only with the bias") {
    test::LogCapture logs;
    {
        std::unique_ptr<rhi::Device> gpu = test::gpu_device();
        if (!gpu) {
            MESSAGE("skipped: no GPU device");
            return;
        }
        Rig rig(std::move(gpu));
        const Float4 red{1, 0, 0, 1};
        const Float4 green{0, 1, 0, 1};
        const rhi::PipelineHandle base = blend_pipeline(rig, rhi::BlendMode::opaque, true, rhi::CompareOp::always);
        // A second draw at the same depth passes `greater` (reverse-Z: nearer) only if its bias moves it nearer.
        const auto second = [&](rhi::DepthBias bias, float slope) {
            const rhi::PipelineHandle biased =
                blend_pipeline(rig, rhi::BlendMode::opaque, true, rhi::CompareOp::greater, bias);
            const std::array<std::pair<rhi::PipelineHandle, BlendStateParams>, 2> draws{
                {{base, {.color = red, .depth = 0.5f, .slope = slope}},
                 {biased, {.color = green, .depth = 0.5f, .slope = slope}}}};
            return pixel(draw(rig, draws, true), 0, 1);
        };
        CHECK_MESSAGE(near(second({}, 0.0f), red), "no bias: the coplanar draw fails the test");
        CHECK_MESSAGE(near(second({.constant = 4}, 0.0f), green), "constant bias");
        CHECK_MESSAGE(near(second({.constant = -4}, 0.0f), red), "a negative bias pushes it away");
        CHECK_MESSAGE(near(second({.slope = 2.0f}, 0.0f), red), "a slope bias on a flat triangle adds nothing");
        CHECK_MESSAGE(near(second({.slope = 2.0f}, 0.25f), green), "slope bias on a sloped triangle");
        rig.device->wait_idle();
    }
    CHECK_MESSAGE(logs.errors().empty(), logs.errors());
}
