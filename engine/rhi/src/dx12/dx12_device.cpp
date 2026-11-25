// DX12 backend (ADR 0009, VCH-012). D3D12 through the Agility SDK (ADR 0031): one
// direct queue and one fence whose values are SubmissionIds (ADR 0029). Built
// and verified step by step in hosted CI on WARP (docs/guides/dx12-port.md).

#include "dx12_device.hpp"

#include "../slot_map.hpp"
#include "../surface_validation.hpp"

#include <veach/core/assert.hpp>
#include <veach/core/log.hpp>

#include <windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

// The Agility SDK: D3D12 loads D3D12Core.dll (and the debug layer) from D3D12\ next to the executable, where
// veach_deploy_d3d12() in cmake/VeachTargets.cmake puts them, instead of the older copy in the OS.
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

namespace veach::rhi {

namespace {

using Microsoft::WRL::ComPtr;

std::string hresult(HRESULT hr) {
    return std::format("HRESULT 0x{:08X}", static_cast<std::uint32_t>(hr));
}

std::string narrow(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
    if (size > 1) {
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    }
    return out;
}

std::wstring widen(std::string_view text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

void set_name(ID3D12Object* object, std::string_view name) {
    if (!name.empty()) {
        object->SetName(widen(name).c_str());
    }
}

constexpr std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

DXGI_FORMAT to_dxgi(Format format) {
    switch (format) {
        case Format::rgba8_unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::bgra8_unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::rgba16_float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::rgba32_float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::r32_float: return DXGI_FORMAT_R32_FLOAT;
        case Format::rgb32_float: return DXGI_FORMAT_R32G32B32_FLOAT;
        case Format::depth32_float: return DXGI_FORMAT_D32_FLOAT;
        case Format::rgba8_srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::bc7_unorm: return DXGI_FORMAT_BC7_UNORM;
        case Format::bc7_srgb: return DXGI_FORMAT_BC7_UNORM_SRGB;
        case Format::undefined: return DXGI_FORMAT_UNKNOWN;
    }
    return DXGI_FORMAT_UNKNOWN;
}

D3D12_HEAP_TYPE heap_type(MemoryType memory) {
    switch (memory) {
        case MemoryType::upload: return D3D12_HEAP_TYPE_UPLOAD;
        case MemoryType::readback: return D3D12_HEAP_TYPE_READBACK;
        case MemoryType::gpu: return D3D12_HEAP_TYPE_DEFAULT;
    }
    return D3D12_HEAP_TYPE_DEFAULT;
}

D3D12_RESOURCE_DESC1 buffer_resource(std::uint64_t size, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC1 desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

std::unexpected<Error> gpu_error(std::string_view what, HRESULT hr) {
    return make_error(ErrorCode::rhi_gpu_error, std::format("D3D12: {} failed ({})", what, hresult(hr)));
}

/// Debug-layer messages become log records: errors and corruption at error level, so the exit code turns 1.
void __stdcall on_debug_message(D3D12_MESSAGE_CATEGORY /*category*/, D3D12_MESSAGE_SEVERITY severity,
                                D3D12_MESSAGE_ID id, LPCSTR description, void* /*context*/) {
    switch (severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        case D3D12_MESSAGE_SEVERITY_ERROR:
            log::error("rhi", Error{ErrorCode::rhi_gpu_error,
                                    std::format("D3D12 debug layer: {} (id {})", description, static_cast<int>(id)),
                                    "The D3D12 debug layer is on in debug builds; fix the call it names."});
            break;
        case D3D12_MESSAGE_SEVERITY_WARNING:
            log::warn("rhi", "D3D12 debug layer: {} (id {})", description, static_cast<int>(id));
            break;
        default: log::debug("rhi", "D3D12: {}", description); break;
    }
}

/// What the device must support (ADR 0024/0025/0031). Returns what is missing, if anything.
std::optional<std::string> missing_feature_floor(ID3D12Device* device, Capabilities& caps) {
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model{D3D_SHADER_MODEL_6_6};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model))) ||
        shader_model.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
        return "Shader Model 6.6 (ResourceDescriptorHeap)";
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) ||
        options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) {
        return "resource binding tier 3";
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))) ||
        !options12.EnhancedBarriersSupported) {
        return "enhanced barriers";
    }
    caps.bindless = true;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    caps.hardware_ray_tracing =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))) &&
        options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    caps.mesh_shaders =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))) &&
        options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1;
    return std::nullopt;
}

/// One command allocator and the list recorded from it. Reusable once its last submission completed.
struct Recorder {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList7> list;
    std::uint64_t last_use = 0;
};

/// A descriptor heap with a free list. Index 0 is never handed out, so a zero DescriptorHandle means "none"
/// on DX12 as it does on Metal. Slots are released through SubmissionTracker::retire() only (ADR 0029).
class DescriptorHeap {
public:
    HRESULT init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint32_t capacity, bool shader_visible) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = type;
        desc.NumDescriptors = capacity;
        desc.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_)); FAILED(hr)) {
            return hr;
        }
        base_ = heap_->GetCPUDescriptorHandleForHeapStart();
        increment_ = device->GetDescriptorHandleIncrementSize(type);
        capacity_ = capacity;
        return S_OK;
    }

    /// A free slot, or 0 when the heap is full.
    [[nodiscard]] std::uint32_t allocate() {
        if (!free_.empty()) {
            const std::uint32_t index = free_.back();
            free_.pop_back();
            return index;
        }
        return next_ < capacity_ ? next_++ : 0;
    }
    void release(std::uint32_t index) {
        if (index != 0) {
            free_.push_back(index);
        }
    }

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpu(std::uint32_t index) const {
        return {base_.ptr + std::size_t{index} * increment_};
    }
    [[nodiscard]] ID3D12DescriptorHeap* get() const { return heap_.Get(); }
    [[nodiscard]] std::uint32_t capacity() const { return capacity_; }

private:
    ComPtr<ID3D12DescriptorHeap> heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE base_{};
    UINT increment_ = 0;
    std::uint32_t capacity_ = 0;
    std::uint32_t next_ = 1;
    std::vector<std::uint32_t> free_;
};

/// Shader-visible heap sizes. 65,536 CBV/SRV/UAV slots fit every tier-3 device; 2,048 is the sampler maximum.
constexpr std::uint32_t view_heap_capacity = 65'536;
constexpr std::uint32_t sampler_heap_capacity = 2'048;
constexpr std::uint32_t rtv_heap_capacity = 1'024;

/// Rows a texture copy wrote with D3D12's 256-byte pitch, to be packed tightly on map().
struct PitchedReadback {
    ComPtr<ID3D12Resource> resource;  ///< readback heap, persistently mapped
    void* mapped = nullptr;
    std::uint64_t size = 0;
    std::uint32_t row_bytes = 0;
    std::uint32_t pitch = 0;
    std::uint32_t rows = 0;
    bool pending = false;
};

struct Dx12Buffer {
    ComPtr<ID3D12Resource> resource;
    BufferDesc desc;
    void* mapped = nullptr;  ///< upload and readback heaps: persistently mapped
    std::uint32_t srv = 0;
    std::uint32_t uav = 0;
    std::shared_ptr<PitchedReadback> pitched;  ///< readback buffers whose copies needed a row pitch
};

/// ADR 0025's mapping of graph states to enhanced-barrier layout, access and sync.
struct BarrierState {
    D3D12_BARRIER_LAYOUT layout;
    D3D12_BARRIER_ACCESS access;
    D3D12_BARRIER_SYNC sync;
};

BarrierState barrier_state(ResourceState state) {
    switch (state) {
        case ResourceState::undefined:
            return {D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_SYNC_NONE};
        case ResourceState::shader_read:
            return {D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_ALL_SHADING};
        case ResourceState::shader_write:
            return {D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_ALL_SHADING};
        case ResourceState::color_target:
            return {D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                    D3D12_BARRIER_SYNC_RENDER_TARGET};
        case ResourceState::depth_write:
            return {D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                    D3D12_BARRIER_SYNC_DEPTH_STENCIL};
        case ResourceState::copy_src:
            return {D3D12_BARRIER_LAYOUT_COPY_SOURCE, D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_SYNC_COPY};
        case ResourceState::copy_dst:
            return {D3D12_BARRIER_LAYOUT_COPY_DEST, D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_SYNC_COPY};
        case ResourceState::accel_build:
            return {D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
                    D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE};
        case ResourceState::accel_read:
            return {D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,
                    D3D12_BARRIER_SYNC_ALL_SHADING};
        case ResourceState::present:
            return {D3D12_BARRIER_LAYOUT_PRESENT, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_SYNC_NONE};
    }
    return {D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_SYNC_NONE};
}

