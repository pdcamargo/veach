// DX12 backend (ADR 0009, VCH-012). D3D12 through the Agility SDK (ADR 0031): one
// direct queue and one fence whose values are SubmissionIds (ADR 0029). Built
// and verified step by step in hosted CI on WARP (docs/guides/dx12-port.md).

#include "dx12_device.hpp"

#include "../slot_map.hpp"

#include <veach/core/assert.hpp>
#include <veach/core/log.hpp>

#include <windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
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

std::unexpected<Error> gpu_error(std::string_view what, HRESULT hr) {
    return make_error(ErrorCode::rhi_gpu_error, std::format("D3D12: {} failed ({})", what, hresult(hr)));
}

void not_implemented(std::string_view what) {
    log::error("rhi", Error{ErrorCode::rhi_not_implemented, std::format("DX12: {} is not implemented yet", what),
                            "The DX12 backend is being built step by step (docs/guides/dx12-port.md, VCH-012)."});
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

    void begin_render_pass(const RenderPassDesc&) override { not_implemented("begin_render_pass"); }
    void end_render_pass() override {}
    void begin_compute_pass(std::string_view) override { not_implemented("begin_compute_pass"); }
    void end_compute_pass() override {}
    void set_pipeline(PipelineHandle) override { not_implemented("set_pipeline"); }
    void set_parameters(const void*, std::size_t) override { not_implemented("set_parameters"); }
    void draw(std::uint32_t, std::uint32_t, std::uint32_t) override { not_implemented("draw"); }
    void dispatch(std::uint32_t, std::uint32_t, std::uint32_t) override { not_implemented("dispatch"); }
    void build_blas(AccelHandle) override { not_implemented("build_blas"); }
    void refit_blas(AccelHandle) override { not_implemented("refit_blas"); }
    void build_tlas(AccelHandle, std::span<const TlasInstance>) override { not_implemented("build_tlas"); }
    void refit_tlas(AccelHandle, std::span<const TlasInstance>) override { not_implemented("refit_tlas"); }
    void copy_texture_to_buffer(TextureHandle, BufferHandle) override { not_implemented("copy_texture_to_buffer"); }
    void use(ResourceRef, ResourceState) override {}  // committed resources are always resident
    void barrier(std::span<const Barrier> barriers) override {
        if (!barriers.empty()) {
            not_implemented("barrier");
        }
    }
    void push_debug_group(std::string_view name) override {
        const std::string text(name);  // PIX_EVENT_ANSI_VERSION (1): a null-terminated ANSI string
        recorder_->list->BeginEvent(1, text.c_str(), static_cast<UINT>(text.size() + 1));
    }
    void pop_debug_group() override { recorder_->list->EndEvent(); }

    /// Closes the list and hands its recorder back; called once, by Dx12Device::submit.
    std::unique_ptr<Recorder> finish() {
        recorder_->list->Close();
        return std::move(recorder_);
    }

private:
    Dx12Device& device_;
    std::unique_ptr<Recorder> recorder_;
};

class Dx12Device final : public Device {
public:
    Dx12Device(ComPtr<ID3D12Device10> device, Capabilities caps, bool validation, std::uint32_t frames_in_flight)
        : device_(std::move(device)), caps_(std::move(caps)), tracker_(frames_in_flight) {
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
        return {};
    }

    [[nodiscard]] Backend backend() const override { return Backend::dx12; }
    [[nodiscard]] const Capabilities& capabilities() const override { return caps_; }

