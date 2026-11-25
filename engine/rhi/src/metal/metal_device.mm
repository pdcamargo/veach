// Metal backend (ADR 0009). Metal 3: bindless through gpuResourceID/gpuAddress
// (ADR 0024); one encoder per graph pass with tracked hazards, barriers
// validated in debug builds (ADR 0025).

#include "metal_device.hpp"

#include "../frame_timings.hpp"
#include "../pipeline_validation.hpp"
#include "../slot_map.hpp"
#include "../surface_validation.hpp"
#include "../texture_validation.hpp"

#include <veach/core/assert.hpp>
#include <veach/core/log.hpp>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace veach::rhi {

namespace {

MTLPixelFormat to_mtl(Format format) {
    switch (format) {
        case Format::rgba8_unorm: return MTLPixelFormatRGBA8Unorm;
        case Format::bgra8_unorm: return MTLPixelFormatBGRA8Unorm;
        case Format::rgba16_float: return MTLPixelFormatRGBA16Float;
        case Format::rgba32_float: return MTLPixelFormatRGBA32Float;
        case Format::r32_float: return MTLPixelFormatR32Float;
        case Format::depth32_float: return MTLPixelFormatDepth32Float;
        case Format::rgba8_srgb: return MTLPixelFormatRGBA8Unorm_sRGB;
        case Format::bc7_unorm: return MTLPixelFormatBC7_RGBAUnorm;
        case Format::bc7_srgb: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
        case Format::r32_uint: return MTLPixelFormatR32Uint;
        case Format::rg32_uint: return MTLPixelFormatRG32Uint;
        case Format::rgb32_float:
        case Format::undefined: return MTLPixelFormatInvalid;
    }
    return MTLPixelFormatInvalid;
}

NSString* ns(std::string_view text) {
    return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
}

std::string describe(NSError* error) {
    return error ? std::string(error.localizedDescription.UTF8String) : std::string("unknown error");
}

MTLCompareFunction to_mtl(CompareOp op) {
    switch (op) {
        case CompareOp::never: return MTLCompareFunctionNever;
        case CompareOp::less: return MTLCompareFunctionLess;
        case CompareOp::less_equal: return MTLCompareFunctionLessEqual;
        case CompareOp::greater: return MTLCompareFunctionGreater;
        case CompareOp::greater_equal: return MTLCompareFunctionGreaterEqual;
        case CompareOp::equal: return MTLCompareFunctionEqual;
        case CompareOp::always: return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionAlways;
}

MTLCullMode to_mtl(CullMode mode) {
    switch (mode) {
        case CullMode::none: return MTLCullModeNone;
        case CullMode::back: return MTLCullModeBack;
        case CullMode::front: return MTLCullModeFront;
    }
    return MTLCullModeNone;
}

MTLLoadAction to_mtl(LoadOp load) {
    switch (load) {
        case LoadOp::clear: return MTLLoadActionClear;
        case LoadOp::load: return MTLLoadActionLoad;
        case LoadOp::discard: return MTLLoadActionDontCare;
    }
    return MTLLoadActionDontCare;
}

/// Timestamps per frame slot: one start/end pair per timed encoder (VCH-049). A counter sample buffer holds at
/// most 32 KB (4,096 timestamps), so a slot has several: the first frame builds one BLAS per mesh, an encoder each.
constexpr std::uint32_t timing_samples_per_buffer = 4096;
constexpr std::uint32_t timing_buffers_per_frame = 8;
constexpr std::uint32_t timing_samples_per_frame = timing_samples_per_buffer * timing_buffers_per_frame;

MTLResourceUsage usage_for(ResourceState state) {
    return is_write(state) ? (MTLResourceUsageRead | MTLResourceUsageWrite) : MTLResourceUsageRead;
}

struct MetalBuffer {
    id<MTLBuffer> buffer;
    BufferDesc desc;
};

struct MetalTexture {
    id<MTLTexture> texture;
    TextureDesc desc;
    bool drawable = false;  ///< a swapchain image; not owned
};

struct MetalPipeline {
    id<MTLRenderPipelineState> render;
    id<MTLComputePipelineState> compute;
    MTLSize threads_per_group{1, 1, 1};
    id<MTLDepthStencilState> depth;  ///< render only; set with the pipeline
    MTLCullMode cull = MTLCullModeNone;
};

struct MetalAccel {
    id<MTLAccelerationStructure> accel;
    MTLAccelerationStructureDescriptor* descriptor = nil;
    id<MTLBuffer> scratch;
    /// TLAS only: MTLAccelerationStructureInstanceDescriptor[max], one buffer per frame slot because the CPU
    /// writes them while earlier frames may still be building from theirs (ADR 0029).
    std::vector<id<MTLBuffer>> instances;
    std::vector<AccelHandle> blases;
    std::uint32_t max_instances = 0;
    bool tlas = false;
};

struct MetalSurface {
    CAMetalLayer* layer = nil;
    id<CAMetalDrawable> drawable;
    TextureHandle image;
};

class MetalDevice;

class MetalCommandList final : public CommandList {
public:
    MetalCommandList(MetalDevice& device, id<MTLCommandBuffer> buffer) : device_(device), buffer_(buffer) {}

    void begin_render_pass(const RenderPassDesc& desc) override;
    void end_render_pass() override { end_encoder(); }
    void begin_compute_pass(std::string_view name) override;
    void end_compute_pass() override { end_encoder(); }
    void set_pipeline(PipelineHandle pipeline) override;
    void set_parameters(const void* data, std::size_t size) override;
    void draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex) override;
    void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) override;
    void build_blas(AccelHandle blas) override;
    void refit_blas(AccelHandle blas) override;
    void build_tlas(AccelHandle tlas, std::span<const TlasInstance> instances) override;
    void refit_tlas(AccelHandle tlas, std::span<const TlasInstance> instances) override;
    void copy_texture_to_buffer(TextureHandle src, BufferHandle dst) override;
    void copy_buffer(BufferHandle src, BufferHandle dst, std::uint64_t size) override;
    void use(ResourceRef resource, ResourceState state) override;
    void barrier(std::span<const Barrier> barriers) override;
    void push_debug_group(std::string_view name) override { [buffer_ pushDebugGroup:ns(name)]; }
    void pop_debug_group() override { [buffer_ popDebugGroup]; }
    void begin_timing(std::string_view name) override;
    void end_timing() override;

    [[nodiscard]] id<MTLCommandBuffer> buffer() const { return buffer_; }
    void end_encoder();

private:
    /// The sample buffer and the (start, end) indices for the next encoder, when a timing region is open.
    struct Samples {
        id<MTLCounterSampleBuffer> buffer;
        NSUInteger start = MTLCounterDontSample;
        NSUInteger end = MTLCounterDontSample;
    };
    Samples next_samples();
    id<MTLBlitCommandEncoder> blit_encoder();