struct Dx12Texture {
    ComPtr<ID3D12Resource> resource;
    TextureDesc desc;
    std::uint32_t srv = 0;
    std::uint32_t uav = 0;
    std::uint32_t rtv = 0;  ///< in the CPU-only RTV heap
};

struct Dx12Sampler {
    std::uint32_t index = 0;
};

struct Dx12Pipeline {
    ComPtr<ID3D12PipelineState> state;
    bool compute = false;
};

struct Dx12Accel {
    ComPtr<ID3D12Resource> buffer;  ///< RAYTRACING_ACCELERATION_STRUCTURE, layout UNDEFINED forever
    ComPtr<ID3D12Resource> scratch;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};  ///< BLAS; inputs.pGeometryDescs is set at build time
    /// TLAS only: D3D12_RAYTRACING_INSTANCE_DESC[max], one upload buffer per frame slot because the CPU writes
    /// them while earlier frames may still be building from theirs (ADR 0029).
    std::vector<ComPtr<ID3D12Resource>> instances;
    std::vector<void*> instances_mapped;
    std::vector<AccelHandle> blases;
    std::uint32_t max_instances = 0;
    std::uint32_t srv = 0;  ///< TLAS: the acceleration-structure SRV shaders trace against
    bool tlas = false;
};

/// A flip-model swapchain; its buffers are ordinary textures (state: present) for the render graph.
struct Dx12Surface {
    ComPtr<IDXGISwapChain3> swapchain;
    HWND hwnd = nullptr;
    std::vector<TextureHandle> images;
    bool vsync = true;
};

/// Why a Present failed: the window's state and, where the DXGI debug layer is installed, its messages.
std::string present_diagnostics(const Dx12Surface& s) {
    DXGI_SWAP_CHAIN_DESC1 d{};
    s.swapchain->GetDesc1(&d);
    RECT client{};
    GetClientRect(s.hwnd, &client);
    std::string out =
        std::format("swapchain {}x{}, window client {}x{}, visible {}, minimized {}, back buffer {}", d.Width, d.Height,
                    client.right - client.left, client.bottom - client.top, IsWindowVisible(s.hwnd) != 0,
                    IsIconic(s.hwnd) != 0, s.swapchain->GetCurrentBackBufferIndex());
    ComPtr<IDXGIInfoQueue> queue;
    if (FAILED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&queue)))) {
        return out + "; the DXGI debug layer is not installed";
    }
    const UINT64 count = queue->GetNumStoredMessages(DXGI_DEBUG_DXGI);
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        queue->GetMessage(DXGI_DEBUG_DXGI, i, nullptr, &size);
        std::vector<std::byte> storage(size);
        auto* message = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(storage.data());
        if (SUCCEEDED(queue->GetMessage(DXGI_DEBUG_DXGI, i, message, &size))) {
            out += std::format("; DXGI: {}", message->pDescription);
        }
    }
    queue->ClearStoredMessages(DXGI_DEBUG_DXGI);
    return out;
}

class Dx12Device;

class Dx12CommandList final : public CommandList {
public:
    Dx12CommandList(Dx12Device& device, std::unique_ptr<Recorder> recorder)
        : device_(device), recorder_(std::move(recorder)) {}
    ~Dx12CommandList() override;
    Dx12CommandList(const Dx12CommandList&) = delete;
    Dx12CommandList& operator=(const Dx12CommandList&) = delete;
    Dx12CommandList(Dx12CommandList&&) = delete;
    Dx12CommandList& operator=(Dx12CommandList&&) = delete;

    void begin_render_pass(const RenderPassDesc& desc) override;
    void end_render_pass() override { pop_debug_group(); }
    void begin_compute_pass(std::string_view name) override {
        push_debug_group(name);
        compute_pass_ = true;
    }
    void end_compute_pass() override { pop_debug_group(); }
    void set_pipeline(PipelineHandle pipeline) override;
    void set_parameters(const void* data, std::size_t size) override;
    void draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex) override {
        recorder_->list->DrawInstanced(vertex_count, instance_count, first_vertex, 0);
    }
    void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) override { recorder_->list->Dispatch(x, y, z); }
    void build_blas(AccelHandle blas) override { encode_accel(blas, false, {}); }
    void refit_blas(AccelHandle blas) override { encode_accel(blas, true, {}); }
    void build_tlas(AccelHandle tlas, std::span<const TlasInstance> instances) override {
        encode_accel(tlas, false, instances);
    }
    void refit_tlas(AccelHandle tlas, std::span<const TlasInstance> instances) override {
        encode_accel(tlas, true, instances);
    }
    void copy_texture_to_buffer(TextureHandle src, BufferHandle dst) override;
    void use(ResourceRef, ResourceState) override {}  // committed resources are always resident
    void barrier(std::span<const Barrier> barriers) override;
    void push_debug_group(std::string_view name) override {
        const std::string text(name);  // PIX_EVENT_ANSI_VERSION (1): a null-terminated ANSI string
        recorder_->list->BeginEvent(1, text.c_str(), static_cast<UINT>(text.size() + 1));
    }
    void pop_debug_group() override { recorder_->list->EndEvent(); }

    /// Closes the list and hands its recorder back; called once, by Dx12Device::submit.
    std::unique_ptr<Recorder> finish() {
        if (const HRESULT hr = recorder_->list->Close(); FAILED(hr)) {
            log::error("rhi", gpu_error("closing a command list (an invalid command was recorded)", hr).error());
        }
        return std::move(recorder_);
    }

private:
    void encode_accel(AccelHandle handle, bool refit, std::span<const TlasInstance> instances);

    Dx12Device& device_;
    std::unique_ptr<Recorder> recorder_;
    bool compute_pass_ = false;  ///< root constants go to the graphics or the compute slot
};

class Dx12Device final : public Device {
public:
    Dx12Device(ComPtr<IDXGIFactory6> factory, ComPtr<ID3D12Device10> device, Capabilities caps, bool validation,
               std::uint32_t frames_in_flight)
        : factory_(std::move(factory)), device_(std::move(device)), caps_(std::move(caps)), tracker_(frames_in_flight) {
        if (validation && SUCCEEDED(device_.As(&info_queue_))) {
            // Clearing with a color other than the resource's optimized one is only a hint; targets have none.
            std::array<D3D12_MESSAGE_ID, 1> deny{D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE};
            D3D12_INFO_QUEUE_FILTER filter{};
            filter.DenyList.NumIDs = static_cast<UINT>(deny.size());
            filter.DenyList.pIDList = deny.data();
            info_queue_->AddStorageFilterEntries(&filter);
            info_queue_->RegisterMessageCallback(&on_debug_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr,
                                                 &callback_cookie_);
        }
    }

    ~Dx12Device() override {
        wait_idle();
        if (info_queue_ && callback_cookie_ != 0) {
            info_queue_->UnregisterMessageCallback(callback_cookie_);
        }
        if (event_ != nullptr) {
            CloseHandle(event_);
        }
    }
    Dx12Device(const Dx12Device&) = delete;
    Dx12Device& operator=(const Dx12Device&) = delete;
    Dx12Device(Dx12Device&&) = delete;
    Dx12Device& operator=(Dx12Device&&) = delete;

