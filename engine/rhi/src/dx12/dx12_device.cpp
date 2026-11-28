// DX12 backend (ADR 0004, VCH-012). D3D12 through the Agility SDK (ADR 0033): one
// direct queue and one fence whose values are SubmissionIds (ADR 0030). Built
// and verified step by step in hosted CI on WARP (docs/guides/dx12-port.md).

#include "dx12_device.hpp"

#include "../frame_timings.hpp"
#include "../pipeline_validation.hpp"
#include "../ray_tracing.hpp"
#include "../slot_map.hpp"
#include "../surface_validation.hpp"
#include "../texture_validation.hpp"

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
#include <map>
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

std::string_view breadcrumb_op(D3D12_AUTO_BREADCRUMB_OP op) {
    switch (op) {
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
        case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE:
            return "BuildRaytracingAccelerationStructure";
        case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return "Barrier";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
        case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
        default: return "another command";
    }
}

/// What DRED recorded about a device removal: the first command that did not finish in each command list, the
/// debug group around it, and the allocations at a faulting address. Empty without DRED (no debug layer).
std::string dred_report(ID3D12Device* device) {
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
        return {};
    }
    std::string out;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
        for (const D3D12_AUTO_BREADCRUMB_NODE1* node = crumbs.pHeadAutoBreadcrumbNode; node != nullptr;
             node = node->pNext) {
            const UINT done = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
            if (node->pCommandHistory == nullptr || done >= node->BreadcrumbCount) {
                continue;  // finished, or never started
            }
            std::string group;
            for (UINT i = 0; i < node->BreadcrumbContextsCount; ++i) {
                const D3D12_DRED_BREADCRUMB_CONTEXT& context = node->pBreadcrumbContexts[i];
                if (context.BreadcrumbIndex <= done && context.pContextString != nullptr) {
                    group = narrow(context.pContextString);
                }
            }
            out += std::format("; command list '{}' stopped at {} (command {} of {}{})",
                               node->pCommandListDebugNameW != nullptr ? narrow(node->pCommandListDebugNameW) : "",
                               breadcrumb_op(node->pCommandHistory[done]), done + 1, node->BreadcrumbCount,
                               group.empty() ? "" : std::format(", in '{}'", group));
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA != 0) {
        out += std::format("; page fault at GPU address 0x{:X}", fault.PageFaultVA);
        for (const D3D12_DRED_ALLOCATION_NODE1* node = fault.pHeadExistingAllocationNode; node != nullptr;
             node = node->pNext) {
            out += std::format(", in '{}'", node->ObjectNameW != nullptr ? narrow(node->ObjectNameW) : "?");
        }
        for (const D3D12_DRED_ALLOCATION_NODE1* node = fault.pHeadRecentFreedAllocationNode; node != nullptr;
             node = node->pNext) {
            out += std::format(", in freed '{}'", node->ObjectNameW != nullptr ? narrow(node->ObjectNameW) : "?");
        }
    }
    return out;
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
        case Format::r32_uint: return DXGI_FORMAT_R32_UINT;
        case Format::rg32_uint: return DXGI_FORMAT_R32G32_UINT;
        case Format::r8_unorm: return DXGI_FORMAT_R8_UNORM;
        case Format::rg8_unorm: return DXGI_FORMAT_R8G8_UNORM;
        case Format::r16_float: return DXGI_FORMAT_R16_FLOAT;
        case Format::rg16_float: return DXGI_FORMAT_R16G16_FLOAT;
        case Format::undefined: return DXGI_FORMAT_UNKNOWN;
    }
    return DXGI_FORMAT_UNKNOWN;
}

/// Depth textures are typeless resources so that they can have both a depth view (D32_FLOAT) and a shader view
/// (R32_FLOAT); copies of them name the typeless format, as GetCopyableFootprints does.
DXGI_FORMAT resource_format(Format format) {
    return format == Format::depth32_float ? DXGI_FORMAT_R32_TYPELESS : to_dxgi(format);
}

// ---- texture views (ADR 0090) ----------------------------------------------------------------

/// A read view of `range` of a texture, seen as `type` (view_type()).
D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc(const TextureDesc& desc, TextureType type, const TextureRange& r) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = desc.format == Format::depth32_float ? DXGI_FORMAT_R32_FLOAT : to_dxgi(desc.format);
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    switch (type) {
        case TextureType::d2:
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MostDetailedMip = r.base_mip;
            srv.Texture2D.MipLevels = r.mip_count;
            break;
        case TextureType::d2_array:
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.MostDetailedMip = r.base_mip;
            srv.Texture2DArray.MipLevels = r.mip_count;
            srv.Texture2DArray.FirstArraySlice = r.base_layer;
            srv.Texture2DArray.ArraySize = r.layer_count;
            break;
        case TextureType::d3:
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            srv.Texture3D.MostDetailedMip = r.base_mip;
            srv.Texture3D.MipLevels = r.mip_count;
            break;
        case TextureType::cube:
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srv.TextureCube.MostDetailedMip = r.base_mip;
            srv.TextureCube.MipLevels = r.mip_count;
            break;
        case TextureType::cube_array:
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            srv.TextureCubeArray.MostDetailedMip = r.base_mip;
            srv.TextureCubeArray.MipLevels = r.mip_count;
            srv.TextureCubeArray.First2DArrayFace = r.base_layer;
            srv.TextureCubeArray.NumCubes = r.layer_count / 6;
            break;
    }
    return srv;
}

/// A storage view of mip `r.base_mip`: RWTexture2D, RWTexture3D (all of its depth) or RWTexture2DArray.
D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc(const TextureDesc& desc, TextureType type, const TextureRange& r) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = to_dxgi(desc.format);
    switch (type) {
        case TextureType::d2:
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Texture2D.MipSlice = r.base_mip;
            break;
        case TextureType::d3:
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            uav.Texture3D.MipSlice = r.base_mip;
            uav.Texture3D.WSize = UINT_MAX;  // every slice of the mip
            break;
        case TextureType::d2_array:
        case TextureType::cube:
        case TextureType::cube_array:
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uav.Texture2DArray.MipSlice = r.base_mip;
            uav.Texture2DArray.FirstArraySlice = r.base_layer;
            uav.Texture2DArray.ArraySize = r.layer_count;
            break;
    }
    return uav;
}

D3D12_RENDER_TARGET_VIEW_DESC rtv_desc(const TextureDesc& desc, TargetSlice slice) {
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = to_dxgi(desc.format);
    switch (desc.type) {
        case TextureType::d2:
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            rtv.Texture2D.MipSlice = slice.mip;
            break;
        case TextureType::d3:
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
            rtv.Texture3D.MipSlice = slice.mip;
            rtv.Texture3D.FirstWSlice = slice.layer;
            rtv.Texture3D.WSize = 1;
            break;
        case TextureType::d2_array:
        case TextureType::cube:
        case TextureType::cube_array:
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rtv.Texture2DArray.MipSlice = slice.mip;
            rtv.Texture2DArray.FirstArraySlice = slice.layer;
            rtv.Texture2DArray.ArraySize = 1;
            break;
    }
    return rtv;
}