    void write_instances(MetalAccel& tlas, std::span<const TlasInstance> instances);
    void encode_accel(AccelHandle handle, bool refit);
    void use_one(id<MTLResource> resource, ResourceState state);

    MetalDevice& device_;
    id<MTLCommandBuffer> buffer_;
    id<MTLRenderCommandEncoder> render_;
    id<MTLComputeCommandEncoder> compute_;
    MTLSize threads_per_group_{1, 1, 1};
    std::uint32_t timing_slot_ = 0;  ///< frame slot of the open timing region
    bool timing_ = false;
    std::map<ResourceRef, ResourceState> states_;  ///< debug validation of barriers
};

/// Shared with command-buffer completion handlers, which can run after the device is gone.
struct CompletionState {
    explicit CompletionState(std::uint32_t frames) : tracker(frames) {}
    SubmissionTracker tracker;
    std::atomic<double> last_gpu_ms{0.0};
};

class MetalDevice final : public Device {
public:
    MetalDevice(id<MTLDevice> device, bool validation, std::uint32_t frames_in_flight)
        : device_(device), validation_(validation), state_(std::make_shared<CompletionState>(frames_in_flight)) {
        queue_ = [device newCommandQueue];
        caps_.device_name = device.name.UTF8String;
        caps_.gpu = true;
        caps_.hardware_ray_tracing = device.supportsRaytracing;
        caps_.bindless =
            [device supportsFamily:MTLGPUFamilyMetal3] && device.argumentBuffersSupport == MTLArgumentBuffersTier2;
        caps_.mesh_shaders = [device supportsFamily:MTLGPUFamilyApple7] || [device supportsFamily:MTLGPUFamilyMac2];
        create_null_resources();
        create_timing_buffers(frames_in_flight);
    }

    /// VCH-049: one shared counter sample buffer per frame slot, sampled at encoder boundaries. GPU ticks become
    /// nanoseconds through pairs of (CPU ns, GPU ticks) from sampleTimestamps.
    void create_timing_buffers(std::uint32_t frames_in_flight) {
        if (![device_ supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
            timing_unavailable_ =
                std::format("'{}' cannot sample GPU timestamps at encoder boundaries", caps_.device_name);
            return;
        }
        id<MTLCounterSet> timestamps = nil;
        for (id<MTLCounterSet> set in device_.counterSets) {
            if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) {
                timestamps = set;
            }
        }
        if (!timestamps) {
            timing_unavailable_ = std::format("'{}' has no timestamp counter set", caps_.device_name);
            return;
        }
        MTLCounterSampleBufferDescriptor* d = [MTLCounterSampleBufferDescriptor new];
        d.counterSet = timestamps;
        d.storageMode = MTLStorageModeShared;
        d.sampleCount = timing_samples_per_buffer;
        d.label = @"pass timings";
        for (std::uint32_t slot = 0; slot < std::max(1u, frames_in_flight); ++slot) {
            std::vector<id<MTLCounterSampleBuffer>> buffers;
            for (std::uint32_t i = 0; i < timing_buffers_per_frame; ++i) {
                NSError* error = nil;
                id<MTLCounterSampleBuffer> buffer = [device_ newCounterSampleBufferWithDescriptor:d error:&error];
                if (!buffer) {
                    timing_buffers_.clear();
                    timing_unavailable_ =
                        std::format("Metal: cannot create a counter sample buffer: {}", describe(error));
                    return;
                }
                buffers.push_back(buffer);
            }
            timing_buffers_.push_back(std::move(buffers));
        }
        [device_ sampleTimestamps:&calibration_cpu_ gpuTimestamp:&calibration_gpu_];
    }

    /// Resolves the timings of frames that have completed (never waits).
    void collect_timings() {
        if (timing_buffers_.empty()) {
            return;
        }
        MTLTimestamp cpu = 0;
        MTLTimestamp gpu = 0;
        [device_ sampleTimestamps:&cpu gpuTimestamp:&gpu];
        // Apple GPUs tick in nanoseconds; others need not. Calibrate against the CPU clock, which is in nanoseconds.
        const double ns_per_tick =
            gpu > calibration_gpu_ && cpu > calibration_cpu_
                ? static_cast<double>(cpu - calibration_cpu_) / static_cast<double>(gpu - calibration_gpu_)
                : 1.0;
        timings_.collect(
            tracker().completed().value,
            [this](std::uint32_t slot, std::uint32_t count) {
                std::vector<std::uint64_t> out(count, MTLCounterErrorValue);
                for (std::uint32_t first = 0; first < count; first += timing_samples_per_buffer) {
                    const std::uint32_t n = std::min(count - first, timing_samples_per_buffer);
                    NSData* data = [timing_buffers_.at(slot).at(first / timing_samples_per_buffer)
                        resolveCounterRange:NSMakeRange(0, n)];
                    if (data) {
                        std::memcpy(out.data() + first, data.bytes,
                                    std::min<std::size_t>(data.length, std::size_t{n} * sizeof(std::uint64_t)));
                    }
                }
                return out;
            },
            [ns_per_tick](std::uint64_t ticks) { return static_cast<double>(ticks) * ns_per_tick / 1e6; },
            MTLCounterErrorValue);
    }

    [[nodiscard]] FrameTimings last_frame_timings() override {
        collect_timings();
        FrameTimings out = timings_.published();
        out.unavailable = timing_unavailable_;
        return out;
    }

    // Used by MetalCommandList's timing regions: the buffer holding sample `index` of a slot (pairs never span two).
    [[nodiscard]] bool timing_supported() const { return !timing_buffers_.empty(); }
    [[nodiscard]] id<MTLCounterSampleBuffer> timing_buffer(std::uint32_t slot, std::uint32_t index) const {
        return timing_buffers_.at(slot % timing_buffers_.size()).at(index / timing_samples_per_buffer);
    }
    [[nodiscard]] FrameTimingSlots& timing_slots() { return timings_; }

    ~MetalDevice() override { wait_idle(); }

    /// ADR 0039: shared storage, so the zeros are written once from the CPU.
    void create_null_resources() {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:1
                                                                                    height:1
                                                                                 mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        null_texture_ = [device_ newTextureWithDescriptor:d];
        const std::array<std::uint8_t, 4> zero{};
        [null_texture_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:zero.data() bytesPerRow:4];
        null_texture_.label = @"null texture";
        null_buffer_ = [device_ newBufferWithLength:16 options:MTLResourceStorageModeShared];
        std::memset(null_buffer_.contents, 0, 16);
        null_buffer_.label = @"null buffer";
        MTLSamplerDescriptor* s = [MTLSamplerDescriptor new];
        s.supportArgumentBuffers = YES;
        null_sampler_ = [device_ newSamplerStateWithDescriptor:s];
    }

