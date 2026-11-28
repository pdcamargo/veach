#pragma once

// RHI value types: handles, formats, descriptors of resources and pipelines,
// resource states and barriers. See ADR 0004 (RHI), 0024 (binding), 0025 (sync).

#include <veach/core/error.hpp>
#include <veach/rhi/shader.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
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

/// What a shader stores in its parameter block to reach a resource (ADR 0027).
/// Metal: gpuResourceID / gpuAddress. DX12: descriptor heap index (low 32 bits).
struct DescriptorHandle {
    std::uint64_t value = 0;
};
static_assert(sizeof(DescriptorHandle) == 8);

/// Which view of a resource a descriptor is for (DX12 has separate SRVs and UAVs).
enum class View : std::uint8_t { read, storage };

/// What a descriptor points at, for null descriptors (ADR 0027). `texture` and `storage_texture` are 2D; the
/// others match the Slang type of the same name (Texture3D, RWTexture2DArray...).
enum class DescriptorKind : std::uint8_t {
    texture,
    storage_texture,
    buffer,
    storage_buffer,
    sampler,
    texture_2d_array,
    texture_3d,
    texture_cube,
    texture_cube_array,
    storage_texture_2d_array,
    storage_texture_3d,
};

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
    rgba8_srgb,  ///< sRGB-encoded storage, linear when sampled
    bc7_unorm,   ///< 4x4 blocks of 16 bytes (ADR 0011): normal and metallic-roughness maps
    bc7_srgb,    ///< BC7, sRGB-encoded: base color and emissive
    r32_uint,    ///< integer: color target, storage, loaded (not filtered) in shaders
    rg32_uint,   ///< integer: the visibility buffer (ADR 0040)
    r8_unorm,    ///< one 8-bit channel: masks, densities
    rg8_unorm,
    r16_float,  ///< one half-float channel: heights, densities
    rg16_float,
};

/// Bytes per pixel; 0 for block-compressed formats (use row_bytes / row_count).
[[nodiscard]] std::uint32_t bytes_per_pixel(Format format);
[[nodiscard]] bool is_block_compressed(Format format);
[[nodiscard]] bool is_srgb(Format format);
/// Unsigned-integer formats: cleared with ColorAttachment::clear_uint, read with Load, never filtered.
[[nodiscard]] bool is_integer(Format format);
/// Bytes in one row of pixels, or of 4x4 blocks for block-compressed formats, tightly packed.
[[nodiscard]] std::uint64_t row_bytes(Format format, std::uint32_t width);
/// Rows of pixels, or of 4x4 blocks.
[[nodiscard]] std::uint32_t row_count(Format format, std::uint32_t height);
/// Size of mip `level` of a width x height texture: max(1, size >> level).
[[nodiscard]] constexpr std::uint32_t mip_size(std::uint32_t size, std::uint32_t level) {
    return size >> level > 0 ? size >> level : 1;
}
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
    indirect = 1u << 4,     ///< arguments of indirect draws and dispatches (VCH-071)
    index = 1u << 5,        ///< uint32 indices of indexed draws
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

/// The shape of a texture, and the Slang type of its read view (ADR 0090).
enum class TextureType : std::uint8_t {
    d2,          ///< Texture2D
    d2_array,    ///< Texture2DArray: `layers` slices
    d3,          ///< Texture3D: `depth` slices
    cube,        ///< TextureCube: 6 layers, the faces +X, -X, +Y, -Y, +Z, -Z
    cube_array,  ///< TextureCubeArray: 6 layers per cube
};

struct TextureDesc {
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    Format format = Format::rgba8_unorm;
    TextureUsage usage = TextureUsage::sampled;
    std::string debug_name;
    std::uint32_t mip_levels = 1;
    TextureType type = TextureType::d2;
    std::uint32_t depth = 1;   ///< d3 only: slices of mip 0 (mip n has mip_size(depth, n))
    std::uint32_t layers = 1;  ///< d2_array: slices; cube: 6; cube_array: 6 per cube; 1 for d2 and d3
};

/// Layers of a texture: `layers` for arrays and cubes, 1 for d2 and d3.
[[nodiscard]] std::uint32_t layer_count(const TextureDesc& desc);
/// Depth slices of mip `level`: mip_size(depth, level) for d3, 1 otherwise.
[[nodiscard]] std::uint32_t depth_of_mip(const TextureDesc& desc, std::uint32_t level);
/// Tightly packed bytes of one mip level with every layer and slice: row_bytes x row_count x depth x layers, in
/// KTX2 order (layers, then cube faces, then depth slices, then rows).
[[nodiscard]] std::uint64_t mip_bytes(const TextureDesc& desc, std::uint32_t level);
[[nodiscard]] std::string_view to_string(TextureType type);