D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc(const TextureDesc& desc, TargetSlice slice) {
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = to_dxgi(desc.format);
    if (desc.type == TextureType::d2) {
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv.Texture2D.MipSlice = slice.mip;
    } else {  // arrays and cubes; validate_texture_desc rejects 3D depth
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.MipSlice = slice.mip;
        dsv.Texture2DArray.FirstArraySlice = slice.layer;
        dsv.Texture2DArray.ArraySize = 1;
    }
    return dsv;
}

/// D3D12's subresource index (D3D12CalcSubresource, one plane).
UINT subresource(const TextureDesc& desc, std::uint32_t mip, std::uint32_t layer) {
    return mip + layer * std::max(1u, desc.mip_levels);
}

/// ADR 0090's blend presets.
D3D12_RENDER_TARGET_BLEND_DESC blend_desc(BlendMode mode) {
    D3D12_RENDER_TARGET_BLEND_DESC blend{};
    blend.BlendEnable = mode == BlendMode::opaque ? FALSE : TRUE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    switch (mode) {
        case BlendMode::opaque: break;
        case BlendMode::alpha:
            blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case BlendMode::premultiplied:
            blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case BlendMode::additive:
            blend.DestBlend = D3D12_BLEND_ONE;
            blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
            blend.DestBlendAlpha = D3D12_BLEND_ONE;
            break;
    }
    return blend;
}

D3D12_COMPARISON_FUNC to_d3d12(CompareOp op) {
    switch (op) {
        case CompareOp::never: return D3D12_COMPARISON_FUNC_NEVER;
        case CompareOp::less: return D3D12_COMPARISON_FUNC_LESS;
        case CompareOp::less_equal: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case CompareOp::greater: return D3D12_COMPARISON_FUNC_GREATER;
        case CompareOp::greater_equal: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case CompareOp::equal: return D3D12_COMPARISON_FUNC_EQUAL;
        case CompareOp::always: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

D3D12_CULL_MODE to_d3d12(CullMode mode) {
    switch (mode) {
        case CullMode::none: return D3D12_CULL_MODE_NONE;
        case CullMode::back: return D3D12_CULL_MODE_BACK;
        case CullMode::front: return D3D12_CULL_MODE_FRONT;
    }
    return D3D12_CULL_MODE_NONE;
}

/// ClearRenderTargetView takes floats, which D3D12 converts to the target's integers (saturating). 0xFFFFFFFF
/// is sent as 2^32, which saturates to it; values up to 2^24 are exact. Others round, and say so.
std::array<float, 4> integer_clear(const std::array<std::uint32_t, 4>& value, std::string_view pass) {
    std::array<float, 4> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::uint32_t v = value.at(i);
        out.at(i) = v == UINT32_MAX ? 4294967296.0f : static_cast<float>(v);
        if (v != UINT32_MAX && v > (1u << 24)) {
            log::error("rhi", Error{ErrorCode::rhi_not_implemented,
                                    std::format("render pass '{}': DX12 cannot clear an integer target to {} exactly",
                                                pass, v),
                                    "Integer clears go through floats on DX12: use values up to 2^24 or 0xFFFFFFFF."});
        }
    }
    return out;
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

/// What the device must support (ADR 0024/0025/0033). Returns what is missing, if anything.
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
        options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 && !ray_tracing_disabled();
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

/// What a descriptor slot holds, as veach/bindless.slang checks it (ADR 0027).
enum class SlotKind : std::uint32_t { none, texture, storage_texture, buffer, storage_buffer, sampler, accel };

/// A descriptor heap with a free list. Index 0 is never handed out, so a zero DescriptorHandle means "none"
/// on DX12 as it does on Metal. Slots are released through SubmissionTracker::retire() only (ADR 0030).
/// Each slot has an entry, (kind << 24) | generation, mirrored into the shader-visible validation table; handles
/// carry the entry they were made with in their upper 32 bits, so debug shaders spot freed, reused and
/// wrong-kind handles (ADR 0027).
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
        entries_.assign(capacity, 0);
        generations_.assign(capacity, 0);
        return S_OK;
    }

    /// Where this heap's entries live in the validation table (one u32 per slot).
    void attach(std::uint32_t* table) { table_ = table; }

    /// Keeps slots 1..count for fixed descriptors (the null descriptors); allocate() starts after them.
    void reserve(std::uint32_t count) { next_ = 1 + count; }

    /// A free slot now holding `kind`, or 0 when the heap is full.
    [[nodiscard]] std::uint32_t allocate(SlotKind kind) {
        std::uint32_t index = 0;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else if (next_ < capacity_) {
            index = next_++;
        }
        if (index != 0) {
            mark(index, kind);
        }
        return index;
    }
    /// Records that `index` now holds `kind`, with a new generation.
    void mark(std::uint32_t index, SlotKind kind) {
        std::uint32_t generation = ++generations_[index] & 0xFFFFFFu;
        if (generation == 0) {
            generation = ++generations_[index] & 0xFFFFFFu;  // 0 would read as a free slot
        }
        set_entry(index, (static_cast<std::uint32_t>(kind) << 24) | generation);
    }
    void release(std::uint32_t index) {
        if (index != 0) {
            set_entry(index, 0);
            free_.push_back(index);
        }
    }
    /// The handle shaders use for `index`: the slot, and its current entry for validation.
    [[nodiscard]] DescriptorHandle handle(std::uint32_t index) const {
        return {index == 0 ? 0 : index | (std::uint64_t{entries_[index]} << 32)};
    }

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpu(std::uint32_t index) const {
        return {base_.ptr + std::size_t{index} * increment_};
    }
    [[nodiscard]] ID3D12DescriptorHeap* get() const { return heap_.Get(); }
    [[nodiscard]] std::uint32_t capacity() const { return capacity_; }

private:
    void set_entry(std::uint32_t index, std::uint32_t entry) {
        entries_[index] = entry;
        if (table_ != nullptr) {
            table_[index] = entry;
        }
    }

    ComPtr<ID3D12DescriptorHeap> heap_;
    std::vector<std::uint32_t> entries_;
    std::vector<std::uint32_t> generations_;
    std::uint32_t* table_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE base_{};
    UINT increment_ = 0;
    std::uint32_t capacity_ = 0;
    std::uint32_t next_ = 1;
    std::vector<std::uint32_t> free_;
};

/// Shader-visible heap sizes. 65,536 CBV/SRV/UAV slots fit every tier-3 device; 2,048 is the sampler maximum.
constexpr std::uint32_t view_heap_capacity = 65'536;
constexpr std::uint32_t sampler_heap_capacity = 2'048;
constexpr std::uint32_t null_sampler_slot = 1;  ///< view-heap null descriptors are slots 1..4 (create_null_descriptors)
constexpr std::uint32_t rtv_heap_capacity = 1'024;
constexpr std::uint32_t dsv_heap_capacity = 256;
/// Timestamp queries per frame slot: a start and an end per timed region (VCH-049).
constexpr std::uint32_t timing_queries_per_frame = 4096;

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