    [[nodiscard]] SubmissionTracker& tracker() { return state_->tracker; }
    MetalDevice(const MetalDevice&) = delete;
    MetalDevice& operator=(const MetalDevice&) = delete;
    MetalDevice(MetalDevice&&) = delete;
    MetalDevice& operator=(MetalDevice&&) = delete;

    [[nodiscard]] Backend backend() const override { return Backend::metal; }
    [[nodiscard]] const Capabilities& capabilities() const override { return caps_; }

    [[nodiscard]] Result<BufferHandle> create_buffer(const BufferDesc& desc,
                                                     std::span<const std::byte> initial) override {
        // Unified memory: shared storage keeps v0 simple and lets initial data be written directly.
        id<MTLBuffer> buffer =
            [device_ newBufferWithLength:std::max<std::uint64_t>(desc.size, 16)
                                 options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked];
        if (!buffer) {
            return make_error(ErrorCode::rhi_backend_unavailable,
                              std::format("Metal: cannot allocate {} bytes", desc.size));
        }
        if (!desc.debug_name.empty()) {
            buffer.label = ns(desc.debug_name);
        }
        if (!initial.empty()) {
            std::memcpy(buffer.contents, initial.data(), std::min<std::size_t>(initial.size(), desc.size));
        }
        return buffers_.insert({buffer, desc});
    }