/// "To the end": the remaining mips or layers of a TextureRange.
inline constexpr std::uint32_t remaining = UINT32_MAX;

/// Part of a texture that a descriptor views: a run of mips and of layers (array layers, cube faces). Views of a 3D
/// texture always cover all of its depth. Device::descriptor(texture, view, range).
struct TextureRange {
    std::uint32_t base_mip = 0;
    std::uint32_t mip_count = remaining;  ///< storage views see one mip: base_mip
    std::uint32_t base_layer = 0;
    std::uint32_t layer_count = remaining;
    constexpr auto operator<=>(const TextureRange&) const = default;
};

/// The mip and layer a render pass draws into: an array layer, a cube face, or a 3D texture's depth slice.
struct TargetSlice {
    std::uint32_t mip = 0;
    std::uint32_t layer = 0;
};

/// A box in one mip of a texture, over a run of layers: what a buffer-to-texture copy writes.
struct TextureRegion {
    std::uint32_t mip = 0;
    std::uint32_t base_layer = 0;
    std::uint32_t layer_count = 1;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;       ///< d3 only
    std::uint32_t width = 0;   ///< 0: to the edge of the mip
    std::uint32_t height = 0;  ///< 0: to the edge of the mip
    std::uint32_t depth = 0;   ///< 0: to the back of the mip (1 for the 2D kinds)
};

/// Texel data for copy_buffer_to_texture starts at a multiple of 512 bytes and has rows 256-byte aligned (D3D12's
/// placement and pitch rules; Metal accepts them). render::UploadRing lays data out this way.
inline constexpr std::uint64_t texture_copy_offset_alignment = 512;
inline constexpr std::uint32_t texture_copy_row_alignment = 256;
/// Bytes from one row (of 4x4 blocks, for block-compressed formats) to the next in a copy of `width` texels.
[[nodiscard]] std::uint32_t texture_copy_pitch(Format format, std::uint32_t width);
/// Bytes from one layer's data to the next in a copy of `region` (resolved): its depth slices, row_pitch x
/// row_count(height) bytes each, rounded up to texture_copy_offset_alignment.
[[nodiscard]] std::uint64_t texture_copy_layer_stride(Format format, const TextureRegion& region,
                                                      std::uint32_t row_pitch);

/// `region` with its zero extents filled in, or rhi.invalid_texture if it does not fit in the texture, or cuts
/// a 4x4 block of a block-compressed format anywhere but at the mip's edge.
[[nodiscard]] Result<TextureRegion> resolve_region(const TextureDesc& desc, const TextureRegion& region);

enum class Filter : std::uint8_t { nearest, linear };  ///< within a mip and between mips
enum class AddressMode : std::uint8_t { clamp, repeat };

/// Depth comparison: the fragment passes when `fragment op stored`. Reverse-Z (conventions.md): nearer is greater.
/// Comparison samplers use the same order: `reference op texel`.
enum class CompareOp : std::uint8_t { never, less, less_equal, greater, greater_equal, equal, always };

/// Anisotropic filtering for textures seen at grazing angles (ADR 0090): terrain and material samplers use 8;
/// 16 is the most every backend allows.
inline constexpr std::uint32_t terrain_anisotropy = 8;
inline constexpr std::uint32_t max_anisotropy_limit = 16;

struct SamplerDesc {
    Filter filter = Filter::linear;
    AddressMode address = AddressMode::clamp;
    /// 1: off. 2 to 16: anisotropic filtering, with Filter::linear.
    std::uint32_t max_anisotropy = 1;
    /// Set: a comparison sampler (Slang SamplerComparisonState, SampleCmp). Each texel reads 1 where `reference op
    /// texel` holds and 0 elsewhere, and Filter::linear averages 2x2 of them (PCF). Reverse-Z shadow maps use
    /// greater_equal: lit where the fragment is at least as near the light as the occluder.
    std::optional<CompareOp> compare = std::nullopt;
};

/// Which faces are not drawn. Front faces wind counter-clockwise, as in glTF.
enum class CullMode : std::uint8_t { none, back, front };

/// How a color target combines a fragment's color `src` with the color already there, `dst` (ADR 0090).
enum class BlendMode : std::uint8_t {
    opaque,  ///< src replaces dst
    /// Straight alpha: rgb = src.rgb * src.a + dst.rgb * (1 - src.a), a = src.a + dst.a * (1 - src.a).
    alpha,
    /// src.rgb is already multiplied by src.a: rgb = src.rgb + dst.rgb * (1 - src.a), a as for alpha.
    premultiplied,
    /// rgb = src.rgb + dst.rgb, and dst.a is kept: light is added, coverage is unchanged.
    additive,
};