/// ADR 0029's mapping of graph states to enhanced-barrier layout, access and sync.
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
    std::uint32_t rtv = 0;  ///< in the CPU-only RTV heap: mip 0, layer 0
    std::uint32_t dsv = 0;  ///< in the CPU-only DSV heap: mip 0, layer 0
    /// Descriptors of parts of the texture (ADR 0090), made on first use and freed with it.
    std::map<std::pair<View, TextureRange>, std::uint32_t> views;
    /// RTVs (color targets) or DSVs (depth targets) of other mips and layers, by (mip, layer).
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> targets;
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
    /// them while earlier frames may still be building from theirs (ADR 0030).
    std::vector<ComPtr<ID3D12Resource>> instances;
    std::vector<void*> instances_mapped;
    std::vector<AccelHandle> blases;
    std::uint32_t max_instances = 0;
    std::uint32_t srv = 0;  ///< TLAS: the acceleration-structure SRV shaders trace against
    bool tlas = false;
    std::string name;
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
    void copy_buffer(BufferHandle src, BufferHandle dst, std::uint64_t size) override;
    void copy_buffer_to_texture(BufferHandle src, std::uint64_t offset, std::uint32_t row_pitch, TextureHandle dst,
                                const TextureRegion& region) override;
    void use(ResourceRef, ResourceState) override {}  // committed resources are always resident
    void barrier(std::span<const Barrier> barriers) override;
    void push_debug_group(std::string_view name) override {
        const std::string text(name);  // PIX_EVENT_ANSI_VERSION (1): a null-terminated ANSI string
        recorder_->list->BeginEvent(1, text.c_str(), static_cast<UINT>(text.size() + 1));
    }
    void pop_debug_group() override { recorder_->list->EndEvent(); }
    void begin_timing(std::string_view name) override;
    void end_timing() override;

    /// Closes the list and hands its recorder back; called once, by Dx12Device::submit.
    std::unique_ptr<Recorder> finish() {
        resolve_timings();
        if (const HRESULT hr = recorder_->list->Close(); FAILED(hr)) {
            log::error("rhi", gpu_error("closing a command list (an invalid command was recorded)", hr).error());
        }
        return std::move(recorder_);
    }