    [[nodiscard]] Result<TextureHandle> create_texture(const TextureDesc& desc,
                                                       std::span<const std::span<const std::byte>> mips) override {
        if (auto valid = validate_texture_desc(desc); !valid) {
            return std::unexpected(valid.error());
        }
        if (is_block_compressed(desc.format) && !device_.supportsBCTextureCompression) {
            return make_error(ErrorCode::rhi_backend_unavailable,
                              std::format("'{}' does not support BC texture compression", caps_.device_name),
                              "BC7 textures (ADR 0032) need an Apple-silicon Mac.");
        }
        const std::uint32_t levels = std::max(1u, desc.mip_levels);
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:to_mtl(desc.format)
                                                                                     width:desc.width
                                                                                    height:desc.height
                                                                                 mipmapped:NO];
        d.mipmapLevelCount = levels;
        d.storageMode = MTLStorageModePrivate;
        d.hazardTrackingMode = MTLHazardTrackingModeTracked;
        MTLTextureUsage usage = 0;
        if (has(desc.usage, TextureUsage::sampled)) {
            usage |= MTLTextureUsageShaderRead;
        }
        if (has(desc.usage, TextureUsage::storage)) {
            usage |= MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
        }
        if (has(desc.usage, TextureUsage::color_target) || has(desc.usage, TextureUsage::depth_target)) {
            usage |= MTLTextureUsageRenderTarget;
        }
        d.usage = usage;
        id<MTLTexture> texture = [device_ newTextureWithDescriptor:d];
        if (!texture) {
            return make_error(ErrorCode::rhi_backend_unavailable,
                              std::format("Metal: cannot create a {}x{} {} texture", desc.width, desc.height,
                                          to_string(desc.format)));
        }
        if (!desc.debug_name.empty()) {
            texture.label = ns(desc.debug_name);
        }
        if (!mips.empty()) {
            if (mips.size() != levels) {
                return make_error(ErrorCode::internal, std::format("create_texture '{}': {} mip levels, data for {}",
                                                                   desc.debug_name, levels, mips.size()));
            }
            upload(texture, desc, mips);
        }
        return textures_.insert({texture, desc, false});
    }

    /// Private textures are filled by a blit from a shared staging buffer, committed now: the queue runs in
    /// order, so every later submission sees the data. The command buffer keeps the staging buffer alive.
    void upload(id<MTLTexture> texture, const TextureDesc& desc, std::span<const std::span<const std::byte>> mips) {
        std::size_t total = 0;
        for (const auto& mip : mips) {
            total += mip.size();
        }
        id<MTLBuffer> staging = [device_ newBufferWithLength:std::max<std::size_t>(total, 16)
                                                     options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> commands = [queue_ commandBuffer];
        commands.label = @"texture upload";
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        std::size_t offset = 0;
        for (std::uint32_t level = 0; level < mips.size(); ++level) {
            const std::uint32_t w = mip_size(desc.width, level);
            const std::uint32_t h = mip_size(desc.height, level);
            const std::uint64_t row = row_bytes(desc.format, w);
            std::memcpy(static_cast<std::byte*>(staging.contents) + offset, mips[level].data(), mips[level].size());
            [blit copyFromBuffer:staging
                       sourceOffset:offset
                  sourceBytesPerRow:row
                sourceBytesPerImage:row * row_count(desc.format, h)
                         sourceSize:MTLSizeMake(w, h, 1)
                          toTexture:texture
                   destinationSlice:0
                   destinationLevel:level
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
            offset += mips[level].size();
        }
        [blit endEncoding];
        [commands commit];
    }

    [[nodiscard]] Result<SamplerHandle> create_sampler(const SamplerDesc& desc) override {
        MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
        const MTLSamplerMinMagFilter filter =
            desc.filter == Filter::linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.minFilter = filter;
        d.magFilter = filter;
        const MTLSamplerAddressMode address =
            desc.address == AddressMode::repeat ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
        d.sAddressMode = address;
        d.tAddressMode = address;
        d.supportArgumentBuffers = YES;
        return samplers_.insert([device_ newSamplerStateWithDescriptor:d]);
    }

    Result<id<MTLLibrary>> library(const ShaderLibrary* lib) {
        if (!lib || lib->metallib.empty()) {
            return make_error(ErrorCode::rhi_not_implemented,
                              std::format("shader library '{}' has no Metal code", lib ? lib->name : "(null)"),
                              "Compile it with veach_add_shader() on macOS (docs/adr/0026-shader-compilation.md).");
        }
        if (const auto it = libraries_.find(lib); it != libraries_.end()) {
            return it->second;
        }
        dispatch_data_t data =
            dispatch_data_create(lib->metallib.data(), lib->metallib.size(), nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError* error = nil;
        id<MTLLibrary> library = [device_ newLibraryWithData:data error:&error];
        if (!library) {
            return make_error(ErrorCode::rhi_backend_unavailable,
                              std::format("Metal: cannot load shader library '{}': {}", lib->name, describe(error)));
        }
        libraries_.emplace(lib, library);
        return library;
    }

    Result<id<MTLFunction>> function(const ShaderLibrary* lib, std::string_view entry) {
        auto library_result = library(lib);
        if (!library_result) {
            return std::unexpected(library_result.error());
        }
        id<MTLFunction> fn = [*library_result newFunctionWithName:ns(entry)];
        if (!fn) {
            return make_error(ErrorCode::rhi_backend_unavailable,
                              std::format("shader library '{}' has no entry point '{}'", lib->name, entry),
                              "Check the ENTRIES list passed to veach_add_shader().");
        }
        return fn;
    }

    [[nodiscard]] Result<PipelineHandle> create_graphics_pipeline(const GraphicsPipelineDesc& desc) override {
        if (auto valid = validate_graphics_pipeline_desc(desc); !valid) {
            return std::unexpected(valid.error());
        }
        auto vs = function(desc.library, desc.vertex_entry);
        auto fs = function(desc.library, desc.fragment_entry);
        if (!vs) {
            return std::unexpected(vs.error());
        }
        if (!fs) {
            return std::unexpected(fs.error());
        }
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = *vs;
        d.fragmentFunction = *fs;
        for (std::size_t i = 0; i < desc.color_formats.size() && desc.color_formats[i] != Format::undefined; ++i) {
            d.colorAttachments[i].pixelFormat = to_mtl(desc.color_formats[i]);
        }
        if (desc.depth_format != Format::undefined) {
            d.depthAttachmentPixelFormat = to_mtl(desc.depth_format);
        }
        if (!desc.debug_name.empty()) {
            d.label = ns(desc.debug_name);
        }
        NSError* error = nil;
        id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:d error:&error];
        if (!state) {
            return make_error(
                ErrorCode::rhi_backend_unavailable,
                std::format("Metal: graphics pipeline '{}' failed: {}", desc.debug_name, describe(error)));
        }
        // Writing depth without testing it is a test that always passes (as on D3D12).
        MTLDepthStencilDescriptor* depth = [MTLDepthStencilDescriptor new];
        depth.depthCompareFunction = desc.depth_test ? to_mtl(desc.depth_compare) : MTLCompareFunctionAlways;
        depth.depthWriteEnabled = desc.depth_write;
        return pipelines_.insert(
            {state, nil, MTLSizeMake(1, 1, 1), [device_ newDepthStencilStateWithDescriptor:depth], to_mtl(desc.cull)});
    }

    [[nodiscard]] Result<PipelineHandle> create_compute_pipeline(const ComputePipelineDesc& desc) override {
        auto cs = function(desc.library, desc.entry);
        if (!cs) {
            return std::unexpected(cs.error());
        }
        NSError* error = nil;
        id<MTLComputePipelineState> state = [device_ newComputePipelineStateWithFunction:*cs error:&error];
        if (!state) {
            return make_error(ErrorCode::rhi_backend_unavailable,
                              std::format("Metal: compute pipeline '{}' failed: {}", desc.debug_name, describe(error)));
        }
        const auto& g = desc.threads_per_group;
        if (std::uint64_t{g[0]} * g[1] * g[2] > state.maxTotalThreadsPerThreadgroup) {
            return make_error(
                ErrorCode::rhi_backend_unavailable,
                std::format("compute pipeline '{}': {}x{}x{} threads per group exceeds the device limit {}",
                            desc.debug_name, g[0], g[1], g[2], state.maxTotalThreadsPerThreadgroup));
        }
        return pipelines_.insert({nil, state, MTLSizeMake(g[0], g[1], g[2]), nil, MTLCullModeNone});
    }

    Result<AccelHandle> make_accel(MTLAccelerationStructureDescriptor* d, MetalAccel accel, std::string_view name) {
        if (!caps_.hardware_ray_tracing) {
            return make_error(ErrorCode::rhi_backend_unavailable,
                              std::format("'{}' does not support ray tracing", caps_.device_name),
                              "Acceleration structures need an Apple GPU with Metal ray tracing (M1 or newer).");
        }
        const MTLAccelerationStructureSizes sizes = [device_ accelerationStructureSizesWithDescriptor:d];
        accel.accel = [device_ newAccelerationStructureWithSize:sizes.accelerationStructureSize];
        accel.scratch = [device_
            newBufferWithLength:std::max<NSUInteger>({sizes.buildScratchBufferSize, sizes.refitScratchBufferSize, 16})
                        options:MTLResourceStorageModePrivate];
        accel.descriptor = d;
        if (!name.empty()) {
            accel.accel.label = ns(name);
        }
        return accels_.insert(std::move(accel));
    }

    [[nodiscard]] Result<AccelHandle> create_blas(const BlasDesc& desc) override {
        const MetalBuffer* vertices = buffers_.get(desc.vertices);
        if (!vertices) {
            return make_error(ErrorCode::internal, "create_blas: invalid vertex buffer");
        }
        MTLAccelerationStructureTriangleGeometryDescriptor* geometry =
            [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        geometry.vertexBuffer = vertices->buffer;
        geometry.vertexStride = desc.vertex_stride;
        geometry.vertexFormat = MTLAttributeFormatFloat3;
        geometry.opaque = desc.opaque;
        if (const MetalBuffer* indices = buffers_.get(desc.indices)) {
            geometry.indexBuffer = indices->buffer;
            geometry.indexType = MTLIndexTypeUInt32;
            geometry.triangleCount = desc.index_count / 3;
        } else {
            geometry.triangleCount = desc.vertex_count / 3;
        }
        MTLPrimitiveAccelerationStructureDescriptor* d = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        d.geometryDescriptors = @[ geometry ];
        if (desc.allow_refit) {
            d.usage = MTLAccelerationStructureUsageRefit;
        }
        return make_accel(d, MetalAccel{}, desc.debug_name);
    }

    [[nodiscard]] Result<AccelHandle> create_tlas(const TlasDesc& desc) override {
        NSMutableArray<id<MTLAccelerationStructure>>* blases = [NSMutableArray array];
        MetalAccel accel;
        for (const AccelHandle h : desc.blases) {
            const MetalAccel* blas = accels_.get(h);
            if (!blas || blas->tlas) {
                return make_error(ErrorCode::internal, "create_tlas: invalid BLAS handle");
            }
            [blases addObject:blas->accel];
            accel.blases.push_back(h);
        }
        accel.tlas = true;
        accel.max_instances = desc.max_instances;
        for (std::uint32_t slot = 0; slot < tracker().frames_in_flight(); ++slot) {
            accel.instances.push_back([device_ newBufferWithLength:sizeof(MTLAccelerationStructureInstanceDescriptor) *
                                                                   std::max(1u, desc.max_instances)
                                                           options:MTLResourceStorageModeShared]);
        }
        MTLInstanceAccelerationStructureDescriptor* d = [MTLInstanceAccelerationStructureDescriptor descriptor];
        d.instancedAccelerationStructures = blases;
        d.instanceCount = desc.max_instances;
        d.instanceDescriptorBuffer = accel.instances.front();
        d.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeDefault;
        if (desc.allow_refit) {
            d.usage = MTLAccelerationStructureUsageRefit;
        }
        return make_accel(d, std::move(accel), desc.debug_name);
    }

    void destroy(BufferHandle h) override { retire(buffers_, h); }
    void destroy(TextureHandle h) override { retire(textures_, h); }
    void destroy(SamplerHandle h) override { retire(samplers_, h); }
    void destroy(PipelineHandle h) override { retire(pipelines_, h); }
    void destroy(AccelHandle h) override { retire(accels_, h); }

    /// The handle dies now; the Objective-C object (kept alive by the closure) once the GPU is done with it.
    template <class Map, class H>
    void retire(Map& map, H h) {
        if (auto* object = map.get(h)) {
            auto keep = std::make_shared<std::decay_t<decltype(*object)>>(std::move(*object));
            map.erase(h);
            tracker().retire([keep] {});
        }
    }

    [[nodiscard]] void* map(BufferHandle h) override {
        MetalBuffer* b = buffers_.get(h);
        return b ? b->buffer.contents : nullptr;
    }
    [[nodiscard]] const TextureDesc* texture_desc(TextureHandle h) const override {
        const MetalTexture* t = textures_.get(h);
        return t ? &t->desc : nullptr;
    }

    [[nodiscard]] DescriptorHandle descriptor(BufferHandle h, View) override {
        const MetalBuffer* b = buffers_.get(h);
        return {b ? b->buffer.gpuAddress : 0};
    }
    [[nodiscard]] DescriptorHandle descriptor(TextureHandle h, View) override {
        const MetalTexture* t = textures_.get(h);
        return {t ? t->texture.gpuResourceID._impl : 0};
    }
    [[nodiscard]] DescriptorHandle descriptor(SamplerHandle h) override {
        auto* s = samplers_.get(h);
        return {s ? (*s).gpuResourceID._impl : 0};
    }
    [[nodiscard]] DescriptorHandle null_descriptor(DescriptorKind kind) override {
        switch (kind) {
            case DescriptorKind::texture:
            case DescriptorKind::storage_texture: return {null_texture_.gpuResourceID._impl};
            case DescriptorKind::buffer:
            case DescriptorKind::storage_buffer: return {null_buffer_.gpuAddress};
            case DescriptorKind::sampler: return {null_sampler_.gpuResourceID._impl};
        }
        return {};
    }

    /// Null descriptors are reached through handles, so every encoder makes them resident.
    void use_null_resources(id<MTLComputeCommandEncoder> encoder) const {
        [encoder useResource:null_texture_ usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [encoder useResource:null_buffer_ usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    }
    void use_null_resources(id<MTLRenderCommandEncoder> encoder) const {
        const MTLRenderStages stages = MTLRenderStageVertex | MTLRenderStageFragment;
        [encoder useResource:null_texture_ usage:MTLResourceUsageRead stages:stages];
        [encoder useResource:null_buffer_ usage:MTLResourceUsageRead stages:stages];
    }

    [[nodiscard]] DescriptorHandle descriptor(AccelHandle h) override {
        const MetalAccel* a = accels_.get(h);
        return {a ? a->accel.gpuResourceID._impl : 0};
    }

    [[nodiscard]] Result<SurfaceId> create_surface(const NativeWindowHandle& window, const SurfaceDesc& desc) override {
        if (auto valid = validate_surface_handle(window); !valid) {
            return std::unexpected(valid.error());
        }
        if (window.kind != NativeWindowHandle::Kind::metal) {
            return make_error(ErrorCode::rhi_invalid_surface, "the Metal backend needs a CAMetalLayer surface");
        }
        CAMetalLayer* layer = (__bridge CAMetalLayer*)window.metal_layer;
        layer.device = device_;
        layer.pixelFormat = to_mtl(surface_format);
        layer.framebufferOnly = YES;
        layer.displaySyncEnabled = desc.vsync;
        layer.drawableSize = CGSizeMake(desc.width, desc.height);
        surfaces_.push_back({layer, nil, {}});
        return SurfaceId{static_cast<std::uint32_t>(surfaces_.size())};
    }

    MetalSurface* surface(SurfaceId id) {
        return id.is_valid() && id.value <= surfaces_.size() ? &surfaces_[id.value - 1] : nullptr;
    }

    void resize_surface(SurfaceId id, std::uint32_t width, std::uint32_t height) override {
        if (MetalSurface* s = surface(id)) {
            s->layer.drawableSize = CGSizeMake(width, height);
        }
    }

    [[nodiscard]] Result<TextureHandle> acquire_image(SurfaceId id) override {
        MetalSurface* s = surface(id);
        if (!s) {
            return make_error(ErrorCode::rhi_invalid_surface, "unknown surface");
        }
        s->drawable = [s->layer nextDrawable];
        if (!s->drawable) {
            return make_error(ErrorCode::rhi_invalid_surface, "Metal: no drawable available");
        }
        textures_.erase(s->image);
        const CGSize size = s->layer.drawableSize;
        s->image = textures_.insert({s->drawable.texture,
                                     {static_cast<std::uint32_t>(size.width), static_cast<std::uint32_t>(size.height),
                                      surface_format, TextureUsage::color_target, "swapchain"},
                                     true});
        return s->image;
    }

    void destroy_surface(SurfaceId id) override {
        if (MetalSurface* s = surface(id)) {
            wait_idle();
            textures_.erase(s->image);
            s->drawable = nil;
            s->layer = nil;
        }
    }

    [[nodiscard]] std::unique_ptr<CommandList> begin_commands(std::string_view name) override {
        MTLCommandBufferDescriptor* d = [MTLCommandBufferDescriptor new];
        d.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
        id<MTLCommandBuffer> buffer = [queue_ commandBufferWithDescriptor:d];
        buffer.label = ns(name);
        return std::make_unique<MetalCommandList>(*this, buffer);
    }

    SubmissionId submit(std::unique_ptr<CommandList> commands, SurfaceId present) override {
        auto* list = static_cast<MetalCommandList*>(commands.get());
        list->end_encoder();
        id<MTLCommandBuffer> buffer = list->buffer();
        if (MetalSurface* s = surface(present); s && s->drawable) {
            [buffer presentDrawable:s->drawable];
            s->drawable = nil;
        }
        const SubmissionId submission = tracker().next_submission();
        const std::string label = buffer.label ? buffer.label.UTF8String : "commands";
        std::shared_ptr<CompletionState> state = state_;
        [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
          if (done.status == MTLCommandBufferStatusError) {
              log::error("rhi", Error{ErrorCode::rhi_gpu_error,
                                      std::format("GPU error in '{}': {}", label, describe(done.error)),
                                      "Run a debug build (Metal validation on) for details."});
          }
          state->last_gpu_ms.store((done.GPUEndTime - done.GPUStartTime) * 1000.0);
          state->tracker.mark_completed(submission);
        }];
        [buffer commit];
        in_flight_[submission.value] = buffer;
        prune();
        return submission;
    }

    void wait(SubmissionId id) override {
        if (id.value != 0 && !tracker().is_complete(id)) {
            if (const auto it = in_flight_.find(id.value); it != in_flight_.end()) {
                [it->second waitUntilCompleted];  // the queue is in order: everything before it is done too
            }
            tracker().mark_completed(id);  // the completion handler may not have run yet
        }
        prune();
        tracker().collect();
    }

    void wait_idle() override { wait(tracker().last_submitted()); }

    FrameInfo begin_frame() override {
        SubmissionId wait_for;
        const FrameInfo frame = tracker().begin_frame(wait_for);
        if (wait_for.value != 0) {
            wait(wait_for);
        } else {
            prune();
            tracker().collect();
        }
        collect_timings();  // this slot's previous frame has completed: resolve it before reuse
        timings_.begin_frame(frame.slot, frame.index);
        return frame;
    }

    void end_frame() override {
        tracker().end_frame();
        timings_.end_frame(tracker().current_frame().slot, tracker().last_submitted().value);
    }
    [[nodiscard]] FrameInfo current_frame() const override { return state_->tracker.current_frame(); }
    [[nodiscard]] std::uint32_t frames_in_flight() const override { return state_->tracker.frames_in_flight(); }
    [[nodiscard]] double last_frame_gpu_ms() const override { return state_->last_gpu_ms.load(); }

    void prune() {
        const std::uint64_t done = tracker().completed().value;
        std::erase_if(in_flight_, [done](const auto& entry) { return entry.first <= done; });
    }

    // Used by MetalCommandList.
    MetalBuffer* buffer(BufferHandle h) { return buffers_.get(h); }
    MetalTexture* texture(TextureHandle h) { return textures_.get(h); }
    MetalPipeline* pipeline(PipelineHandle h) { return pipelines_.get(h); }
    MetalAccel* accel(AccelHandle h) { return accels_.get(h); }
    id<MTLResource> resource(ResourceRef r) {
        switch (r.kind) {
            case ResourceKind::buffer:
                if (MetalBuffer* b = buffers_.get(BufferHandle{r.index, r.generation})) {
                    return b->buffer;
                }
                break;
            case ResourceKind::texture:
                if (MetalTexture* t = textures_.get(TextureHandle{r.index, r.generation})) {
                    return t->texture;
                }
                break;
            case ResourceKind::acceleration_structure:
                if (MetalAccel* a = accels_.get(AccelHandle{r.index, r.generation})) {
                    return a->accel;
                }
                break;
            default: break;
        }
        return nil;
    }
    [[nodiscard]] bool validation() const { return validation_; }

private:
    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    bool validation_ = false;
    id<MTLTexture> null_texture_;  ///< 1x1, zero; read and read-write
    id<MTLBuffer> null_buffer_;    ///< 16 zero bytes
    id<MTLSamplerState> null_sampler_;
    Capabilities caps_;
    SlotMap<MetalBuffer, BufferHandle> buffers_;
    SlotMap<MetalTexture, TextureHandle> textures_;
    SlotMap<id<MTLSamplerState>, SamplerHandle> samplers_;
    SlotMap<MetalPipeline, PipelineHandle> pipelines_;
    SlotMap<MetalAccel, AccelHandle> accels_;
    std::unordered_map<const ShaderLibrary*, id<MTLLibrary>> libraries_;
    std::vector<MetalSurface> surfaces_;
    std::shared_ptr<CompletionState> state_;
    std::map<std::uint64_t, id<MTLCommandBuffer>> in_flight_;
    std::vector<std::vector<id<MTLCounterSampleBuffer>>> timing_buffers_;  ///< [slot][i]; empty when unsupported
    FrameTimingSlots timings_{state_->tracker.frames_in_flight(), timing_samples_per_frame};
    std::string timing_unavailable_;
    MTLTimestamp calibration_cpu_ = 0;
    MTLTimestamp calibration_gpu_ = 0;
};

// ---- MetalCommandList -----------------------------------------------------------

void MetalCommandList::end_encoder() {
    if (render_) {
        [render_ endEncoding];
        render_ = nil;
    }
    if (compute_) {
        [compute_ endEncoding];
        compute_ = nil;
    }
}

void MetalCommandList::begin_render_pass(const RenderPassDesc& desc) {
    end_encoder();
    MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
    for (std::uint32_t i = 0; i < desc.color_count; ++i) {
        const ColorAttachment& c = desc.colors.at(i);
        MetalTexture* t = device_.texture(c.texture);
        VEACH_ASSERT(t != nullptr, "render pass '{}': invalid color attachment {}", desc.debug_name, i);
        d.colorAttachments[i].texture = t->texture;
        d.colorAttachments[i].loadAction = to_mtl(c.load);
        d.colorAttachments[i].storeAction = MTLStoreActionStore;
        // Integer targets take the clear value as integers; doubles hold every 32-bit value exactly.
        d.colorAttachments[i].clearColor =
            is_integer(t->desc.format)
                ? MTLClearColorMake(c.clear_uint[0], c.clear_uint[1], c.clear_uint[2], c.clear_uint[3])
                : MTLClearColorMake(c.clear_color[0], c.clear_color[1], c.clear_color[2], c.clear_color[3]);
    }
    if (desc.depth.texture.is_valid()) {
        MetalTexture* t = device_.texture(desc.depth.texture);
        VEACH_ASSERT(t != nullptr, "render pass '{}': invalid depth attachment", desc.debug_name);
        d.depthAttachment.texture = t->texture;
        d.depthAttachment.loadAction = to_mtl(desc.depth.load);
        d.depthAttachment.storeAction = MTLStoreActionStore;
        d.depthAttachment.clearDepth = desc.depth.clear_depth;
    }
    if (const Samples samples = next_samples(); samples.buffer) {
        d.sampleBufferAttachments[0].sampleBuffer = samples.buffer;
        d.sampleBufferAttachments[0].startOfVertexSampleIndex = samples.start;
        d.sampleBufferAttachments[0].endOfVertexSampleIndex = MTLCounterDontSample;
        d.sampleBufferAttachments[0].startOfFragmentSampleIndex = MTLCounterDontSample;
        d.sampleBufferAttachments[0].endOfFragmentSampleIndex = samples.end;
    }
    render_ = [buffer_ renderCommandEncoderWithDescriptor:d];
    render_.label = ns(desc.debug_name);
    device_.use_null_resources(render_);
}

void MetalCommandList::begin_compute_pass(std::string_view name) {
    end_encoder();
    MTLComputePassDescriptor* d = [MTLComputePassDescriptor computePassDescriptor];
    if (const Samples samples = next_samples(); samples.buffer) {
        d.sampleBufferAttachments[0].sampleBuffer = samples.buffer;
        d.sampleBufferAttachments[0].startOfEncoderSampleIndex = samples.start;
        d.sampleBufferAttachments[0].endOfEncoderSampleIndex = samples.end;
    }
    compute_ = [buffer_ computeCommandEncoderWithDescriptor:d];
    compute_.label = ns(name);
    device_.use_null_resources(compute_);
}

void MetalCommandList::set_pipeline(PipelineHandle handle) {
    MetalPipeline* p = device_.pipeline(handle);
    VEACH_ASSERT(p != nullptr, "set_pipeline: invalid pipeline");
    if (render_ && p->render) {
        [render_ setRenderPipelineState:p->render];
        [render_ setDepthStencilState:p->depth];
        [render_ setCullMode:p->cull];
        [render_ setFrontFacingWinding:MTLWindingCounterClockwise];  // glTF
    } else if (compute_ && p->compute) {
        [compute_ setComputePipelineState:p->compute];
        threads_per_group_ = p->threads_per_group;
    } else {
        VEACH_PANIC("set_pipeline: pipeline kind does not match the current pass");
    }
}

void MetalCommandList::set_parameters(const void* data, std::size_t size) {
    VEACH_ASSERT(size <= max_parameter_bytes, "parameter block of {} bytes exceeds {}", size, max_parameter_bytes);
    if (render_) {
        [render_ setVertexBytes:data length:size atIndex:0];
        [render_ setFragmentBytes:data length:size atIndex:0];
    } else if (compute_) {
        [compute_ setBytes:data length:size atIndex:0];
    }
}

void MetalCommandList::draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex) {
    VEACH_ASSERT(render_ != nil, "draw() outside a render pass");
    [render_ drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:first_vertex
                vertexCount:vertex_count
              instanceCount:instance_count];
}

void MetalCommandList::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    VEACH_ASSERT(compute_ != nil, "dispatch() outside a compute pass");
    [compute_ dispatchThreadgroups:MTLSizeMake(x, y, z) threadsPerThreadgroup:threads_per_group_];
}

void MetalCommandList::encode_accel(AccelHandle handle, bool refit) {
    end_encoder();
    MetalAccel* a = device_.accel(handle);
    VEACH_ASSERT(a != nullptr, "invalid acceleration structure");
    MTLAccelerationStructurePassDescriptor* d =
        [MTLAccelerationStructurePassDescriptor accelerationStructurePassDescriptor];
    if (const Samples samples = next_samples(); samples.buffer) {
        d.sampleBufferAttachments[0].sampleBuffer = samples.buffer;
        d.sampleBufferAttachments[0].startOfEncoderSampleIndex = samples.start;
        d.sampleBufferAttachments[0].endOfEncoderSampleIndex = samples.end;
    }
    id<MTLAccelerationStructureCommandEncoder> encoder = [buffer_ accelerationStructureCommandEncoderWithDescriptor:d];
    if (refit) {
        [encoder refitAccelerationStructure:a->accel
                                 descriptor:a->descriptor
                                destination:nil
                              scratchBuffer:a->scratch
                        scratchBufferOffset:0];
    } else {
        [encoder buildAccelerationStructure:a->accel
                                 descriptor:a->descriptor
                              scratchBuffer:a->scratch
                        scratchBufferOffset:0];
    }
    [encoder endEncoding];
}

void MetalCommandList::build_blas(AccelHandle blas) {
    encode_accel(blas, false);
}

void MetalCommandList::refit_blas(AccelHandle blas) {
    encode_accel(blas, true);
}

void MetalCommandList::write_instances(MetalAccel& tlas, std::span<const TlasInstance> instances) {
    VEACH_ASSERT(instances.size() <= tlas.max_instances, "{} instances exceed the TLAS capacity of {}",
                 instances.size(), tlas.max_instances);
    id<MTLBuffer> buffer = tlas.instances.at(device_.tracker().current_frame().slot % tlas.instances.size());
    auto* out = static_cast<MTLAccelerationStructureInstanceDescriptor*>(buffer.contents);
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const TlasInstance& in = instances[i];
        MTLAccelerationStructureInstanceDescriptor& d = out[i];
        for (int column = 0; column < 4; ++column) {
            d.transformationMatrix.columns[column] = MTLPackedFloat3Make(
                in.transform[0][static_cast<std::size_t>(column)], in.transform[1][static_cast<std::size_t>(column)],
                in.transform[2][static_cast<std::size_t>(column)]);
        }
        d.options =
            in.opaque ? MTLAccelerationStructureInstanceOptionOpaque : MTLAccelerationStructureInstanceOptionNonOpaque;
        d.mask = in.mask;
        d.intersectionFunctionTableOffset = 0;
        d.accelerationStructureIndex = in.blas_index;
    }
    auto* descriptor = static_cast<MTLInstanceAccelerationStructureDescriptor*>(tlas.descriptor);
    descriptor.instanceDescriptorBuffer = buffer;
    descriptor.instanceCount = instances.size();
}