    Result<> init() {
        D3D12_COMMAND_QUEUE_DESC queue{};
        queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (const HRESULT hr = device_->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue_)); FAILED(hr)) {
            return gpu_error("CreateCommandQueue", hr);
        }
        if (const HRESULT hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)); FAILED(hr)) {
            return gpu_error("CreateFence", hr);
        }
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event_ == nullptr) {
            return make_error(ErrorCode::rhi_gpu_error, "CreateEvent failed for the D3D12 fence");
        }
        if (const HRESULT hr =
                view_heap_.init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, view_heap_capacity, true);
            FAILED(hr)) {
            return gpu_error("creating the CBV/SRV/UAV heap", hr);
        }
        if (const HRESULT hr =
                sampler_heap_.init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, sampler_heap_capacity, true);
            FAILED(hr)) {
            return gpu_error("creating the sampler heap", hr);
        }
        if (const HRESULT hr = rtv_heap_.init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, rtv_heap_capacity, false);
            FAILED(hr)) {
            return gpu_error("creating the RTV heap", hr);
        }
        return create_root_signature();
    }

    [[nodiscard]] Backend backend() const override { return Backend::dx12; }
    [[nodiscard]] const Capabilities& capabilities() const override { return caps_; }

    // ---- resources

    [[nodiscard]] Result<BufferHandle> create_buffer(const BufferDesc& desc,
                                                     std::span<const std::byte> initial) override {
        const std::uint64_t size = align_up(std::max<std::uint64_t>({desc.size, 16, desc.stride}), 4);
        const bool gpu_memory = desc.memory == MemoryType::gpu;
        const bool storage = has(desc.usage, BufferUsage::storage);
        // Upload and readback heaps can't have UAVs.
        const D3D12_RESOURCE_FLAGS flags =
            gpu_memory && storage ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        auto resource = create_resource(heap_type(desc.memory), buffer_resource(size, flags),
                                        D3D12_BARRIER_LAYOUT_UNDEFINED, desc.debug_name);
        if (!resource) {
            return std::unexpected(resource.error());
        }
        Dx12Buffer buffer;
        buffer.resource = std::move(*resource);
        buffer.desc = desc;
        if (!gpu_memory) {
            const D3D12_RANGE nothing_read{0, 0};
            if (const HRESULT hr = buffer.resource->Map(0, desc.memory == MemoryType::upload ? &nothing_read : nullptr,
                                                        &buffer.mapped);
                FAILED(hr)) {
                return gpu_error(std::format("mapping '{}'", desc.debug_name), hr);
            }
        }
        if (storage && desc.memory != MemoryType::readback) {  // readback heaps are copy destinations only
            buffer.srv = view_heap_.allocate();
            buffer.uav = gpu_memory ? view_heap_.allocate() : 0;
            if (buffer.srv == 0 || (gpu_memory && buffer.uav == 0)) {
                view_heap_.release(buffer.srv);
                view_heap_.release(buffer.uav);
                return heap_full("CBV/SRV/UAV", view_heap_.capacity());
            }
            create_buffer_views(buffer, size);
        }
        if (!initial.empty()) {
            const auto bytes = initial.first(std::min<std::size_t>(initial.size(), desc.size));
            if (buffer.mapped != nullptr) {
                std::memcpy(buffer.mapped, bytes.data(), bytes.size());
            } else if (auto uploaded = upload(buffer.resource.Get(), bytes); !uploaded) {
                free_descriptors(buffer);
                return std::unexpected(uploaded.error());
            }
        }
        return buffers_.insert(std::move(buffer));
    }

    [[nodiscard]] Result<TextureHandle> create_texture(const TextureDesc& desc,
                                                       std::span<const std::span<const std::byte>> mips) override {
        const std::uint32_t levels = std::max(1u, desc.mip_levels);
        if (!mips.empty() && mips.size() != levels) {
            return make_error(ErrorCode::internal, std::format("create_texture '{}': {} mip levels, data for {}",
                                                               desc.debug_name, levels, mips.size()));
        }
        const DXGI_FORMAT format = to_dxgi(desc.format);
        if (format == DXGI_FORMAT_UNKNOWN || desc.format == Format::rgb32_float) {
            return make_error(ErrorCode::rhi_not_implemented,
                              std::format("'{}': {} is not a texture format", desc.debug_name, to_string(desc.format)));
        }
        const bool depth = desc.format == Format::depth32_float;
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = desc.width;
        d.Height = desc.height;
        d.DepthOrArraySize = 1;
        d.MipLevels = static_cast<UINT16>(levels);
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (has(desc.usage, TextureUsage::color_target)) {
            d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }
        if (has(desc.usage, TextureUsage::depth_target)) {
            d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        }
        if (has(desc.usage, TextureUsage::storage)) {
            d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        // New textures start in COMMON; the graph's first barrier says `undefined`, which is legal from any layout.
        auto resource = create_resource(D3D12_HEAP_TYPE_DEFAULT, d, D3D12_BARRIER_LAYOUT_COMMON, desc.debug_name);
        if (!resource) {
            return std::unexpected(resource.error());
        }
        Dx12Texture texture;
        texture.resource = std::move(*resource);
        texture.desc = desc;
        const bool sampled = has(desc.usage, TextureUsage::sampled);
        const bool storage = has(desc.usage, TextureUsage::storage);
        const bool target = has(desc.usage, TextureUsage::color_target);
        texture.srv = sampled ? view_heap_.allocate() : 0;
        texture.uav = storage ? view_heap_.allocate() : 0;
        texture.rtv = target ? rtv_heap_.allocate() : 0;
        if ((sampled && texture.srv == 0) || (storage && texture.uav == 0) || (target && texture.rtv == 0)) {
            free_descriptors(texture);
            return heap_full(target && texture.rtv == 0 ? "RTV" : "CBV/SRV/UAV",
                             target && texture.rtv == 0 ? rtv_heap_.capacity() : view_heap_.capacity());
        }
        if (sampled) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = depth ? DXGI_FORMAT_R32_FLOAT : format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = levels;
            device_->CreateShaderResourceView(texture.resource.Get(), &srv, view_heap_.cpu(texture.srv));
        }
        if (storage) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = format;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device_->CreateUnorderedAccessView(texture.resource.Get(), nullptr, &uav, view_heap_.cpu(texture.uav));
        }
        if (target) {
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = format;
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device_->CreateRenderTargetView(texture.resource.Get(), &rtv, rtv_heap_.cpu(texture.rtv));
        }
        if (!mips.empty()) {
            if (auto uploaded = upload_texture(texture.resource.Get(), d, desc, mips); !uploaded) {
                free_descriptors(texture);
                return std::unexpected(uploaded.error());
            }
        }
        return textures_.insert(std::move(texture));
    }

    [[nodiscard]] Result<SamplerHandle> create_sampler(const SamplerDesc& desc) override {
        const std::uint32_t index = sampler_heap_.allocate();
        if (index == 0) {
            return heap_full("sampler", sampler_heap_.capacity());
        }
        D3D12_SAMPLER_DESC d{};
        d.Filter = desc.filter == Filter::linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        const D3D12_TEXTURE_ADDRESS_MODE address =
            desc.address == AddressMode::repeat ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        d.AddressU = address;
        d.AddressV = address;
        d.AddressW = address;
        d.MaxAnisotropy = 1;
        d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        d.MaxLOD = D3D12_FLOAT32_MAX;
        device_->CreateSampler(&d, sampler_heap_.cpu(index));
        return samplers_.insert({index});
    }

    // ---- pipelines: DXIL from the embedded ShaderLibrary, one global root signature (ADR 0024)

    [[nodiscard]] Result<PipelineHandle> create_graphics_pipeline(const GraphicsPipelineDesc& desc) override {
        auto vs = dxil(desc.library, desc.vertex_entry);
        if (!vs) {
            return std::unexpected(vs.error());
        }
        auto fs = dxil(desc.library, desc.fragment_entry);
        if (!fs) {
            return std::unexpected(fs.error());
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root_signature_.Get();
        d.VS = {vs->data(), vs->size()};
        d.PS = {fs->data(), fs->size()};
        for (std::size_t i = 0; i < desc.color_formats.size() && desc.color_formats.at(i) != Format::undefined; ++i) {
            d.RTVFormats[i] = to_dxgi(desc.color_formats.at(i));
            D3D12_RENDER_TARGET_BLEND_DESC& blend = d.BlendState.RenderTarget[i];
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = D3D12_BLEND_ZERO;
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_ZERO;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            blend.LogicOp = D3D12_LOGIC_OP_NOOP;
            blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            ++d.NumRenderTargets;
        }
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;  // Metal's default: nothing is culled
        d.RasterizerState.DepthClipEnable = TRUE;
        if (desc.depth_format != Format::undefined) {
            d.DSVFormat = to_dxgi(desc.depth_format);
            d.DepthStencilState.DepthEnable = TRUE;
            d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reverse-Z (conventions.md)
        }
        const D3D12_DEPTH_STENCILOP_DESC keep{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                              D3D12_COMPARISON_FUNC_ALWAYS};
        d.DepthStencilState.FrontFace = keep;
        d.DepthStencilState.BackFace = keep;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.SampleDesc.Count = 1;
        Dx12Pipeline pipeline;
        if (const HRESULT hr = device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pipeline.state)); FAILED(hr)) {
            return gpu_error(std::format("graphics pipeline '{}'", desc.debug_name), hr);
        }
        set_name(pipeline.state.Get(), desc.debug_name);
        return pipelines_.insert(std::move(pipeline));
    }

    [[nodiscard]] Result<PipelineHandle> create_compute_pipeline(const ComputePipelineDesc& desc) override {
        auto cs = dxil(desc.library, desc.entry);
        if (!cs) {
            return std::unexpected(cs.error());
        }
        // threads_per_group is Metal's; D3D12 takes [numthreads] from the shader.
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root_signature_.Get();
        d.CS = {cs->data(), cs->size()};
        Dx12Pipeline pipeline;
        pipeline.compute = true;
        if (const HRESULT hr = device_->CreateComputePipelineState(&d, IID_PPV_ARGS(&pipeline.state)); FAILED(hr)) {
            return gpu_error(std::format("compute pipeline '{}'", desc.debug_name), hr);
        }
        set_name(pipeline.state.Get(), desc.debug_name);
        return pipelines_.insert(std::move(pipeline));
    }

    // ---- acceleration structures (DXR 1.1)

    [[nodiscard]] Result<AccelHandle> create_blas(const BlasDesc& desc) override {
        const Dx12Buffer* vertices = buffers_.get(desc.vertices);
        if (vertices == nullptr) {
            return make_error(ErrorCode::internal, "create_blas: invalid vertex buffer");
        }
        Dx12Accel accel;
        D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles = accel.geometry.Triangles;
        accel.geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        accel.geometry.Flags =
            desc.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        triangles.VertexBuffer.StartAddress = vertices->resource->GetGPUVirtualAddress();
        triangles.VertexBuffer.StrideInBytes = desc.vertex_stride;
        triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        triangles.VertexCount = desc.vertex_count;
        if (const Dx12Buffer* indices = buffers_.get(desc.indices)) {
            triangles.IndexBuffer = indices->resource->GetGPUVirtualAddress();
            triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            triangles.IndexCount = desc.index_count;
        }
        accel.inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        accel.inputs.NumDescs = 1;
        accel.inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        accel.inputs.pGeometryDescs = &accel.geometry;
        return make_accel(std::move(accel), desc.allow_refit, desc.debug_name);
    }

    [[nodiscard]] Result<AccelHandle> create_tlas(const TlasDesc& desc) override {
        Dx12Accel accel;
        for (const AccelHandle h : desc.blases) {
            const Dx12Accel* blas = accels_.get(h);
            if (blas == nullptr || blas->tlas) {
                return make_error(ErrorCode::internal, "create_tlas: invalid BLAS handle");
            }
            accel.blases.push_back(h);
        }
        accel.tlas = true;
        accel.max_instances = std::max(1u, desc.max_instances);
        const std::uint64_t instance_bytes = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * accel.max_instances;
        for (std::uint32_t slot = 0; slot < tracker_.frames_in_flight(); ++slot) {
            auto buffer =
                create_resource(D3D12_HEAP_TYPE_UPLOAD, buffer_resource(instance_bytes, D3D12_RESOURCE_FLAG_NONE),
                                D3D12_BARRIER_LAYOUT_UNDEFINED, desc.debug_name + " instances");
            if (!buffer) {
                return std::unexpected(buffer.error());
            }
            void* mapped = nullptr;
            const D3D12_RANGE nothing_read{0, 0};
            if (const HRESULT hr = (*buffer)->Map(0, &nothing_read, &mapped); FAILED(hr)) {
                return gpu_error("mapping TLAS instances", hr);
            }
            accel.instances.push_back(std::move(*buffer));
            accel.instances_mapped.push_back(mapped);
        }
        accel.inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        accel.inputs.NumDescs = accel.max_instances;
        accel.inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        return make_accel(std::move(accel), desc.allow_refit, desc.debug_name);
    }
    void destroy(BufferHandle h) override { retire(buffers_, h); }
    void destroy(TextureHandle h) override { retire(textures_, h); }
    void destroy(SamplerHandle h) override { retire(samplers_, h); }
    void destroy(PipelineHandle h) override { retire(pipelines_, h); }
    void destroy(AccelHandle h) override { retire(accels_, h); }

    [[nodiscard]] void* map(BufferHandle h) override {
        Dx12Buffer* b = buffers_.get(h);
        if (b == nullptr) {
            return nullptr;
        }
        if (PitchedReadback* p = b->pitched.get(); p != nullptr && p->pending) {
            // A texture copy landed with 256-byte rows; pack them the way copy_texture_to_buffer promises.
            const auto* from = static_cast<const std::byte*>(p->mapped);
            auto* to = static_cast<std::byte*>(b->mapped);
            for (std::uint32_t row = 0; row < p->rows; ++row) {
                std::memcpy(to + std::size_t{row} * p->row_bytes, from + std::size_t{row} * p->pitch, p->row_bytes);
            }
            p->pending = false;
        }
        return b->mapped;
    }
    [[nodiscard]] const TextureDesc* texture_desc(TextureHandle h) const override {
        const Dx12Texture* t = textures_.get(h);
        return t ? &t->desc : nullptr;
    }

    // ---- bindless descriptors (ADR 0024): heap indices

    [[nodiscard]] DescriptorHandle descriptor(BufferHandle h, View view) override {
        const Dx12Buffer* b = buffers_.get(h);
        return {b ? (view == View::storage ? b->uav : b->srv) : 0u};
    }
    [[nodiscard]] DescriptorHandle descriptor(TextureHandle h, View view) override {
        const Dx12Texture* t = textures_.get(h);
        return {t ? (view == View::storage ? t->uav : t->srv) : 0u};
    }
    [[nodiscard]] DescriptorHandle descriptor(SamplerHandle h) override {
        const Dx12Sampler* s = samplers_.get(h);
        return {s ? s->index : 0u};
    }
    [[nodiscard]] DescriptorHandle descriptor(AccelHandle h) override {
        const Dx12Accel* a = accels_.get(h);
        return {a ? a->srv : 0u};
    }
    // ---- surfaces: flip-model swapchains on the HWND

    [[nodiscard]] Result<SurfaceId> create_surface(const NativeWindowHandle& window, const SurfaceDesc& desc) override {
        if (auto valid = validate_surface_handle(window); !valid) {
            return std::unexpected(valid.error());
        }
        if (window.kind != NativeWindowHandle::Kind::win32) {
            return make_error(ErrorCode::rhi_invalid_surface, "the DX12 backend needs an HWND surface");
        }
        auto* hwnd = static_cast<HWND>(window.hwnd);
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = desc.width;
        d.Height = desc.height;
        d.Format = to_dxgi(surface_format);
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = tracker_.frames_in_flight() + 1;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        ComPtr<IDXGISwapChain1> swapchain;
        if (const HRESULT hr = factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &d, nullptr, nullptr, &swapchain);
            FAILED(hr)) {
            return make_error(ErrorCode::rhi_invalid_surface,
                              std::format("DXGI: creating the swapchain failed ({})", hresult(hr)));
        }
        factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);  // fullscreen is the Window component's job
        Dx12Surface surface;
        swapchain.As(&surface.swapchain);
        surface.hwnd = hwnd;
        surface.vsync = desc.vsync;
        if (auto images = wrap_images(surface); !images) {
            return std::unexpected(images.error());
        }
        surfaces_.push_back(std::move(surface));
        return SurfaceId{static_cast<std::uint32_t>(surfaces_.size())};
    }

    void resize_surface(SurfaceId id, std::uint32_t width, std::uint32_t height) override {
        Dx12Surface* s = surface(id);
        if (s == nullptr || !s->swapchain) {
            return;
        }
        wait_idle();  // ResizeBuffers needs every reference to the old buffers gone, the GPU's included
        release_images(*s);
        if (const HRESULT hr = s->swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0); FAILED(hr)) {
            log::error("rhi", Error{ErrorCode::rhi_invalid_surface,
                                    std::format("DXGI: resizing the swapchain to {}x{} failed ({})", width, height,
                                                hresult(hr)),
                                    {}});
            return;
        }
        if (auto images = wrap_images(*s); !images) {
            log::error("rhi", images.error());
        }
    }

    [[nodiscard]] Result<TextureHandle> acquire_image(SurfaceId id) override {
        Dx12Surface* s = surface(id);
        if (s == nullptr || !s->swapchain || s->images.empty()) {
            return make_error(ErrorCode::rhi_invalid_surface, "unknown surface");
        }
        // Flip model: the next image is known now; the queue waits for it to come back from the compositor.
        return s->images.at(s->swapchain->GetCurrentBackBufferIndex());
    }

    void destroy_surface(SurfaceId id) override {
        if (Dx12Surface* s = surface(id)) {
            wait_idle();
            release_images(*s);
            s->swapchain.Reset();
        }
    }

    // ---- work

    [[nodiscard]] std::unique_ptr<CommandList> begin_commands(std::string_view debug_name) override {
        std::unique_ptr<Recorder> recorder = take_recorder();
        if (!recorder) {
            return nullptr;
        }
        std::array<ID3D12DescriptorHeap*, 2> heaps{view_heap_.get(), sampler_heap_.get()};
        recorder->list->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
        // After SetDescriptorHeaps: directly indexed heaps require that order.
        recorder->list->SetGraphicsRootSignature(root_signature_.Get());
        recorder->list->SetComputeRootSignature(root_signature_.Get());
        auto list = std::make_unique<Dx12CommandList>(*this, std::move(recorder));
        list->push_debug_group(debug_name);
        return list;
    }

    SubmissionId submit(std::unique_ptr<CommandList> commands, SurfaceId present) override {
        auto* list = static_cast<Dx12CommandList*>(commands.get());
        list->pop_debug_group();
        const SubmissionId submission = execute(list->finish(), surface(present));
        poll();
        return submission;
    }

    void wait(SubmissionId submission) override {
        if (submission.value != 0 && fence_->GetCompletedValue() < submission.value) {
            fence_->SetEventOnCompletion(submission.value, event_);
            WaitForSingleObject(event_, INFINITE);
        }
        poll();
    }

    void wait_idle() override { wait(tracker_.last_submitted()); }

    FrameInfo begin_frame() override {
        SubmissionId wait_for;
        const FrameInfo frame = tracker_.begin_frame(wait_for);
        if (wait_for.value != 0) {
            wait(wait_for);
        } else {
            poll();
        }
        return frame;
    }

    void end_frame() override { tracker_.end_frame(); }
    [[nodiscard]] FrameInfo current_frame() const override { return tracker_.current_frame(); }
    [[nodiscard]] std::uint32_t frames_in_flight() const override { return tracker_.frames_in_flight(); }
    // TODO(VCH-049): timestamp queries.
    [[nodiscard]] double last_frame_gpu_ms() const override { return 0.0; }

    /// A list that was never submitted (an error path): nothing of it reached the GPU.
    void give_back(std::unique_ptr<Recorder> recorder) { recorders_.push_back(std::move(recorder)); }

    // Used by Dx12CommandList.
    Dx12Buffer* buffer(BufferHandle h) { return buffers_.get(h); }
    Dx12Texture* texture(TextureHandle h) { return textures_.get(h); }
    Dx12Pipeline* pipeline(PipelineHandle h) { return pipelines_.get(h); }
    Dx12Accel* accel(AccelHandle h) { return accels_.get(h); }
    [[nodiscard]] std::uint32_t frame_slot() const { return tracker_.current_frame().slot; }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE rtv(std::uint32_t index) const { return rtv_heap_.cpu(index); }
    ID3D12Resource* resource(ResourceRef r) {
        switch (r.kind) {
            case ResourceKind::buffer:
                if (Dx12Buffer* b = buffers_.get(BufferHandle{r.index, r.generation})) {
                    return b->resource.Get();
                }
                break;
            case ResourceKind::texture:
                if (Dx12Texture* t = textures_.get(TextureHandle{r.index, r.generation})) {
                    return t->resource.Get();
                }
                break;
            case ResourceKind::acceleration_structure:
                if (Dx12Accel* a = accels_.get(AccelHandle{r.index, r.generation})) {
                    return a->buffer.Get();
                }
                break;
            default: break;
        }
        return nullptr;
    }

    /// The pitched copy target for a readback buffer, grown if needed. A replaced one may still be the target of
    /// a copy in flight, so it is retired, not freed.
    PitchedReadback* pitched_readback(Dx12Buffer& buffer, std::uint64_t size) {
        if (!buffer.pitched || buffer.pitched->size < size) {
            if (buffer.pitched) {
                tracker_.retire([old = std::move(buffer.pitched)] {});
            }
            auto resource = create_resource(D3D12_HEAP_TYPE_READBACK, buffer_resource(size, D3D12_RESOURCE_FLAG_NONE),
                                            D3D12_BARRIER_LAYOUT_UNDEFINED, buffer.desc.debug_name + " (pitched)");
            if (!resource) {
                log::error("rhi", resource.error());
                return nullptr;
            }
            auto pitched = std::make_shared<PitchedReadback>();
            pitched->resource = std::move(*resource);
            pitched->size = size;
            if (const HRESULT hr = pitched->resource->Map(0, nullptr, &pitched->mapped); FAILED(hr)) {
                log::error("rhi", gpu_error("mapping a pitched readback buffer", hr).error());
                return nullptr;
            }
            buffer.pitched = std::move(pitched);
        }
        return buffer.pitched.get();
    }

