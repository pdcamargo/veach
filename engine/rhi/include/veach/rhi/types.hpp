#pragma once

// RHI value types: handles, formats, descriptors of resources and pipelines,
// resource states and barriers. See ADR 0009 (RHI), 0024 (binding), 0025 (sync).

#include <veach/rhi/shader.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace veach::rhi {

// ---- handles ------------------------------------------------------------------

enum class ResourceKind : std::uint8_t { buffer, texture, sampler, pipeline, acceleration_structure };

/// Generational handle into a backend's slot table. 0/0 is "none".
template <ResourceKind Kind>
struct Handle {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool is_valid() const { return generation != 0; }
    constexpr auto operator<=>(const Handle&) const = default;
};

using BufferHandle = Handle<ResourceKind::buffer>;
using TextureHandle = Handle<ResourceKind::texture>;
using SamplerHandle = Handle<ResourceKind::sampler>;
using PipelineHandle = Handle<ResourceKind::pipeline>;
using AccelHandle = Handle<ResourceKind::acceleration_structure>;

/// Any buffer, texture or acceleration structure: what barriers and residency talk about.
struct ResourceRef {
    ResourceKind kind = ResourceKind::buffer;
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    ResourceRef() = default;
    template <ResourceKind K>
    ResourceRef(Handle<K> h)
        : kind(K), index(h.index), generation(h.generation) {}  // NOLINT(google-explicit-constructor)
    auto operator<=>(const ResourceRef&) const = default;
};

/// What a shader stores in its parameter block to reach a resource (ADR 0024).
/// Metal: gpuResourceID / gpuAddress. DX12: descriptor heap index (low 32 bits).
struct DescriptorHandle {
    std::uint64_t value = 0;
};
static_assert(sizeof(DescriptorHandle) == 8);

/// Which view of a resource a descriptor is for (DX12 has separate SRVs and UAVs).
enum class View : std::uint8_t { read, storage };

struct SubmissionId {
    std::uint64_t value = 0;
};

struct SurfaceId {
    std::uint32_t value = 0;
    [[nodiscard]] constexpr bool is_valid() const { return value != 0; }
};

// ---- formats and usage --------------------------------------------------------

enum class Format : std::uint8_t {
    undefined,
    rgba8_unorm,
    bgra8_unorm,
    rgba16_float,
    rgba32_float,
    r32_float,
    rgb32_float,  ///< vertex positions for acceleration structures only
    depth32_float,
};

[[nodiscard]] std::uint32_t bytes_per_pixel(Format format);
[[nodiscard]] std::string_view to_string(Format format);

enum class MemoryType : std::uint8_t {
    gpu,       ///< device-local
    upload,    ///< CPU writes, GPU reads (persistently mapped)
    readback,  ///< GPU writes, CPU reads (persistently mapped)
};

/// Bit flags.
enum class BufferUsage : std::uint32_t {
    none = 0,
    storage = 1u << 0,  ///< shader-visible (read or read-write through a descriptor)
    copy_src = 1u << 1,
    copy_dst = 1u << 2,
    accel_input = 1u << 3,  ///< vertex/index data for acceleration structure builds
};

enum class TextureUsage : std::uint32_t {
    none = 0,
    sampled = 1u << 0,
    storage = 1u << 1,
    color_target = 1u << 2,
    depth_target = 1u << 3,
    copy_src = 1u << 4,
    copy_dst = 1u << 5,
};

constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr bool has(BufferUsage set, BufferUsage flag) {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(flag)) != 0;
}
constexpr bool has(TextureUsage set, TextureUsage flag) {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(flag)) != 0;
}

// ---- resource descriptions ------------------------------------------------------

struct BufferDesc {
    std::uint64_t size = 0;
    BufferUsage usage = BufferUsage::storage;
    MemoryType memory = MemoryType::gpu;
    std::string debug_name;
    /// Element size for StructuredBuffer<T> views (sizeof(T)); 0 means raw (ByteAddressBuffer). DX12 needs it.
    std::uint32_t stride = 0;
};

struct TextureDesc {
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    Format format = Format::rgba8_unorm;
    TextureUsage usage = TextureUsage::sampled;
    std::string debug_name;
};

enum class Filter : std::uint8_t { nearest, linear };
enum class AddressMode : std::uint8_t { clamp, repeat };

struct SamplerDesc {
    Filter filter = Filter::linear;
    AddressMode address = AddressMode::clamp;
};

struct GraphicsPipelineDesc {
    const ShaderLibrary* library = nullptr;
    std::string_view vertex_entry;
    std::string_view fragment_entry;
    std::array<Format, 4> color_formats{};  ///< Format::undefined ends the list.
    Format depth_format = Format::undefined;
    std::string debug_name;
};

struct ComputePipelineDesc {
    const ShaderLibrary* library = nullptr;
    std::string_view entry;
    /// Must equal the shader's [numthreads(...)]: Metal takes it at dispatch time, not from the shader.
    std::array<std::uint32_t, 3> threads_per_group{8, 8, 1};
    std::string debug_name;
};

// ---- acceleration structures ------------------------------------------------------

/// Bottom level: triangles. Vertex positions are Format::rgb32_float.
struct BlasDesc {
    BufferHandle vertices;
    std::uint32_t vertex_count = 0;
    std::uint32_t vertex_stride = 12;
    BufferHandle indices;  ///< optional, uint32
    std::uint32_t index_count = 0;
    bool opaque = true;
    bool allow_refit = false;
    std::string debug_name;
};

/// Top level. Every BLAS an instance may reference is listed here; instances
/// select one by index (Metal needs the full set when the TLAS is created).
struct TlasDesc {
    std::uint32_t max_instances = 1;
    std::span<const AccelHandle> blases;
    bool allow_refit = false;
    std::string debug_name;
};

struct TlasInstance {
    /// Object-to-world, 3 rows x 4 columns, row-major (the D3D12 instance layout).
    std::array<std::array<float, 4>, 3> transform{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    std::uint32_t blas_index = 0;  ///< index into TlasDesc::blases
    std::uint32_t instance_id = 0;
    std::uint8_t mask = 0xFF;
};

// ---- render passes ---------------------------------------------------------------

enum class LoadOp : std::uint8_t { load, clear, discard };

struct ColorAttachment {
    TextureHandle texture;
    LoadOp load = LoadOp::clear;
    std::array<float, 4> clear_color{0, 0, 0, 1};
};

struct RenderPassDesc {
    std::array<ColorAttachment, 4> colors{};
    std::uint32_t color_count = 0;
    std::string_view debug_name;
};

// ---- synchronization (ADR 0025) ---------------------------------------------------

enum class ResourceState : std::uint8_t {
    undefined,
    shader_read,
    shader_write,
    color_target,
    depth_write,
    copy_src,
    copy_dst,
    accel_build,
    accel_read,
    present,
};

[[nodiscard]] std::string_view to_string(ResourceState state);
[[nodiscard]] constexpr bool is_write(ResourceState s) {
    return s == ResourceState::shader_write || s == ResourceState::color_target || s == ResourceState::depth_write ||
           s == ResourceState::copy_dst || s == ResourceState::accel_build;
}

struct Barrier {
    ResourceRef resource;
    ResourceState before = ResourceState::undefined;
    ResourceState after = ResourceState::undefined;
    /// The old contents are not needed (first use of a transient this frame). `before` is still the real
    /// previous state, so the barrier synchronizes with earlier frames' use; DX12 discards the layout.
    bool discard = false;
};

}  // namespace veach::rhi