void MetalCommandList::build_tlas(AccelHandle tlas, std::span<const TlasInstance> instances) {
    MetalAccel* a = device_.accel(tlas);
    VEACH_ASSERT(a != nullptr && a->tlas, "build_tlas: not a TLAS");
    write_instances(*a, instances);
    encode_accel(tlas, false);
}

void MetalCommandList::refit_tlas(AccelHandle tlas, std::span<const TlasInstance> instances) {
    MetalAccel* a = device_.accel(tlas);
    VEACH_ASSERT(a != nullptr && a->tlas, "refit_tlas: not a TLAS");
    write_instances(*a, instances);
    encode_accel(tlas, true);
}

MetalCommandList::Samples MetalCommandList::next_samples() {
    if (!timing_) {
        return {};
    }
    if (!device_.timing_supported()) {
        return {};
    }
    const std::optional<std::uint32_t> first = device_.timing_slots().allocate_pair(timing_slot_);
    if (!first) {
        return {};
    }
    const NSUInteger local = *first % timing_samples_per_buffer;
    return {device_.timing_buffer(timing_slot_, *first), local, local + 1};
}

id<MTLBlitCommandEncoder> MetalCommandList::blit_encoder() {
    MTLBlitPassDescriptor* d = [MTLBlitPassDescriptor blitPassDescriptor];
    if (const Samples samples = next_samples(); samples.buffer) {
        d.sampleBufferAttachments[0].sampleBuffer = samples.buffer;
        d.sampleBufferAttachments[0].startOfEncoderSampleIndex = samples.start;
        d.sampleBufferAttachments[0].endOfEncoderSampleIndex = samples.end;
    }
    return [buffer_ blitCommandEncoderWithDescriptor:d];
}