private:
    /// Executes a closed list, presents `present` if given, and signals the fence with a new SubmissionId.
    SubmissionId execute(std::unique_ptr<Recorder> recorder, Dx12Surface* present = nullptr) {
        std::array<ID3D12CommandList*, 1> lists{recorder->list.Get()};
        queue_->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
        if (present != nullptr && present->swapchain) {
            if (const HRESULT hr = present->swapchain->Present(present->vsync ? 1 : 0, 0); FAILED(hr)) {
                if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
                    report_device_removed();
                } else {
                    log::error("rhi", Error{ErrorCode::rhi_invalid_surface,
                                            std::format("DXGI: Present failed ({}): {}", hresult(hr),
                                                        present_diagnostics(*present)),
                                            {}});
                }
            }
        }
        const SubmissionId submission = tracker_.next_submission();
        queue_->Signal(fence_.Get(), submission.value);
        recorder->last_use = submission.value;
        give_back(std::move(recorder));
        return submission;
    }

    Result<ComPtr<ID3D12Resource>> create_resource(D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC1& desc,
                                                   D3D12_BARRIER_LAYOUT layout, std::string_view name) {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = heap;
        ComPtr<ID3D12Resource> resource;
        if (const HRESULT hr = device_->CreateCommittedResource3(&properties, D3D12_HEAP_FLAG_NONE, &desc, layout,
                                                                 nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&resource));
            FAILED(hr)) {
            return gpu_error(std::format("creating '{}' ({} bytes)", name, desc.Width), hr);
        }
        set_name(resource.Get(), name);
        return resource;
    }

    static std::unexpected<Error> heap_full(std::string_view heap, std::uint32_t capacity) {
        return make_error(ErrorCode::rhi_gpu_error,
                          std::format("the {} descriptor heap is full ({} slots)", heap, capacity),
                          "Destroyed resources free their slots once the GPU is done with them; this many live "
                          "resources need a bigger heap (dx12_device.cpp).");
    }

    void create_buffer_views(const Dx12Buffer& buffer, std::uint64_t size) {
        const std::uint32_t stride = buffer.desc.stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        if (stride != 0) {  // StructuredBuffer<T>
            srv.Format = DXGI_FORMAT_UNKNOWN;
            srv.Buffer.NumElements = static_cast<UINT>(size / stride);
            srv.Buffer.StructureByteStride = stride;
            uav.Format = DXGI_FORMAT_UNKNOWN;
            uav.Buffer.NumElements = srv.Buffer.NumElements;
            uav.Buffer.StructureByteStride = stride;
        } else {  // ByteAddressBuffer
            srv.Format = DXGI_FORMAT_R32_TYPELESS;
            srv.Buffer.NumElements = static_cast<UINT>(size / 4);
            srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            uav.Format = DXGI_FORMAT_R32_TYPELESS;
            uav.Buffer.NumElements = srv.Buffer.NumElements;
            uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        }
        device_->CreateShaderResourceView(buffer.resource.Get(), &srv, view_heap_.cpu(buffer.srv));
        if (buffer.uav != 0) {
            device_->CreateUnorderedAccessView(buffer.resource.Get(), nullptr, &uav, view_heap_.cpu(buffer.uav));
        }
    }

    /// Copies `data` into a device-local buffer through a staging buffer, on its own submission. The queue runs
    /// in order, so every later submission sees the data; the staging buffer is retired with it.
    Result<> upload(ID3D12Resource* destination, std::span<const std::byte> data) {
        auto staging = create_resource(D3D12_HEAP_TYPE_UPLOAD, buffer_resource(data.size(), D3D12_RESOURCE_FLAG_NONE),
                                       D3D12_BARRIER_LAYOUT_UNDEFINED, "upload staging");
        if (!staging) {
            return std::unexpected(staging.error());
        }
        void* mapped = nullptr;
        const D3D12_RANGE nothing_read{0, 0};
        if (const HRESULT hr = (*staging)->Map(0, &nothing_read, &mapped); FAILED(hr)) {
            return gpu_error("mapping an upload staging buffer", hr);
        }
        std::memcpy(mapped, data.data(), data.size());
        std::unique_ptr<Recorder> recorder = take_recorder();
        if (!recorder) {
            return make_error(ErrorCode::rhi_gpu_error, "D3D12: no command list for an upload");
        }
        recorder->list->CopyBufferRegion(destination, 0, staging->Get(), 0, data.size());
        if (const HRESULT hr = recorder->list->Close(); FAILED(hr)) {
            give_back(std::move(recorder));
            return gpu_error("recording a buffer upload", hr);
        }
        static_cast<void>(execute(std::move(recorder)));
        tracker_.retire([keep = std::move(*staging)] {});
        return {};
    }

    /// Copies every mip through a staging buffer laid out by GetCopyableFootprints1, on its own submission, and
    /// leaves the texture in SHADER_RESOURCE (the RHI's shader_read). The staging buffer is retired with it.
    Result<> upload_texture(ID3D12Resource* texture, const D3D12_RESOURCE_DESC1& resource_desc, const TextureDesc& desc,
                            std::span<const std::span<const std::byte>> mips) {
        const auto count = static_cast<UINT>(mips.size());
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        std::vector<UINT> rows(count);
        std::vector<UINT64> row_sizes(count);
        UINT64 total = 0;
        device_->GetCopyableFootprints1(&resource_desc, 0, count, 0, footprints.data(), rows.data(), row_sizes.data(),
                                        &total);
        auto staging = create_resource(D3D12_HEAP_TYPE_UPLOAD, buffer_resource(total, D3D12_RESOURCE_FLAG_NONE),
                                       D3D12_BARRIER_LAYOUT_UNDEFINED, desc.debug_name + " (upload)");
        if (!staging) {
            return std::unexpected(staging.error());
        }
        void* mapped = nullptr;
        const D3D12_RANGE nothing_read{0, 0};
        if (const HRESULT hr = (*staging)->Map(0, &nothing_read, &mapped); FAILED(hr)) {
            return gpu_error("mapping a texture upload buffer", hr);
        }
        for (UINT level = 0; level < count; ++level) {
            const std::uint64_t packed = row_bytes(desc.format, mip_size(desc.width, level));
            for (UINT row = 0; row < rows[level]; ++row) {
                std::memcpy(static_cast<std::byte*>(mapped) + footprints[level].Offset +
                                std::uint64_t{row} * footprints[level].Footprint.RowPitch,
                            mips[level].data() + row * packed, packed);
            }
        }
        std::unique_ptr<Recorder> recorder = take_recorder();
        if (!recorder) {
            return make_error(ErrorCode::rhi_gpu_error, "D3D12: no command list for a texture upload");
        }
        ID3D12GraphicsCommandList7* list = recorder->list.Get();
        const auto transition = [&](D3D12_BARRIER_SYNC sync_before, D3D12_BARRIER_ACCESS access_before,
                                    D3D12_BARRIER_LAYOUT layout_before, D3D12_BARRIER_SYNC sync_after,
                                    D3D12_BARRIER_ACCESS access_after, D3D12_BARRIER_LAYOUT layout_after) {
            D3D12_TEXTURE_BARRIER barrier{};
            barrier.SyncBefore = sync_before;
            barrier.SyncAfter = sync_after;
            barrier.AccessBefore = access_before;
            barrier.AccessAfter = access_after;
            barrier.LayoutBefore = layout_before;
            barrier.LayoutAfter = layout_after;
            barrier.pResource = texture;
            barrier.Subresources.IndexOrFirstMipLevel = 0xffffffff;
            D3D12_BARRIER_GROUP group{};
            group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            group.NumBarriers = 1;
            group.pTextureBarriers = &barrier;
            list->Barrier(1, &group);
        };
        transition(D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_UNDEFINED,
                   D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_LAYOUT_COPY_DEST);
        for (UINT level = 0; level < count; ++level) {
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = texture;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = level;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = staging->Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint = footprints[level];
            // The footprint covers whole 4x4 blocks. The debug layer wants the box block-aligned for mips smaller
            // than a block (2x2 copies as 4x4) but no larger than the mip for an unaligned top level (6x6 is 6x6).
            const std::uint32_t least = is_block_compressed(desc.format) ? 4 : 1;
            const D3D12_BOX box{
                0, 0, 0, std::max(mip_size(desc.width, level), least), std::max(mip_size(desc.height, level), least),
                1};
            list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        }
        transition(D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_LAYOUT_COPY_DEST,
                   D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
        if (const HRESULT hr = list->Close(); FAILED(hr)) {
            give_back(std::move(recorder));
            return gpu_error(std::format("recording the upload of '{}'", desc.debug_name), hr);
        }
        static_cast<void>(execute(std::move(recorder)));
        tracker_.retire([keep = std::move(*staging)] {});
        return {};
    }

    void free_descriptors(const Dx12Buffer& b) {
        view_heap_.release(b.srv);
        view_heap_.release(b.uav);
    }
    void free_descriptors(const Dx12Texture& t) {
        view_heap_.release(t.srv);
        view_heap_.release(t.uav);
        rtv_heap_.release(t.rtv);
    }
    void free_descriptors(const Dx12Sampler& s) { sampler_heap_.release(s.index); }
    void free_descriptors(const Dx12Pipeline& /*pipeline*/) const {}
    void free_descriptors(const Dx12Accel& a) { view_heap_.release(a.srv); }

    Result<AccelHandle> make_accel(Dx12Accel accel, bool allow_refit, std::string_view name) {
        if (!caps_.hardware_ray_tracing) {
            return make_error(ErrorCode::rhi_backend_unavailable,
                              std::format("'{}' does not support DXR 1.1", caps_.device_name),
                              "Acceleration structures need DXR 1.1 (ray tracing tier 1.1); WARP from the Agility "
                              "SDK has it (ADR 0031).");
        }
        accel.inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        if (allow_refit) {
            accel.inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        }
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
        device_->GetRaytracingAccelerationStructurePrebuildInfo(&accel.inputs, &sizes);
        auto buffer =
            create_resource(D3D12_HEAP_TYPE_DEFAULT,
                            buffer_resource(align_up(std::max<std::uint64_t>(sizes.ResultDataMaxSizeInBytes, 256),
                                                     D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT),
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                                                D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE),
                            D3D12_BARRIER_LAYOUT_UNDEFINED, name);
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        auto scratch = create_resource(
            D3D12_HEAP_TYPE_DEFAULT,
            buffer_resource(
                std::max<std::uint64_t>({sizes.ScratchDataSizeInBytes, sizes.UpdateScratchDataSizeInBytes, 256}),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_BARRIER_LAYOUT_UNDEFINED, std::string(name) + " scratch");
        if (!scratch) {
            return std::unexpected(scratch.error());
        }
        accel.buffer = std::move(*buffer);
        accel.scratch = std::move(*scratch);
        accel.inputs.pGeometryDescs = nullptr;  // points into `accel`, which moves; set again at build time
        if (accel.tlas) {
            accel.srv = view_heap_.allocate();
            if (accel.srv == 0) {
                return heap_full("CBV/SRV/UAV", view_heap_.capacity());
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.RaytracingAccelerationStructure.Location = accel.buffer->GetGPUVirtualAddress();
            device_->CreateShaderResourceView(nullptr, &srv, view_heap_.cpu(accel.srv));
        }
        return accels_.insert(std::move(accel));
    }

    /// 64 root-constant DWORDs at b0 (the Slang parameter block) and both heaps directly indexed (ADR 0024).
    Result<> create_root_signature() {
        D3D12_ROOT_PARAMETER1 constants{};
        constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        constants.Constants.ShaderRegister = 0;
        constants.Constants.RegisterSpace = 0;
        constants.Constants.Num32BitValues = static_cast<UINT>(max_parameter_bytes / 4);
        constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = 1;
        desc.Desc_1_1.pParameters = &constants;
        desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                              D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> errors;
        if (const HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &errors); FAILED(hr)) {
            return make_error(ErrorCode::rhi_gpu_error,
                              std::format("D3D12: serializing the root signature failed ({}): {}", hresult(hr),
                                          errors ? static_cast<const char*>(errors->GetBufferPointer()) : ""));
        }
        if (const HRESULT hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                            IID_PPV_ARGS(&root_signature_));
            FAILED(hr)) {
            return gpu_error("CreateRootSignature", hr);
        }
        return {};
    }

    /// The DXIL of one entry point (cmake/VeachShaders.cmake compiles it on every Windows build).
    static Result<std::span<const std::uint8_t>> dxil(const ShaderLibrary* library, std::string_view entry) {
        const ShaderEntry* e = library ? library->find(entry) : nullptr;
        if (e == nullptr) {
            return make_error(
                ErrorCode::rhi_backend_unavailable,
                std::format("shader library '{}' has no entry point '{}'", library ? library->name : "(null)", entry),
                "Check the ENTRIES list passed to veach_add_shader().");
        }
        if (e->dxil.empty()) {
            return make_error(ErrorCode::rhi_not_implemented,
                              std::format("shader '{}' entry '{}' has no DXIL", library->name, entry),
                              "Compile it with veach_add_shader() on Windows (docs/adr/0026-shader-compilation.md).");
        }
        return e->dxil;
    }

    /// The handle dies now; the D3D12 object and its descriptor slots once the GPU is done with them (ADR 0029).
    template <class Map, class H>
    void retire(Map& map, H h) {
        if (auto* object = map.get(h)) {
            auto keep = std::make_shared<std::decay_t<decltype(*object)>>(std::move(*object));
            map.erase(h);
            tracker_.retire([this, keep] { free_descriptors(*keep); });
        }
    }

    Dx12Surface* surface(SurfaceId id) {
        return id.is_valid() && id.value <= surfaces_.size() ? &surfaces_[id.value - 1] : nullptr;
    }

    /// Registers the swapchain's buffers as textures with RTVs. They start in PRESENT, as the graph imports them.
    Result<> wrap_images(Dx12Surface& s) {
        DXGI_SWAP_CHAIN_DESC1 d{};
        s.swapchain->GetDesc1(&d);
        for (UINT i = 0; i < d.BufferCount; ++i) {
            Dx12Texture image;
            if (const HRESULT hr = s.swapchain->GetBuffer(i, IID_PPV_ARGS(&image.resource)); FAILED(hr)) {
                return gpu_error("getting a swapchain buffer", hr);
            }
            set_name(image.resource.Get(), "swapchain");
            image.desc = {d.Width, d.Height, surface_format, TextureUsage::color_target, "swapchain"};
            image.rtv = rtv_heap_.allocate();
            if (image.rtv == 0) {
                return heap_full("RTV", rtv_heap_.capacity());
            }
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = d.Format;
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device_->CreateRenderTargetView(image.resource.Get(), &rtv, rtv_heap_.cpu(image.rtv));
            s.images.push_back(textures_.insert(std::move(image)));
        }
        return {};
    }

    /// Only after wait_idle(): the GPU no longer uses the images, so their slots are freed at once.
    void release_images(Dx12Surface& s) {
        for (const TextureHandle h : s.images) {
            if (const Dx12Texture* t = textures_.get(h)) {
                free_descriptors(*t);
            }
            textures_.erase(h);
        }
        s.images.clear();
    }

    /// Reads the fence: marks finished submissions complete and runs the releases that are now safe.
    void poll() {
        const UINT64 done = fence_->GetCompletedValue();
        if (done == UINT64_MAX) {  // a removed device reports every fence value as reached
            report_device_removed();
            tracker_.mark_completed(tracker_.last_submitted());
        } else {
            tracker_.mark_completed({done});
        }
        tracker_.collect();
    }

    void report_device_removed() {
        if (device_removed_reported_) {
            return;
        }
        device_removed_reported_ = true;
        log::error("rhi", Error{ErrorCode::rhi_gpu_error,
                                std::format("D3D12 device removed ({})", hresult(device_->GetDeviceRemovedReason())),
                                "Run a debug build (D3D12 debug layer on) for the call that caused it."});
    }

    std::unique_ptr<Recorder> take_recorder() {
        const std::uint64_t done = tracker_.completed().value;
        std::unique_ptr<Recorder> recorder;
        for (auto it = recorders_.begin(); it != recorders_.end(); ++it) {
            if ((*it)->last_use <= done) {
                recorder = std::move(*it);
                recorders_.erase(it);
                break;
            }
        }
        if (!recorder) {
            recorder = std::make_unique<Recorder>();
            HRESULT hr =
                device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&recorder->allocator));
            if (SUCCEEDED(hr)) {
                hr = device_->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                                 IID_PPV_ARGS(&recorder->list));
            }
            if (FAILED(hr)) {
                log::error("rhi", gpu_error("creating a command list", hr).error());
                return nullptr;
            }
        } else {
            recorder->allocator->Reset();
        }
        recorder->list->Reset(recorder->allocator.Get(), nullptr);
        return recorder;
    }

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<ID3D12Device10> device_;
    ComPtr<ID3D12InfoQueue1> info_queue_;
    DWORD callback_cookie_ = 0;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    Capabilities caps_;
    SubmissionTracker tracker_;
    std::vector<std::unique_ptr<Recorder>> recorders_;
    DescriptorHeap view_heap_;
    DescriptorHeap sampler_heap_;
    DescriptorHeap rtv_heap_;
    SlotMap<Dx12Buffer, BufferHandle> buffers_;
    SlotMap<Dx12Texture, TextureHandle> textures_;
    SlotMap<Dx12Sampler, SamplerHandle> samplers_;
    SlotMap<Dx12Pipeline, PipelineHandle> pipelines_;
    SlotMap<Dx12Accel, AccelHandle> accels_;
    std::vector<Dx12Surface> surfaces_;
    ComPtr<ID3D12RootSignature> root_signature_;
    bool device_removed_reported_ = false;
};