private:
    void encode_accel(AccelHandle handle, bool refit, std::span<const TlasInstance> instances);
    void resolve_timings();

    Dx12Device& device_;
    std::unique_ptr<Recorder> recorder_;
    bool compute_pass_ = false;  ///< root constants go to the graphics or the compute slot
    // Timestamp queries this list wrote (VCH-049): [first, end) of its frame slot's heap, resolved by finish().
    std::uint32_t timing_slot_ = 0;
    std::uint32_t timing_first_ = UINT32_MAX;
    std::uint32_t timing_end_ = 0;
    std::optional<std::uint32_t> timing_end_query_;  ///< the open region's end query
    bool timing_ = false;
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
        check_descriptor_errors();
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
        if (const HRESULT hr = dsv_heap_.init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsv_heap_capacity, false);
            FAILED(hr)) {
            return gpu_error("creating the DSV heap", hr);
        }
        create_timing_queries();
        if (auto created = create_validation_buffers(); !created) {
            return created;
        }
        create_null_descriptors();
        return create_root_signature();
    }

    [[nodiscard]] DescriptorHandle null_descriptor(DescriptorKind kind) override {
        return kind == DescriptorKind::sampler ? sampler_heap_.handle(null_sampler_slot)
                                               : view_heap_.handle(null_view_slot(kind));
    }
    /// View-heap slots 1..4 hold the first four kinds' null descriptors, 5..10 the texture kinds of ADR 0090.
    static std::uint32_t null_view_slot(DescriptorKind kind) {
        const auto k = static_cast<std::uint32_t>(kind);
        return k < static_cast<std::uint32_t>(DescriptorKind::sampler) ? k + 1 : k;
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
            buffer.srv = view_heap_.allocate(SlotKind::buffer);
            buffer.uav = gpu_memory ? view_heap_.allocate(SlotKind::storage_buffer) : 0;
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
        if (auto valid = validate_texture_desc(desc); !valid) {
            return std::unexpected(valid.error());
        }
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
        if (auto sized = validate_mip_data(desc, mips); !sized) {
            return std::unexpected(sized.error());
        }
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension =
            desc.type == TextureType::d3 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = desc.width;
        d.Height = desc.height;
        d.DepthOrArraySize = static_cast<UINT16>(desc.type == TextureType::d3 ? desc.depth : layer_count(desc));
        d.MipLevels = static_cast<UINT16>(levels);
        d.Format = resource_format(desc.format);
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
        const bool depth_target = has(desc.usage, TextureUsage::depth_target);
        texture.srv = sampled ? view_heap_.allocate(SlotKind::texture) : 0;
        texture.uav = storage ? view_heap_.allocate(SlotKind::storage_texture) : 0;
        texture.rtv = target ? rtv_heap_.allocate(SlotKind::none) : 0;
        texture.dsv = depth_target ? dsv_heap_.allocate(SlotKind::none) : 0;
        if (target && texture.rtv == 0) {
            free_descriptors(texture);
            return heap_full("RTV", rtv_heap_.capacity());
        }
        if (depth_target && texture.dsv == 0) {
            free_descriptors(texture);
            return heap_full("DSV", dsv_heap_.capacity());
        }
        if ((sampled && texture.srv == 0) || (storage && texture.uav == 0)) {
            free_descriptors(texture);
            return heap_full("CBV/SRV/UAV", view_heap_.capacity());
        }
        const TextureRange whole{0, levels, 0, layer_count(desc)};
        if (sampled) {
            const D3D12_SHADER_RESOURCE_VIEW_DESC srv = srv_desc(desc, desc.type, whole);
            device_->CreateShaderResourceView(texture.resource.Get(), &srv, view_heap_.cpu(texture.srv));
        }
        if (storage) {
            const D3D12_UNORDERED_ACCESS_VIEW_DESC uav =
                uav_desc(desc, view_type(desc, View::storage, whole), {0, 1, 0, layer_count(desc)});
            device_->CreateUnorderedAccessView(texture.resource.Get(), nullptr, &uav, view_heap_.cpu(texture.uav));
        }
        if (target) {
            const D3D12_RENDER_TARGET_VIEW_DESC rtv = rtv_desc(desc, {});
            device_->CreateRenderTargetView(texture.resource.Get(), &rtv, rtv_heap_.cpu(texture.rtv));
        }
        if (depth_target) {
            const D3D12_DEPTH_STENCIL_VIEW_DESC dsv = dsv_desc(desc, {});
            device_->CreateDepthStencilView(texture.resource.Get(), &dsv, dsv_heap_.cpu(texture.dsv));
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
        if (auto valid = validate_sampler_desc(desc); !valid) {
            return std::unexpected(valid.error());
        }
        const std::uint32_t index = sampler_heap_.allocate(SlotKind::sampler);
        if (index == 0) {
            return heap_full("sampler", sampler_heap_.capacity());
        }
        D3D12_SAMPLER_DESC d{};
        const bool compare = desc.compare.has_value();
        if (desc.max_anisotropy > 1) {
            d.Filter = compare ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
        } else if (desc.filter == Filter::linear) {
            d.Filter = compare ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        } else {
            d.Filter = compare ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
        }
        const D3D12_TEXTURE_ADDRESS_MODE address =
            desc.address == AddressMode::repeat ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        d.AddressU = address;
        d.AddressV = address;
        d.AddressW = address;
        d.MaxAnisotropy = desc.max_anisotropy;
        d.ComparisonFunc = compare ? to_d3d12(*desc.compare) : D3D12_COMPARISON_FUNC_NEVER;
        d.MaxLOD = D3D12_FLOAT32_MAX;
        device_->CreateSampler(&d, sampler_heap_.cpu(index));
        return samplers_.insert({index});
    }

    // ---- pipelines: DXIL from the embedded ShaderLibrary, one global root signature (ADR 0027)

    [[nodiscard]] Result<PipelineHandle> create_graphics_pipeline(const GraphicsPipelineDesc& desc) override {
        if (auto valid = validate_graphics_pipeline_desc(desc); !valid) {
            return std::unexpected(valid.error());
        }
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
            d.BlendState.RenderTarget[i] = blend_desc(desc.blend.at(i));
            ++d.NumRenderTargets;
        }
        d.BlendState.IndependentBlendEnable = TRUE;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = to_d3d12(desc.cull);
        d.RasterizerState.FrontCounterClockwise = TRUE;  // glTF
        d.RasterizerState.DepthClipEnable = TRUE;
        d.RasterizerState.DepthBias = desc.depth_bias.constant;
        d.RasterizerState.SlopeScaledDepthBias = desc.depth_bias.slope;
        d.RasterizerState.DepthBiasClamp = desc.depth_bias.clamp;
        if (desc.depth_format != Format::undefined) {
            d.DSVFormat = to_dxgi(desc.depth_format);
        }
        // DepthEnable covers the test and the write; writing without testing is a test that always passes.
        d.DepthStencilState.DepthEnable = desc.depth_test || desc.depth_write ? TRUE : FALSE;
        d.DepthStencilState.DepthWriteMask =
            desc.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DepthStencilState.DepthFunc = desc.depth_test ? to_d3d12(desc.depth_compare) : D3D12_COMPARISON_FUNC_ALWAYS;
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

    // ---- bindless descriptors (ADR 0027): heap indices

    [[nodiscard]] DescriptorHandle descriptor(BufferHandle h, View view) override {
        const Dx12Buffer* b = buffers_.get(h);
        return view_heap_.handle(b ? (view == View::storage ? b->uav : b->srv) : 0u);
    }
    [[nodiscard]] DescriptorHandle descriptor(TextureHandle h, View view) override {
        const Dx12Texture* t = textures_.get(h);
        return view_heap_.handle(t ? (view == View::storage ? t->uav : t->srv) : 0u);
    }
    [[nodiscard]] DescriptorHandle descriptor(TextureHandle h, View view, const TextureRange& requested) override {
        Dx12Texture* t = textures_.get(h);
        if (t == nullptr) {
            return {};
        }
        auto range = resolve_range(t->desc, view, requested);
        if (!range) {
            log::error("rhi", range.error());
            return {};
        }
        const bool storage = view == View::storage;
        const std::uint32_t whole_mips = storage ? 1 : std::max(1u, t->desc.mip_levels);
        if (*range == TextureRange{0, whole_mips, 0, layer_count(t->desc)}) {
            return descriptor(h, view);
        }
        const auto key = std::make_pair(view, *range);
        auto it = t->views.find(key);
        if (it == t->views.end()) {
            const bool allowed = has(t->desc.usage, storage ? TextureUsage::storage : TextureUsage::sampled);
            const std::uint32_t slot =
                allowed ? view_heap_.allocate(storage ? SlotKind::storage_texture : SlotKind::texture) : 0;
            if (slot == 0) {
                log::error("rhi", allowed ? heap_full("CBV/SRV/UAV", view_heap_.capacity()).error()
                                          : Error{ErrorCode::rhi_invalid_texture,
                                                  std::format("'{}' has no {} usage for a view", t->desc.debug_name,
                                                              storage ? "storage" : "sampled"),
                                                  "Create the texture with that TextureUsage."});
                return {};
            }
            const TextureType type = view_type(t->desc, view, *range);
            if (storage) {
                const D3D12_UNORDERED_ACCESS_VIEW_DESC uav = uav_desc(t->desc, type, *range);
                device_->CreateUnorderedAccessView(t->resource.Get(), nullptr, &uav, view_heap_.cpu(slot));
            } else {
                const D3D12_SHADER_RESOURCE_VIEW_DESC srv = srv_desc(t->desc, type, *range);
                device_->CreateShaderResourceView(t->resource.Get(), &srv, view_heap_.cpu(slot));
            }
            it = t->views.emplace(key, slot).first;
        }
        return view_heap_.handle(it->second);
    }
    [[nodiscard]] DescriptorHandle descriptor(SamplerHandle h) override {
        const Dx12Sampler* s = samplers_.get(h);
        return sampler_heap_.handle(s ? s->index : 0u);
    }
    [[nodiscard]] DescriptorHandle descriptor(AccelHandle h) override {
        const Dx12Accel* a = accels_.get(h);
        return view_heap_.handle(a ? a->srv : 0u);
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
        const D3D12_GPU_VIRTUAL_ADDRESS table = validation_table_->GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS errors = validation_errors_->GetGPUVirtualAddress();
        recorder->list->SetGraphicsRootShaderResourceView(1, table);
        recorder->list->SetGraphicsRootUnorderedAccessView(2, errors);
        recorder->list->SetComputeRootShaderResourceView(1, table);
        recorder->list->SetComputeRootUnorderedAccessView(2, errors);
        set_name(recorder->list.Get(), debug_name);  // DRED reports lists by name
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
            // A GPU that never finishes (a shader stuck in a loop, WARP included) would wait here without a word, and a
            // test would time out with no output (VCH-110): say so every 10 s, and stop if the device was removed.
            unsigned waited_s = 0;
            while (WaitForSingleObject(event_, 10000) == WAIT_TIMEOUT) {
                waited_s += 10;
                if (FAILED(device_->GetDeviceRemovedReason())) {
                    break;  // poll() reports it
                }
                log::warn("rhi", "still waiting for the GPU after {} s: submission {}, last completed {}", waited_s,
                          submission.value, fence_->GetCompletedValue());
            }
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
        collect_timings();  // this slot's previous frame has completed: resolve it before reuse
        timings_.begin_frame(frame.slot, frame.index);
        return frame;
    }

    void end_frame() override {
        tracker_.end_frame();
        timings_.end_frame(tracker_.current_frame().slot, tracker_.last_submitted().value);
    }
    [[nodiscard]] FrameInfo current_frame() const override { return tracker_.current_frame(); }
    [[nodiscard]] std::uint32_t frames_in_flight() const override { return tracker_.frames_in_flight(); }
    /// The timed part of the frame: the first render-graph pass's start to the last one's end.
    [[nodiscard]] double last_frame_gpu_ms() const override { return timings_.published_span_ms(); }

    [[nodiscard]] MemoryUsage memory_usage() override {
        const auto bytes = [&](ID3D12Resource* resource) -> std::uint64_t {
            if (resource == nullptr) {
                return 0;
            }
            const D3D12_RESOURCE_DESC desc = resource->GetDesc();
            return device_->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        };
        MemoryUsage usage;
        buffers_.for_each(
            [&](const Dx12Buffer& b) { usage.resources.push_back({b.desc.debug_name, bytes(b.resource.Get())}); });
        textures_.for_each(
            [&](const Dx12Texture& t) { usage.resources.push_back({t.desc.debug_name, bytes(t.resource.Get())}); });
        accels_.for_each([&](const Dx12Accel& a) {
            std::uint64_t total = bytes(a.buffer.Get()) + bytes(a.scratch.Get());
            for (const ComPtr<ID3D12Resource>& instances : a.instances) {
                total += bytes(instances.Get());
            }
            usage.resources.push_back({a.name, total});
        });
        return usage;
    }

    [[nodiscard]] FrameTimings last_frame_timings() override {
        collect_timings();
        FrameTimings out = timings_.published();
        out.unavailable = timing_unavailable_;
        return out;
    }

    // Used by Dx12CommandList's timing regions.
    [[nodiscard]] FrameTimingSlots& timing_slots() { return timings_; }
    [[nodiscard]] ID3D12QueryHeap* timing_heap(std::uint32_t slot) const {
        return timing_heaps_.empty() ? nullptr : timing_heaps_.at(slot % timing_heaps_.size()).Get();
    }
    [[nodiscard]] ID3D12Resource* timing_readback(std::uint32_t slot) const {
        return timing_readbacks_.empty() ? nullptr : timing_readbacks_.at(slot % timing_readbacks_.size()).Get();
    }

    /// A list that was never submitted (an error path): nothing of it reached the GPU.
    void give_back(std::unique_ptr<Recorder> recorder) { recorders_.push_back(std::move(recorder)); }

    // Used by Dx12CommandList.
    Dx12Buffer* buffer(BufferHandle h) { return buffers_.get(h); }
    Dx12Texture* texture(TextureHandle h) { return textures_.get(h); }
    Dx12Pipeline* pipeline(PipelineHandle h) { return pipelines_.get(h); }
    Dx12Accel* accel(AccelHandle h) { return accels_.get(h); }
    [[nodiscard]] std::uint32_t frame_slot() const { return tracker_.current_frame().slot; }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE rtv(std::uint32_t index) const { return rtv_heap_.cpu(index); }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE dsv(std::uint32_t index) const { return dsv_heap_.cpu(index); }
    /// The RTV or DSV that draws into `slice` of `t`, made on first use (0: the heap is full).
    D3D12_CPU_DESCRIPTOR_HANDLE target_view(Dx12Texture& t, TargetSlice slice, bool depth) {
        if (slice.mip == 0 && slice.layer == 0) {
            return depth ? dsv(t.dsv) : rtv(t.rtv);
        }
        DescriptorHeap& heap = depth ? dsv_heap_ : rtv_heap_;
        const auto key = std::make_pair(slice.mip, slice.layer);
        auto it = t.targets.find(key);
        if (it == t.targets.end()) {
            const std::uint32_t index = heap.allocate(SlotKind::none);
            if (index == 0) {
                log::error("rhi", heap_full(depth ? "DSV" : "RTV", heap.capacity()).error());
                return heap.cpu(depth ? t.dsv : t.rtv);
            }
            if (depth) {
                const D3D12_DEPTH_STENCIL_VIEW_DESC d = dsv_desc(t.desc, slice);
                device_->CreateDepthStencilView(t.resource.Get(), &d, heap.cpu(index));
            } else {
                const D3D12_RENDER_TARGET_VIEW_DESC d = rtv_desc(t.desc, slice);
                device_->CreateRenderTargetView(t.resource.Get(), &d, heap.cpu(index));
            }
            it = t.targets.emplace(key, index).first;
        }
        return heap.cpu(it->second);
    }
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
        const auto levels = static_cast<UINT>(mips.size());
        const UINT layers = layer_count(desc);
        const UINT count = levels * layers;  // subresource i: mip i % levels of layer i / levels
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
        for (UINT i = 0; i < count; ++i) {
            const UINT level = i % levels;
            const UINT layer = i / levels;
            const std::uint64_t packed = row_bytes(desc.format, mip_size(desc.width, level));
            const UINT slices = footprints[i].Footprint.Depth;
            // KTX2 order in `mips[level]`: layer, then depth slice, then rows.
            const std::byte* source = mips[level].data() + std::uint64_t{layer} * packed * rows[i] * slices;
            for (UINT row = 0; row < rows[i] * slices; ++row) {
                std::memcpy(static_cast<std::byte*>(mapped) + footprints[i].Offset +
                                std::uint64_t{row} * footprints[i].Footprint.RowPitch,
                            source + row * packed, packed);
            }
        }
        std::unique_ptr<Recorder> recorder = take_recorder();
        if (!recorder) {
            return make_error(ErrorCode::rhi_gpu_error, "D3D12: no command list for a texture upload");
        }
        ID3D12GraphicsCommandList7* list = recorder->list.Get();
        set_name(list, desc.debug_name + " (upload)");
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
        for (UINT i = 0; i < count; ++i) {
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = texture;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = i;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = staging->Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint = footprints[i];
            // Block-compressed footprints are whole blocks, and so are the mips of a texture whose size is a
            // multiple of 4 (validate_texture_desc): a 2x2 mip copies as 4x4.
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
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
        dsv_heap_.release(t.dsv);
        for (const auto& [key, slot] : t.views) {
            view_heap_.release(slot);
        }
        for (const auto& [key, slot] : t.targets) {
            (has(t.desc.usage, TextureUsage::depth_target) ? dsv_heap_ : rtv_heap_).release(slot);
        }
    }
    void free_descriptors(const Dx12Sampler& s) { sampler_heap_.release(s.index); }
    void free_descriptors(const Dx12Pipeline& /*pipeline*/) const {}
    void free_descriptors(const Dx12Accel& a) { view_heap_.release(a.srv); }

    Result<AccelHandle> make_accel(Dx12Accel accel, bool allow_refit, std::string_view name) {
        if (!caps_.hardware_ray_tracing) {
            return make_error(ErrorCode::rhi_backend_unavailable,
                              std::format("'{}' does not support DXR 1.1", caps_.device_name),
                              "Acceleration structures need DXR 1.1 (ray tracing tier 1.1); WARP from the Agility "
                              "SDK has it (ADR 0033).");
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
        accel.name = name;
        accel.inputs.pGeometryDescs = nullptr;  // points into `accel`, which moves; set again at build time
        if (accel.tlas) {
            accel.srv = view_heap_.allocate(SlotKind::accel);
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

    /// 64 root-constant DWORDs at b0 (the Slang parameter block) and both heaps directly indexed (ADR 0027).
    /// Null views read as zero (D3D12 defines reads of a null descriptor of the shader's type). Slots 1..4 of the
    /// view heap, in DescriptorKind order, and slot 1 of the sampler heap (a real sampler: there are no null ones).
    void create_null_descriptors() {
        view_heap_.reserve(10);
        D3D12_SHADER_RESOURCE_VIEW_DESC texture{};
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        texture.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        texture.Texture2D.MipLevels = 1;
        view_heap_.mark(1, SlotKind::texture);
        view_heap_.mark(2, SlotKind::storage_texture);
        view_heap_.mark(3, SlotKind::buffer);
        view_heap_.mark(4, SlotKind::storage_buffer);
        device_->CreateShaderResourceView(nullptr, &texture, view_heap_.cpu(1));
        D3D12_UNORDERED_ACCESS_VIEW_DESC storage_texture{};
        storage_texture.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        storage_texture.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(nullptr, nullptr, &storage_texture, view_heap_.cpu(2));
        D3D12_SHADER_RESOURCE_VIEW_DESC buffer{};
        buffer.Format = DXGI_FORMAT_UNKNOWN;
        buffer.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        buffer.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        buffer.Buffer.NumElements = 1;
        buffer.Buffer.StructureByteStride = 4;
        device_->CreateShaderResourceView(nullptr, &buffer, view_heap_.cpu(3));
        D3D12_UNORDERED_ACCESS_VIEW_DESC storage_buffer{};
        storage_buffer.Format = DXGI_FORMAT_UNKNOWN;
        storage_buffer.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        storage_buffer.Buffer.NumElements = 1;
        storage_buffer.Buffer.StructureByteStride = 4;
        device_->CreateUnorderedAccessView(nullptr, nullptr, &storage_buffer, view_heap_.cpu(4));
        // ADR 0090: one null descriptor per texture type, so an absent volume or cube map reads zero.
        const TextureDesc one{1, 1, Format::rgba8_unorm, TextureUsage::sampled, "null"};
        const TextureRange all{0, 1, 0, 1};
        const auto null_srv = [&](DescriptorKind kind, TextureType type, std::uint32_t layers) {
            TextureRange range = all;
            range.layer_count = layers;
            const std::uint32_t slot = null_view_slot(kind);
            view_heap_.mark(slot, SlotKind::texture);
            const D3D12_SHADER_RESOURCE_VIEW_DESC srv = srv_desc(one, type, range);
            device_->CreateShaderResourceView(nullptr, &srv, view_heap_.cpu(slot));
        };
        null_srv(DescriptorKind::texture_2d_array, TextureType::d2_array, 1);
        null_srv(DescriptorKind::texture_3d, TextureType::d3, 1);
        null_srv(DescriptorKind::texture_cube, TextureType::cube, 6);
        null_srv(DescriptorKind::texture_cube_array, TextureType::cube_array, 6);
        TextureDesc storage_one = one;
        storage_one.format = Format::rgba32_float;
        for (const auto& [kind, type] : {std::pair{DescriptorKind::storage_texture_2d_array, TextureType::d2_array},
                                         std::pair{DescriptorKind::storage_texture_3d, TextureType::d3}}) {
            const std::uint32_t slot = null_view_slot(kind);
            view_heap_.mark(slot, SlotKind::storage_texture);
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = uav_desc(storage_one, type, all);
            if (type == TextureType::d3) {
                uav.Texture3D.WSize = 1;
            }
            device_->CreateUnorderedAccessView(nullptr, nullptr, &uav, view_heap_.cpu(slot));
        }
        sampler_heap_.reserve(1);
        sampler_heap_.mark(null_sampler_slot, SlotKind::sampler);
        D3D12_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        device_->CreateSampler(&sampler, sampler_heap_.cpu(null_sampler_slot));
    }

    Result<> create_root_signature() {
        // b0: the parameter block; t30 and u31: the descriptor validation table and error record (ADR 0027).
        std::array<D3D12_ROOT_PARAMETER1, 3> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.ShaderRegister = 0;
        parameters[0].Constants.Num32BitValues = static_cast<UINT>(max_parameter_bytes / 4);
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[1].Descriptor.ShaderRegister = 30;
        parameters[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[2].Descriptor.ShaderRegister = 31;
        parameters[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        for (D3D12_ROOT_PARAMETER1& p : parameters) {
            p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
        desc.Desc_1_1.pParameters = parameters.data();
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
                              "Compile it with veach_add_shader() on Windows (docs/adr/0009-shader-compilation.md).");
        }
        return e->dxil;
    }

    /// The handle dies now; the D3D12 object and its descriptor slots once the GPU is done with them (ADR 0030).
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
            image.rtv = rtv_heap_.allocate(SlotKind::none);
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
        check_descriptor_errors();
    }

    /// Reports handles that debug shaders found invalid since the last check (veach/bindless.slang, ADR 0027).
    void check_descriptor_errors() {
        if (validation_errors_data_ == nullptr || validation_errors_data_[0] == validation_errors_seen_) {
            return;
        }
        const std::uint32_t count = validation_errors_data_[0];
        const std::uint32_t slot = validation_errors_data_[2];
        const std::uint32_t entry = validation_errors_data_[3];
        const std::uint32_t kind = validation_errors_data_[4];
        static constexpr std::array<std::string_view, 7> names{
            "unknown", "texture", "storage texture", "buffer", "storage buffer", "sampler", "acceleration structure"};
        const auto name = [](std::uint32_t k) {
            return names.at(k < names.size() ? k : 0);
        };
        const std::uint32_t capacity = kind == 5 ? sampler_heap_capacity : view_heap_capacity;
        const std::uint32_t current =
            slot < capacity ? validation_table_data_[16 + (kind == 5 ? view_heap_capacity : 0) + slot] : 0;
        const std::string holds = slot == 0                 ? "handle 0 (never set)"
                                  : slot >= capacity        ? "a slot past the end of the heap"
                                  : current == 0            ? "a free slot (its resource was destroyed)"
                                  : (current >> 24) != kind ? std::format("a {}", name(current >> 24))
                                                            : "a newer resource (the handle is stale)";
        log::error("rhi", Error{ErrorCode::rhi_invalid_descriptor,
                                std::format("{} shader descriptor loads used an invalid handle; the first wanted a {} "
                                            "at slot {} (generation {}) and found {}, by frame {}",
                                            count - validation_errors_seen_, name(kind), slot, entry & 0xFFFFFFu, holds,
                                            tracker_.current_frame().index),
                                "Set every handle a shader reads, keep resources alive while frames use them, and "
                                "use device.null_descriptor(kind) for \"no resource\" (ADR 0027)."});
        validation_errors_seen_ = count;
    }

    /// ADR 0027. The table is in an upload heap (a root SRV may point there); the error record in a CPU-readable
    /// custom heap, so the shader's atomics land where the CPU reads them without a copy.
    Result<> create_validation_buffers() {
        const std::uint64_t entries = 16 + std::uint64_t{view_heap_capacity} + sampler_heap_capacity;
        auto table = create_resource(D3D12_HEAP_TYPE_UPLOAD, buffer_resource(entries * 4, D3D12_RESOURCE_FLAG_NONE),
                                     D3D12_BARRIER_LAYOUT_UNDEFINED, "descriptor validation table");
        if (!table) {
            return std::unexpected(table.error());
        }
        validation_table_ = std::move(*table);
        void* mapped = nullptr;
        const D3D12_RANGE nothing_read{0, 0};
        if (const HRESULT hr = validation_table_->Map(0, &nothing_read, &mapped); FAILED(hr)) {
            return gpu_error("mapping the descriptor validation table", hr);
        }
        validation_table_data_ = static_cast<std::uint32_t*>(mapped);
        std::memset(validation_table_data_, 0, entries * 4);
        validation_table_data_[0] = view_heap_capacity;
        validation_table_data_[1] = sampler_heap_capacity;
        view_heap_.attach(validation_table_data_ + 16);
        sampler_heap_.attach(validation_table_data_ + 16 + view_heap_capacity);

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_CUSTOM;
        heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        const D3D12_RESOURCE_DESC1 desc = buffer_resource(64, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (const HRESULT hr =
                device_->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED,
                                                  nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&validation_errors_));
            FAILED(hr)) {
            return gpu_error("creating the descriptor validation record", hr);
        }
        set_name(validation_errors_.Get(), "descriptor validation errors");
        if (const HRESULT hr = validation_errors_->Map(0, nullptr, &mapped); FAILED(hr)) {
            return gpu_error("mapping the descriptor validation record", hr);
        }
        std::memset(mapped, 0, 64);
        validation_errors_data_ = static_cast<volatile std::uint32_t*>(mapped);
        return {};
    }

    /// Without them, timings are unavailable (and say why); nothing else depends on them.
    void create_timing_queries() {
        if (const HRESULT hr = queue_->GetTimestampFrequency(&timestamp_frequency_);
            FAILED(hr) || timestamp_frequency_ == 0) {
            timing_unavailable_ = std::format("the direct queue has no timestamp frequency ({})", hresult(hr));
            return;
        }
        for (std::uint32_t slot = 0; slot < tracker_.frames_in_flight(); ++slot) {
            D3D12_QUERY_HEAP_DESC d{};
            d.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            d.Count = timing_queries_per_frame;
            ComPtr<ID3D12QueryHeap> heap;
            if (const HRESULT hr = device_->CreateQueryHeap(&d, IID_PPV_ARGS(&heap)); FAILED(hr)) {
                timing_unavailable_ = std::format("CreateQueryHeap failed ({})", hresult(hr));
                break;
            }
            set_name(heap.Get(), "pass timings");
            auto readback =
                create_resource(D3D12_HEAP_TYPE_READBACK,
                                buffer_resource(std::uint64_t{timing_queries_per_frame} * sizeof(std::uint64_t),
                                                D3D12_RESOURCE_FLAG_NONE),
                                D3D12_BARRIER_LAYOUT_UNDEFINED, "pass timings readback");
            void* mapped = nullptr;
            if (!readback || FAILED((*readback)->Map(0, nullptr, &mapped))) {
                timing_unavailable_ = "cannot create the timestamp readback buffer";
                break;
            }
            timing_heaps_.push_back(std::move(heap));
            timing_readbacks_.push_back(std::move(*readback));
            timing_mapped_.push_back(static_cast<const std::uint64_t*>(mapped));
        }
        if (!timing_unavailable_.empty()) {
            timing_heaps_.clear();
            timing_readbacks_.clear();
            timing_mapped_.clear();
        }
    }

    /// Resolves the timings of frames that have completed (never waits).
    void collect_timings() {
        if (timing_mapped_.empty()) {
            return;
        }
        const double ms_per_tick = 1000.0 / static_cast<double>(timestamp_frequency_);
        timings_.collect(
            tracker_.completed().value,
            [this](std::uint32_t slot, std::uint32_t count) {
                const std::uint64_t* stamps = timing_mapped_.at(slot);
                return std::vector<std::uint64_t>(stamps, stamps + count);
            },
            [ms_per_tick](std::uint64_t ticks) { return static_cast<double>(ticks) * ms_per_tick; });
    }

    void report_device_removed() {
        if (device_removed_reported_) {
            return;
        }
        device_removed_reported_ = true;
        log::error("rhi", Error{ErrorCode::rhi_gpu_error,
                                std::format("D3D12 device removed ({}){}", hresult(device_->GetDeviceRemovedReason()),
                                            dred_report(device_.Get())),
                                "Run a debug build (D3D12 debug layer and DRED on) for the call that caused it; "
                                "VEACH_DX12_GPU_VALIDATION=1 adds GPU-based validation."});
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
    DescriptorHeap dsv_heap_;
    SlotMap<Dx12Buffer, BufferHandle> buffers_;
    SlotMap<Dx12Texture, TextureHandle> textures_;
    SlotMap<Dx12Sampler, SamplerHandle> samplers_;
    SlotMap<Dx12Pipeline, PipelineHandle> pipelines_;
    SlotMap<Dx12Accel, AccelHandle> accels_;
    std::vector<Dx12Surface> surfaces_;
    ComPtr<ID3D12RootSignature> root_signature_;
    // ADR 0027: the live-descriptor table (header, then one entry per view and sampler slot) and the error record
    // debug shaders write (count, first-recorded flag, slot, handle entry, kind). Bound to every command list.
    ComPtr<ID3D12Resource> validation_table_;
    ComPtr<ID3D12Resource> validation_errors_;
    std::uint32_t* validation_table_data_ = nullptr;
    volatile std::uint32_t* validation_errors_data_ = nullptr;
    std::uint32_t validation_errors_seen_ = 0;
    bool device_removed_reported_ = false;
    // VCH-049: a timestamp query heap and a readback buffer (persistently mapped) per frame slot.
    std::vector<ComPtr<ID3D12QueryHeap>> timing_heaps_;
    std::vector<ComPtr<ID3D12Resource>> timing_readbacks_;
    std::vector<const std::uint64_t*> timing_mapped_;
    UINT64 timestamp_frequency_ = 0;
    FrameTimingSlots timings_{tracker_.frames_in_flight(), timing_queries_per_frame};
    std::string timing_unavailable_;
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
        VEACH_ASSERT(t != nullptr && t->rtv != 0 && valid_target(t->desc, c.slice),
                     "render pass '{}': color attachment {} is not a render target (or has no such mip and layer)",
                     desc.debug_name, i);
        targets.at(i) = device_.target_view(*t, c.slice, false);
        if (c.load == LoadOp::clear) {
            const std::array<float, 4> color =
                is_integer(t->desc.format) ? integer_clear(c.clear_uint, desc.debug_name) : c.clear_color;
            list->ClearRenderTargetView(targets.at(i), color.data(), 0, nullptr);
        }
        width = mip_size(t->desc.width, c.slice.mip);
        height = mip_size(t->desc.height, c.slice.mip);
    }
    std::optional<D3D12_CPU_DESCRIPTOR_HANDLE> depth;
    if (desc.depth.texture.is_valid()) {
        Dx12Texture* t = device_.texture(desc.depth.texture);
        VEACH_ASSERT(t != nullptr && t->dsv != 0 && valid_target(t->desc, desc.depth.slice),
                     "render pass '{}': the depth attachment is not a depth target (or has no such mip and layer)",
                     desc.debug_name);
        depth = device_.target_view(*t, desc.depth.slice, true);
        if (desc.depth.load == LoadOp::clear) {
            list->ClearDepthStencilView(*depth, D3D12_CLEAR_FLAG_DEPTH, desc.depth.clear_depth, 0, 0, nullptr);
        }
        width = mip_size(t->desc.width, desc.depth.slice.mip);
        height = mip_size(t->desc.height, desc.depth.slice.mip);
    }
    list->OMSetRenderTargets(desc.color_count, targets.data(), FALSE, depth ? &*depth : nullptr);
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
            d.Flags = in.opaque ? D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE
                                : D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
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
        // `before` state is always correct, so v0 keeps the contents (ADR 0030, item 5).
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

void Dx12CommandList::copy_buffer(BufferHandle src, BufferHandle dst, std::uint64_t size) {
    Dx12Buffer* from = device_.buffer(src);
    Dx12Buffer* to = device_.buffer(dst);
    VEACH_ASSERT(from != nullptr && to != nullptr, "copy_buffer: invalid handles");
    VEACH_ASSERT(size <= from->desc.size && size <= to->desc.size, "copy_buffer: {} bytes do not fit", size);
    recorder_->list->CopyBufferRegion(to->resource.Get(), 0, from->resource.Get(), 0, size);
}

void Dx12CommandList::copy_buffer_to_texture(BufferHandle src, std::uint64_t offset, std::uint32_t row_pitch,
                                             TextureHandle dst, const TextureRegion& requested) {
    Dx12Buffer* b = device_.buffer(src);
    Dx12Texture* t = device_.texture(dst);
    VEACH_ASSERT(b != nullptr && t != nullptr, "copy_buffer_to_texture: invalid handles");
    auto region = resolve_region(t->desc, requested);
    VEACH_ASSERT(region.has_value(), "copy_buffer_to_texture: {}", region ? std::string() : region.error().message);
    VEACH_ASSERT(valid_copy_layout(t->desc.format, *region, offset, row_pitch, b->desc.size),
                 "copy_buffer_to_texture into '{}': misaligned, or past the end of the buffer", t->desc.debug_name);
    const std::uint64_t stride = texture_copy_layer_stride(t->desc.format, *region, row_pitch);
    // Block-compressed footprints are whole blocks, also where a region ends at a mip edge inside one.
    const std::uint32_t block = is_block_compressed(t->desc.format) ? 4 : 1;
    for (std::uint32_t layer = 0; layer < region->layer_count; ++layer) {
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = t->resource.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.SubresourceIndex = subresource(t->desc, region->mip, region->base_layer + layer);
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = b->resource.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Offset = offset + stride * layer;
        from.PlacedFootprint.Footprint.Format = resource_format(t->desc.format);
        from.PlacedFootprint.Footprint.Width = (region->width + block - 1) / block * block;
        from.PlacedFootprint.Footprint.Height = (region->height + block - 1) / block * block;
        from.PlacedFootprint.Footprint.Depth = region->depth;
        from.PlacedFootprint.Footprint.RowPitch = row_pitch;
        recorder_->list->CopyTextureRegion(&to, region->x, region->y, region->z, &from, nullptr);
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
    to.PlacedFootprint.Footprint.Format = resource_format(t->desc.format);
    // Block-compressed footprints are whole blocks.
    const std::uint32_t block = is_block_compressed(t->desc.format) ? 4 : 1;
    to.PlacedFootprint.Footprint.Width = (t->desc.width + block - 1) / block * block;
    to.PlacedFootprint.Footprint.Height = (t->desc.height + block - 1) / block * block;
    to.PlacedFootprint.Footprint.Depth = 1;
    to.PlacedFootprint.Footprint.RowPitch = pitch;
    recorder_->list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
}

void Dx12CommandList::begin_timing(std::string_view name) {
    VEACH_ASSERT(!timing_, "begin_timing('{}') inside another timing region", name);
    timing_ = true;
    timing_slot_ = device_.frame_slot();
    FrameTimingSlots& slots = device_.timing_slots();
    slots.begin_region(timing_slot_, name);
    ID3D12QueryHeap* heap = device_.timing_heap(timing_slot_);
    if (heap == nullptr) {
        return;
    }
    if (const std::optional<std::uint32_t> first = slots.allocate_pair(timing_slot_)) {
        recorder_->list->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, *first);
        timing_end_query_ = *first + 1;
        timing_first_ = std::min(timing_first_, *first);
        timing_end_ = std::max(timing_end_, *first + 2);
    }
}

void Dx12CommandList::end_timing() {
    VEACH_ASSERT(timing_, "end_timing() without begin_timing()");
    if (timing_end_query_) {
        recorder_->list->EndQuery(device_.timing_heap(timing_slot_), D3D12_QUERY_TYPE_TIMESTAMP, *timing_end_query_);
        timing_end_query_.reset();
    }
    device_.timing_slots().end_region(timing_slot_);
    timing_ = false;
}

/// Copies this list's timestamps to its frame slot's readback buffer, at their query indices.
void Dx12CommandList::resolve_timings() {
    if (timing_) {
        end_timing();  // a region left open: close it so every query in the range was written
    }
    if (timing_first_ < timing_end_) {
        recorder_->list->ResolveQueryData(device_.timing_heap(timing_slot_), D3D12_QUERY_TYPE_TIMESTAMP, timing_first_,
                                          timing_end_ - timing_first_, device_.timing_readback(timing_slot_),
                                          std::uint64_t{timing_first_} * sizeof(std::uint64_t));
    }
}

Dx12CommandList::~Dx12CommandList() {
    if (recorder_) {
        recorder_->list->Close();
        device_.give_back(std::move(recorder_));
    }
}

static_assert(!std::is_abstract_v<Dx12Device>, "Dx12Device must implement the whole rhi::Device interface");

bool env_equals(const char* name, std::string_view expected) {
    std::array<char, 16> value{};
    const DWORD length = GetEnvironmentVariableA(name, value.data(), static_cast<DWORD>(value.size()));
    return length > 0 && length < value.size() && std::string_view(value.data(), length) == expected;
}

/// VEACH_DX12_ADAPTER=warp forces WARP, the software device, even when a GPU is present.
bool warp_forced() {
    return env_equals("VEACH_DX12_ADAPTER", "warp");
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
            ComPtr<ID3D12Debug1> debug1;
            if (env_equals("VEACH_DX12_GPU_VALIDATION", "1") && SUCCEEDED(debug.As(&debug1))) {
                debug1->SetEnableGPUBasedValidation(TRUE);
                log::info("rhi", "D3D12 GPU-based validation enabled (VEACH_DX12_GPU_VALIDATION=1)");
            }
        } else {
            log::warn("rhi", "the D3D12 debug layer is unavailable (D3D12\\d3d12SDKLayers.dll missing?)");
        }
        // DRED: a device removal then names the command that was running and the allocation that faulted.
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
    }
    // The DXGI debug layer (dxgidebug.dll) comes with the Graphics Tools feature, which may be missing.
    ComPtr<IDXGIFactory6> factory;
    if (desc.validation && SUCCEEDED(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory)))) {
        log::debug("rhi", "DXGI debug layer enabled");
    } else if (const HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)); FAILED(hr)) {
        return make_error(ErrorCode::rhi_backend_unavailable, std::format("DXGI is unavailable ({})", hresult(hr)));
    }

    // A hardware adapter that meets the floor, fastest first; otherwise WARP (hosted CI has no GPU, ADR 0033).
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
                          "(ADR 0033); WARP from the Agility SDK provides all three.");
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