void MetalCommandList::begin_timing(std::string_view name) {
    VEACH_ASSERT(!timing_, "begin_timing('{}') inside another timing region", name);
    timing_slot_ = device_.tracker().current_frame().slot;
    device_.timing_slots().begin_region(timing_slot_, name);
    timing_ = true;
}

void MetalCommandList::end_timing() {
    VEACH_ASSERT(timing_, "end_timing() without begin_timing()");
    end_encoder();  // the region's last encoder samples its end when it ends
    device_.timing_slots().end_region(timing_slot_);
    timing_ = false;
}

void MetalCommandList::copy_buffer(BufferHandle src, BufferHandle dst, std::uint64_t size) {
    end_encoder();
    MetalBuffer* from = device_.buffer(src);
    MetalBuffer* to = device_.buffer(dst);
    VEACH_ASSERT(from != nullptr && to != nullptr, "copy_buffer: invalid handles");
    VEACH_ASSERT(size <= from->desc.size && size <= to->desc.size, "copy_buffer: {} bytes do not fit", size);
    id<MTLBlitCommandEncoder> blit = blit_encoder();
    [blit copyFromBuffer:from->buffer sourceOffset:0 toBuffer:to->buffer destinationOffset:0 size:size];
    [blit endEncoding];
}

void MetalCommandList::copy_texture_to_buffer(TextureHandle src, BufferHandle dst) {
    end_encoder();
    MetalTexture* t = device_.texture(src);
    MetalBuffer* b = device_.buffer(dst);
    VEACH_ASSERT(t != nullptr && b != nullptr, "copy_texture_to_buffer: invalid handles");
    const auto row = static_cast<std::uint32_t>(row_bytes(t->desc.format, t->desc.width));
    const std::uint32_t rows = row_count(t->desc.format, t->desc.height);
    VEACH_ASSERT(b->desc.size >= std::uint64_t{row} * rows, "copy_texture_to_buffer: buffer too small");
    id<MTLBlitCommandEncoder> blit = blit_encoder();
    [blit copyFromTexture:t->texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(t->desc.width, t->desc.height, 1)
                        toBuffer:b->buffer
               destinationOffset:0
          destinationBytesPerRow:row
        destinationBytesPerImage:std::uint64_t{row} * rows];
    [blit endEncoding];
}