    // TODO(VCH-012): resources, descriptors, pipelines, acceleration structures and surfaces (next steps).
    [[nodiscard]] Result<BufferHandle> create_buffer(const BufferDesc&, std::span<const std::byte>) override {
        return stub("create_buffer");
    }
    [[nodiscard]] Result<TextureHandle> create_texture(const TextureDesc&) override { return stub("create_texture"); }
    [[nodiscard]] Result<SamplerHandle> create_sampler(const SamplerDesc&) override { return stub("create_sampler"); }
    [[nodiscard]] Result<PipelineHandle> create_graphics_pipeline(const GraphicsPipelineDesc&) override {
        return stub("create_graphics_pipeline");
    }
    [[nodiscard]] Result<PipelineHandle> create_compute_pipeline(const ComputePipelineDesc&) override {
        return stub("create_compute_pipeline");
    }
    [[nodiscard]] Result<AccelHandle> create_blas(const BlasDesc&) override { return stub("create_blas"); }
    [[nodiscard]] Result<AccelHandle> create_tlas(const TlasDesc&) override { return stub("create_tlas"); }
    void destroy(BufferHandle) override {}
    void destroy(TextureHandle) override {}
    void destroy(SamplerHandle) override {}
    void destroy(PipelineHandle) override {}
    void destroy(AccelHandle) override {}
    [[nodiscard]] void* map(BufferHandle) override { return nullptr; }
    [[nodiscard]] const TextureDesc* texture_desc(TextureHandle) const override { return nullptr; }
    [[nodiscard]] DescriptorHandle descriptor(BufferHandle, View) override { return {}; }
    [[nodiscard]] DescriptorHandle descriptor(TextureHandle, View) override { return {}; }
    [[nodiscard]] DescriptorHandle descriptor(SamplerHandle) override { return {}; }
    [[nodiscard]] DescriptorHandle descriptor(AccelHandle) override { return {}; }
    [[nodiscard]] Result<SurfaceId> create_surface(const NativeWindowHandle&, const SurfaceDesc&) override {
        return stub("create_surface");
    }
    void resize_surface(SurfaceId, std::uint32_t, std::uint32_t) override {}
    [[nodiscard]] Result<TextureHandle> acquire_image(SurfaceId) override { return stub("acquire_image"); }
    void destroy_surface(SurfaceId) override {}

    // ---- work

    [[nodiscard]] std::unique_ptr<CommandList> begin_commands(std::string_view debug_name) override {
        std::unique_ptr<Recorder> recorder = take_recorder();
        if (!recorder) {
            return nullptr;
        }
        auto list = std::make_unique<Dx12CommandList>(*this, std::move(recorder));
        list->push_debug_group(debug_name);
        return list;
    }

    SubmissionId submit(std::unique_ptr<CommandList> commands, SurfaceId /*present*/) override {
        auto* list = static_cast<Dx12CommandList*>(commands.get());
        list->pop_debug_group();
        std::unique_ptr<Recorder> recorder = list->finish();
        std::array<ID3D12CommandList*, 1> lists{recorder->list.Get()};
        queue_->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
        const SubmissionId submission = tracker_.next_submission();
        queue_->Signal(fence_.Get(), submission.value);
        recorder->last_use = submission.value;
        give_back(std::move(recorder));
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

private:
    static std::unexpected<Error> stub(std::string_view what) {
        return make_error(ErrorCode::rhi_not_implemented, std::format("DX12: {} is not implemented yet", what),
                          "The DX12 backend is being built step by step (docs/guides/dx12-port.md, VCH-012).");
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

    ComPtr<ID3D12Device10> device_;
    ComPtr<ID3D12InfoQueue1> info_queue_;
    DWORD callback_cookie_ = 0;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    Capabilities caps_;
    SubmissionTracker tracker_;
    std::vector<std::unique_ptr<Recorder>> recorders_;
    bool device_removed_reported_ = false;
};

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
    ComPtr<IDXGIFactory6> factory;
    if (const HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)); FAILED(hr)) {
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

    auto device = std::make_unique<Dx12Device>(std::move(chosen->device), std::move(chosen->caps), desc.validation,
                                               desc.frames_in_flight);
    if (auto ready = device->init(); !ready) {
        return std::unexpected(ready.error());
    }
    return std::unique_ptr<Device>(std::move(device));
}

}  // namespace veach::rhi