// ---- Dx12CommandList ------------------------------------------------------------

void Dx12CommandList::begin_render_pass(const RenderPassDesc& desc) {
    push_debug_group(desc.debug_name);
    compute_pass_ = false;
    ID3D12GraphicsCommandList7* list = recorder_->list.Get();
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> targets{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    for (std::uint32_t i = 0; i < desc.color_count; ++i) {
        const ColorAttachment& c = desc.colors.at(i);
        Dx12Texture* t = device_.texture(c.texture);
        VEACH_ASSERT(t != nullptr && t->rtv != 0, "render pass '{}': color attachment {} is not a render target",
                     desc.debug_name, i);
        targets.at(i) = device_.rtv(t->rtv);
        if (c.load == LoadOp::clear) {
            list->ClearRenderTargetView(targets.at(i), c.clear_color.data(), 0, nullptr);
        }
        width = t->desc.width;
        height = t->desc.height;
    }
    list->OMSetRenderTargets(desc.color_count, targets.data(), FALSE, nullptr);
    // Metal sizes the viewport and scissor to the attachments implicitly; D3D12 needs them set.
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Dx12CommandList::set_pipeline(PipelineHandle handle) {
    Dx12Pipeline* p = device_.pipeline(handle);
    VEACH_ASSERT(p != nullptr, "set_pipeline: invalid pipeline");
    VEACH_ASSERT(p->compute == compute_pass_, "set_pipeline: pipeline kind does not match the current pass");
    recorder_->list->SetPipelineState(p->state.Get());
}

void Dx12CommandList::set_parameters(const void* data, std::size_t size) {
    VEACH_ASSERT(size <= max_parameter_bytes, "parameter block of {} bytes exceeds {}", size, max_parameter_bytes);
    std::array<std::uint32_t, max_parameter_bytes / 4> constants{};
    std::memcpy(constants.data(), data, size);
    const auto count = static_cast<UINT>((size + 3) / 4);
    if (compute_pass_) {
        recorder_->list->SetComputeRoot32BitConstants(0, count, constants.data(), 0);
    } else {
        recorder_->list->SetGraphicsRoot32BitConstants(0, count, constants.data(), 0);
    }
}

void Dx12CommandList::encode_accel(AccelHandle handle, bool refit, std::span<const TlasInstance> instances) {
    Dx12Accel* a = device_.accel(handle);
    VEACH_ASSERT(a != nullptr, "invalid acceleration structure");
    ID3D12GraphicsCommandList7* list = recorder_->list.Get();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = a->inputs;
    if (a->tlas) {
        VEACH_ASSERT(instances.size() <= a->max_instances, "{} instances exceed the TLAS capacity of {}",
                     instances.size(), a->max_instances);
        const std::size_t slot = device_.frame_slot() % a->instances.size();
        auto* out = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(a->instances_mapped.at(slot));
        for (std::size_t i = 0; i < instances.size(); ++i) {
            const TlasInstance& in = instances[i];
            D3D12_RAYTRACING_INSTANCE_DESC d{};
            static_assert(sizeof(d.Transform) == sizeof(in.transform), "TlasInstance::transform is D3D12's layout");
            std::memcpy(d.Transform, in.transform.data(), sizeof(d.Transform));
            d.InstanceID = in.instance_id & 0xFFFFFFu;
            d.InstanceMask = in.mask;
            d.InstanceContributionToHitGroupIndex = 0;
            d.Flags = in.opaque ? D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE : D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
            const Dx12Accel* blas = device_.accel(a->blases.at(in.blas_index));
            VEACH_ASSERT(blas != nullptr, "TLAS instance {} references a destroyed BLAS", i);
            d.AccelerationStructure = blas->buffer->GetGPUVirtualAddress();
            out[i] = d;
        }
        build.Inputs.NumDescs = static_cast<UINT>(instances.size());
        build.Inputs.InstanceDescs = a->instances.at(slot)->GetGPUVirtualAddress();
        // The graph orders passes, not the builds inside one: the BLASes this TLAS references may have been
        // built just before it in the same pass. Metal orders them through its encoders; DX12 needs a barrier.
        D3D12_GLOBAL_BARRIER blas_built{};
        blas_built.SyncBefore = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
        blas_built.SyncAfter = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
        blas_built.AccessBefore = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;
        blas_built.AccessAfter = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ;
        D3D12_BARRIER_GROUP group{};
        group.Type = D3D12_BARRIER_TYPE_GLOBAL;
        group.NumBarriers = 1;
        group.pGlobalBarriers = &blas_built;
        list->Barrier(1, &group);
    } else {
        build.Inputs.pGeometryDescs = &a->geometry;
    }
    if (refit) {
        build.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        build.SourceAccelerationStructureData = a->buffer->GetGPUVirtualAddress();
    }
    build.DestAccelerationStructureData = a->buffer->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = a->scratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
}

void Dx12CommandList::barrier(std::span<const Barrier> barriers) {
    std::vector<D3D12_TEXTURE_BARRIER> textures;
    std::vector<D3D12_BUFFER_BARRIER> buffers;
    for (const Barrier& b : barriers) {
        ID3D12Resource* resource = device_.resource(b.resource);
        VEACH_ASSERT(resource != nullptr, "barrier on a dead resource {}:{}", b.resource.index, b.resource.generation);
        const BarrierState before = barrier_state(b.before);
        const BarrierState after = barrier_state(b.after);
        // `discard` is an optimization (the old contents are not needed); a plain transition from the real
        // `before` state is always correct, so v0 keeps the contents (ADR 0029, item 5).
        if (b.resource.kind == ResourceKind::texture) {
            D3D12_TEXTURE_BARRIER t{};
            t.SyncBefore = before.sync;
            t.SyncAfter = after.sync;
            t.AccessBefore = before.access;
            t.AccessAfter = after.access;
            t.LayoutBefore = before.layout;
            t.LayoutAfter = after.layout;
            t.pResource = resource;
            t.Subresources.IndexOrFirstMipLevel = 0xffffffff;  // all subresources
            textures.push_back(t);
        } else {
            D3D12_BUFFER_BARRIER buf{};
            buf.SyncBefore = before.sync;
            buf.SyncAfter = after.sync;
            buf.AccessBefore = before.access;
            buf.AccessAfter = after.access;
            buf.pResource = resource;
            buf.Offset = 0;
            buf.Size = UINT64_MAX;
            buffers.push_back(buf);
        }
    }
    std::array<D3D12_BARRIER_GROUP, 2> groups{};
    UINT count = 0;
    if (!textures.empty()) {
        groups.at(count).Type = D3D12_BARRIER_TYPE_TEXTURE;
        groups.at(count).NumBarriers = static_cast<UINT32>(textures.size());
        groups.at(count).pTextureBarriers = textures.data();
        ++count;
    }
    if (!buffers.empty()) {
        groups.at(count).Type = D3D12_BARRIER_TYPE_BUFFER;
        groups.at(count).NumBarriers = static_cast<UINT32>(buffers.size());
        groups.at(count).pBufferBarriers = buffers.data();
        ++count;
    }
    if (count != 0) {
        recorder_->list->Barrier(count, groups.data());
    }
}

void Dx12CommandList::copy_texture_to_buffer(TextureHandle src, BufferHandle dst) {
    Dx12Texture* t = device_.texture(src);
    Dx12Buffer* b = device_.buffer(dst);
    VEACH_ASSERT(t != nullptr && b != nullptr, "copy_texture_to_buffer: invalid handles");
    const auto row = static_cast<std::uint32_t>(row_bytes(t->desc.format, t->desc.width));
    const std::uint32_t rows = row_count(t->desc.format, t->desc.height);
    VEACH_ASSERT(b->desc.size >= std::uint64_t{row} * rows, "copy_texture_to_buffer: buffer too small");
    // D3D12 copies rows with a 256-byte pitch. Unaligned rows go through a pitched buffer that map() packs.
    const auto pitch = static_cast<std::uint32_t>(align_up(row, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    ID3D12Resource* target = b->resource.Get();
    if (pitch != row) {
        PitchedReadback* p = device_.pitched_readback(*b, std::uint64_t{pitch} * rows);
        if (p == nullptr) {
            return;
        }
        p->row_bytes = row;
        p->pitch = pitch;
        p->rows = rows;
        p->pending = true;
        target = p->resource.Get();
    }
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = t->resource.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = target;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint.Format = to_dxgi(t->desc.format);
    // Block-compressed footprints are whole blocks.
    const std::uint32_t block = is_block_compressed(t->desc.format) ? 4 : 1;
    to.PlacedFootprint.Footprint.Width = (t->desc.width + block - 1) / block * block;
    to.PlacedFootprint.Footprint.Height = (t->desc.height + block - 1) / block * block;
    to.PlacedFootprint.Footprint.Depth = 1;
    to.PlacedFootprint.Footprint.RowPitch = pitch;
    recorder_->list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
}

Dx12CommandList::~Dx12CommandList() {
    if (recorder_) {
        recorder_->list->Close();
        device_.give_back(std::move(recorder_));
    }
}

static_assert(!std::is_abstract_v<Dx12Device>, "Dx12Device must implement the whole rhi::Device interface");

/// VEACH_DX12_ADAPTER=warp forces WARP, the software device, even when a GPU is present.
bool warp_forced() {
    std::array<char, 16> value{};
    const DWORD length = GetEnvironmentVariableA("VEACH_DX12_ADAPTER", value.data(), static_cast<DWORD>(value.size()));
    return length > 0 && length < value.size() && std::string_view(value.data(), length) == "warp";
}

struct Candidate {
    ComPtr<ID3D12Device10> device;
    Capabilities caps;
};

/// Creates a device on `adapter` and checks the feature floor; on failure, says why.
std::variant<Candidate, std::string> try_adapter(IDXGIAdapter1* adapter, bool warp) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    Candidate candidate;
    candidate.caps.device_name = narrow(desc.Description);
    if (warp) {
        candidate.caps.device_name += " (WARP)";
    }
    candidate.caps.gpu = true;
    ComPtr<ID3D12Device> device;
    if (const HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)); FAILED(hr)) {
        return std::format("'{}': D3D12CreateDevice failed ({})", candidate.caps.device_name, hresult(hr));
    }
    if (FAILED(device.As(&candidate.device))) {
        return std::format("'{}': no ID3D12Device10 (the Agility SDK in D3D12\\ was not loaded)",
                           candidate.caps.device_name);
    }
    if (auto missing = missing_feature_floor(device.Get(), candidate.caps)) {
        return std::format("'{}' lacks {}", candidate.caps.device_name, *missing);
    }
    return candidate;
}

void log_runtime(ID3D12Device* device, bool warp) {
    ComPtr<ID3D12DeviceConfiguration> config;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&config)))) {
        log::debug("rhi", "D3D12 runtime: Agility SDK {}", config->GetDesc().SDKVersion);
    }
    if (HMODULE module = GetModuleHandleW(L"d3d10warp.dll"); warp && module != nullptr) {
        std::array<wchar_t, MAX_PATH> path{};
        GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        log::debug("rhi", "WARP: {}", narrow(path.data()));
    }
}

}  // namespace