void MetalCommandList::use_one(id<MTLResource> resource, ResourceState state) {
    if (!resource) {
        return;
    }
    if (render_) {
        [render_ useResource:resource usage:usage_for(state) stages:MTLRenderStageVertex | MTLRenderStageFragment];
    } else if (compute_) {
        [compute_ useResource:resource usage:usage_for(state)];
    }
}

void MetalCommandList::use(ResourceRef ref, ResourceState state) {
    if (state == ResourceState::color_target || state == ResourceState::depth_write ||
        state == ResourceState::present || state == ResourceState::copy_src || state == ResourceState::copy_dst ||
        state == ResourceState::accel_build) {
        return;  // attachments, blits and builds name their resources explicitly
    }
    use_one(device_.resource(ref), state);
    if (ref.kind == ResourceKind::acceleration_structure) {
        // A TLAS is only traversable if every BLAS it references is resident too.
        if (MetalAccel* tlas = device_.accel(AccelHandle{ref.index, ref.generation}); tlas && tlas->tlas) {
            for (const AccelHandle blas : tlas->blases) {
                use_one(device_.resource(blas), ResourceState::accel_read);
            }
        }
    }
}

void MetalCommandList::barrier(std::span<const Barrier> barriers) {
    // Metal tracks hazards itself (tracked resources, one encoder per pass). In
    // debug builds we still check that the graph's transitions are consistent,
    // so the barrier list a DX12 backend would execute is known to be right.
    for (const Barrier& b : barriers) {
        if (device_.validation()) {
            if (const auto it = states_.find(b.resource);
                it != states_.end() && b.before != ResourceState::undefined && it->second != b.before) {
                log::error("rhi", "barrier mismatch: resource {}:{} is in {} but the barrier says {}", b.resource.index,
                           b.resource.generation, to_string(it->second), to_string(b.before));
            }
        }
        states_[b.resource] = b.after;
    }
}

}  // namespace