/// Depth bias for shadow maps, added to each fragment's depth before the depth test: constant x r + slope x the
/// triangle's largest depth slope, then limited to |clamp| when clamp is not 0. r is the smallest step of the depth
/// format near the triangle's depth (2^(exponent - 23) for depth32_float). Reverse-Z: a negative bias pushes depth
/// away from the viewer (the light, for a shadow map).
struct DepthBias {
    std::int32_t constant = 0;  ///< whole steps: D3D12 takes an integer
    float slope = 0.0f;
    float clamp = 0.0f;
    constexpr bool operator==(const DepthBias&) const = default;
};

struct GraphicsPipelineDesc {
    const ShaderLibrary* library = nullptr;
    std::string_view vertex_entry;
    std::string_view fragment_entry;
    std::array<Format, 4> color_formats{};  ///< Format::undefined ends the list.
    /// The render pass's depth attachment format (Format::undefined: the pass has none). Must match the pass.
    Format depth_format = Format::undefined;
    bool depth_test = false;                             ///< compare against the depth attachment
    bool depth_write = false;                            ///< store passing fragments' depth
    CompareOp depth_compare = CompareOp::greater_equal;  ///< with depth_test; reverse-Z: nearer wins
    CullMode cull = CullMode::none;
    std::string debug_name;
    std::array<BlendMode, 4> blend{};  ///< per color target, opaque by default; integer targets cannot blend
    DepthBias depth_bias{};            ///< needs a depth_format
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
    /// False: every triangle of the instance is a candidate hit for queries that do not force opacity (alpha testing
    /// in a ray query loop); Metal's NonOpaque option, D3D12's FORCE_NON_OPAQUE.
    bool opaque = true;
};

// ---- render passes ---------------------------------------------------------------

enum class LoadOp : std::uint8_t { load, clear, discard };

struct ColorAttachment {
    TextureHandle texture;
    LoadOp load = LoadOp::clear;
    std::array<float, 4> clear_color{0, 0, 0, 1};  ///< float, normalized and sRGB formats
    /// Integer formats (is_integer) clear to this instead, exactly on every backend. DX12 clears through floats:
    /// values up to 2^24 and 0xFFFFFFFF (which saturates) are exact, others are rounded (and logged).
    std::array<std::uint32_t, 4> clear_uint{};
    TargetSlice slice{};  ///< the mip and layer drawn into
};

struct DepthAttachment {
    TextureHandle texture;  ///< invalid: the pass has no depth attachment
    LoadOp load = LoadOp::clear;
    float clear_depth = 0.0f;  ///< reverse-Z (conventions.md): 0 is the far plane
    TargetSlice slice{};       ///< the mip and layer drawn into (one shadow cascade of a d2_array)
};

struct RenderPassDesc {
    std::array<ColorAttachment, 4> colors{};
    std::uint32_t color_count = 0;
    DepthAttachment depth{};
    std::string_view debug_name;
};

// ---- indirect arguments (VCH-071) -------------------------------------------------

// What shaders write into BufferUsage::indirect buffers: the layouts of D3D12_DISPATCH_ARGUMENTS,
// D3D12_DRAW_ARGUMENTS and D3D12_DRAW_INDEXED_ARGUMENTS, which are also Metal's
// MTLDispatchThreadgroupsIndirectArguments, MTLDrawPrimitivesIndirectArguments and
// MTLDrawIndexedPrimitivesIndirectArguments. Keep first_instance and vertex_offset at 0 in portable code: Metal adds
// them to SV_InstanceID / SV_VertexID, D3D12 does not (ADR 0089).

struct DispatchIndirectArgs {
    std::uint32_t group_count_x = 0;
    std::uint32_t group_count_y = 1;
    std::uint32_t group_count_z = 1;
};
static_assert(sizeof(DispatchIndirectArgs) == 12);

struct DrawIndirectArgs {
    std::uint32_t vertex_count = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t first_vertex = 0;
    std::uint32_t first_instance = 0;
};
static_assert(sizeof(DrawIndirectArgs) == 16);

struct DrawIndexedIndirectArgs {
    std::uint32_t index_count = 0;
    std::uint32_t instance_count = 0;
    std::uint32_t first_index = 0;
    std::int32_t vertex_offset = 0;
    std::uint32_t first_instance = 0;
};
static_assert(sizeof(DrawIndexedIndirectArgs) == 20);

// ---- synchronization (ADR 0029) ---------------------------------------------------

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
    indirect_argument,  ///< read by draw_indirect / draw_indexed_indirect / dispatch_indirect (buffers only)
    index_read,         ///< the index buffer of an indexed draw (buffers, graphics passes only)
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