Result<std::unique_ptr<Device>> create_dx12_device(const DeviceDesc& desc) {
    if (desc.validation) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            log::debug("rhi", "D3D12 debug layer enabled");
        } else {
            log::warn("rhi", "the D3D12 debug layer is unavailable (D3D12\\d3d12SDKLayers.dll missing?)");
        }
    }
    // The DXGI debug layer (dxgidebug.dll) comes with the Graphics Tools feature, which may be missing.
    ComPtr<IDXGIFactory6> factory;
    if (desc.validation && SUCCEEDED(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory)))) {
        log::debug("rhi", "DXGI debug layer enabled");
    } else if (const HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)); FAILED(hr)) {
        return make_error(ErrorCode::rhi_backend_unavailable, std::format("DXGI is unavailable ({})", hresult(hr)));
    }

    // A hardware adapter that meets the floor, fastest first; otherwise WARP (hosted CI has no GPU, ADR 0031).
    std::vector<std::string> rejected;
    std::optional<Candidate> chosen;
    bool warp = warp_forced();
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; !warp && !chosen &&
                     factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                         IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 adapter_desc{};
        adapter->GetDesc1(&adapter_desc);
        if ((adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        auto result = try_adapter(adapter.Get(), false);
        if (auto* candidate = std::get_if<Candidate>(&result)) {
            chosen = std::move(*candidate);
        } else {
            rejected.push_back(std::get<std::string>(result));
        }
    }
    if (!chosen) {
        warp = true;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) {
            auto result = try_adapter(adapter.Get(), true);
            if (auto* candidate = std::get_if<Candidate>(&result)) {
                chosen = std::move(*candidate);
            } else {
                rejected.push_back(std::get<std::string>(result));
            }
        }
    }
    if (!chosen) {
        std::string reasons;
        for (const std::string& r : rejected) {
            reasons += reasons.empty() ? r : "; " + r;
        }
        return make_error(ErrorCode::rhi_backend_unavailable,
                          std::format("no D3D12 device meets the feature floor: {}", reasons),
                          "Veach needs D3D12 with Shader Model 6.6, resource binding tier 3 and enhanced barriers "
                          "(ADR 0031); WARP from the Agility SDK provides all three.");
    }
    log_runtime(chosen->device.Get(), warp);

    auto device = std::make_unique<Dx12Device>(std::move(factory), std::move(chosen->device), std::move(chosen->caps),
                                               desc.validation, desc.frames_in_flight);
    if (auto ready = device->init(); !ready) {
        return std::unexpected(ready.error());
    }
    return std::unique_ptr<Device>(std::move(device));
}

}  // namespace veach::rhi