Result<std::unique_ptr<Device>> create_metal_device(const DeviceDesc& desc) {
    if (desc.validation) {
        // Read when Metal creates the device; must be set before MTLCreateSystemDefaultDevice.
        setenv("MTL_DEBUG_LAYER", "1", 0);
        setenv("MTL_DEBUG_LAYER_ERROR_MODE", "assert", 0);
        // ADR 0039: Metal handles are opaque in shaders, so Metal's shader validation checks them instead (nil,
        // freed or non-resident resources, out-of-bounds buffers); a failure fails the command buffer (rhi.gpu_error).
        setenv("MTL_SHADER_VALIDATION", "1", 0);
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        return make_error(ErrorCode::rhi_backend_unavailable, "no Metal device is available",
                          "Use \"rhi\": \"null\" (the default) on machines without a GPU.");
    }
    if (![device supportsFamily:MTLGPUFamilyMetal3]) {
        return make_error(ErrorCode::rhi_backend_unavailable,
                          std::format("'{}' does not support Metal 3", device.name.UTF8String),
                          "Veach needs Metal 3 with tier-2 argument buffers (Apple M1 or newer); see ADR 0024. "
                          "Virtualized GPUs (e.g. GitHub's macOS runners) do not qualify.");
    }
    if (desc.validation) {
        log::debug("rhi", "Metal API validation enabled (MTL_DEBUG_LAYER)");
    }
    return std::unique_ptr<Device>(std::make_unique<MetalDevice>(device, desc.validation, desc.frames_in_flight));
}

}  // namespace veach::rhi
