#include <veach/render/scene_renderer.hpp>

#include <veach/asset/asset_server.hpp>
#include <veach/asset/scene.hpp>
#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>
#include <veach/core/suggest.hpp>

#include "atmosphere.hpp"
#include "gpu_scene.hpp"
#include "realtime_renderer.hpp"
#include "scene_records.hpp"
#include "veach_render_shaders.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace veach::render {

namespace {

/// Mirrors `Params` in shaders/path_tracer.slang. Metal rounds it to 16 bytes (a float4x4 inside).
struct alignas(16) PathTraceParams {
    math::mat4 inverse_view_projection;
    math::vec4 camera_position_exposure;
    math::vec4 sky_bounces;
    rhi::DescriptorHandle accumulation;
    rhi::DescriptorHandle scene;
    rhi::DescriptorHandle instances;
    rhi::DescriptorHandle materials;
    rhi::DescriptorHandle lights;
    rhi::DescriptorHandle emissive;
    std::array<std::uint32_t, 2> size;
    std::uint32_t sample_index;
    std::uint32_t samples_per_frame;
    std::uint32_t light_count;
    std::uint32_t emissive_count;
    float emissive_total_weight;
    std::uint32_t scene_flags = 0;
    rhi::DescriptorHandle diagnostics;
    rhi::DescriptorHandle sky;  ///< SkyFrame (ADR 0038)
};
static_assert(rhi::parameters_match<PathTraceParams>(
    veach_render_shaders::layout::path_tracer,
    {VEACH_PARAM(PathTraceParams, inverse_view_projection), VEACH_PARAM(PathTraceParams, camera_position_exposure),
     VEACH_PARAM(PathTraceParams, sky_bounces), VEACH_PARAM(PathTraceParams, accumulation),
     VEACH_PARAM(PathTraceParams, scene), VEACH_PARAM(PathTraceParams, instances),
     VEACH_PARAM(PathTraceParams, materials), VEACH_PARAM(PathTraceParams, lights),
     VEACH_PARAM(PathTraceParams, emissive), VEACH_PARAM(PathTraceParams, size),
     VEACH_PARAM(PathTraceParams, sample_index), VEACH_PARAM(PathTraceParams, samples_per_frame),
     VEACH_PARAM(PathTraceParams, light_count), VEACH_PARAM(PathTraceParams, emissive_count),
     VEACH_PARAM(PathTraceParams, emissive_total_weight), VEACH_PARAM(PathTraceParams, scene_flags),
     VEACH_PARAM(PathTraceParams, diagnostics), VEACH_PARAM(PathTraceParams, sky)}));

float luminance(math::vec3 c) {
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;  // GLM_FORCE_XYZW_ONLY
}

rhi::Format to_rhi(TextureFormat format) {
    switch (format) {
        case TextureFormat::rgba8_unorm: return rhi::Format::rgba8_unorm;
        case TextureFormat::rgba8_srgb: return rhi::Format::rgba8_srgb;
        case TextureFormat::bc7_unorm: return rhi::Format::bc7_unorm;
        case TextureFormat::bc7_srgb: return rhi::Format::bc7_srgb;
        case TextureFormat::rgba32_float: return rhi::Format::rgba32_float;
    }
    return rhi::Format::rgba8_unorm;
}

struct Fnv64 {
    std::uint64_t h = 14695981039346656037ULL;
    void add(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            h = (h ^ bytes[i]) * 1099511628211ULL;
        }
    }
    template <class T>
    void add_all(const std::vector<T>& values) {
        add(values.data(), values.size() * sizeof(T));
    }
};

template <class T>
std::span<const std::byte> bytes_of(const std::vector<T>& values) {
    return std::as_bytes(std::span(values));
}

}  // namespace

struct SceneRenderer::Impl {
    struct GpuMesh {
        const MeshAsset* source = nullptr;  ///< the asset data it was made from; changes on hot reload
        /// A runtime mesh's data (MeshStore), kept alive while its buffers exist; null for cooked meshes.
        std::shared_ptr<const RuntimeMesh> runtime;
        rhi::BufferHandle vertices;
        rhi::BufferHandle indices;
        rhi::BufferHandle slots;
        rhi::BufferHandle meshlets;  ///< ADR 0011: Meshlet records, 8 uints each
        rhi::BufferHandle meshlet_vertices;
        rhi::BufferHandle meshlet_triangles;
        rhi::BufferHandle meshlet_bounds;  ///< MeshletBounds, 3 float4s each
        std::uint32_t meshlet_count = 0;
        math::vec4 sphere{0.0f};  ///< object space, around the bounding box
        /// Per material slot, its meshlets by size bucket (draw_list_of): the draw lists' capacities.
        std::vector<std::array<std::uint32_t, draw_bucket_count>> slot_buckets;
        /// Its buffers' descriptors and bounds, ready to copy into each instance's record.
        InstanceRecord base;
        rhi::AccelHandle blas;
        bool built = false;
    };
    struct GpuTexture {
        const TextureAsset* source = nullptr;
        rhi::TextureHandle texture;
    };

    SceneShaders shaders;
    std::vector<MaterialKind> kinds;
    AssetServer& assets;
    rhi::Device* device = nullptr;
    rhi::PipelineHandle pipeline;
    rhi::PipelineHandle display_pipeline;
    RealtimeRenderer realtime;            ///< ADR 0093
    std::unique_ptr<GpuScene> gpu_scene;  ///< the instances both renderers read
    AtmosphereRenderer atmosphere;        ///< ADR 0038
    bool warned_no_sky = false;
    rhi::SamplerHandle sampler;
    std::map<std::uint32_t, rhi::SamplerHandle> anisotropic;  ///< by max_anisotropy (MaterialAsset)
    std::unordered_map<AssetId, GpuMesh> meshes;
    std::unordered_map<AssetId, GpuTexture> textures;
    std::shared_ptr<const RuntimeAssets> runtime;  ///< this frame's MeshStore snapshot (RenderWorld::runtime_assets)
    rhi::AccelHandle tlas;
    std::vector<AssetId> tlas_meshes;  ///< the BLAS list the TLAS was created with
    std::uint32_t tlas_capacity = 0;
    bool tlas_built = false;
    rhi::TextureHandle accumulation;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool accumulation_written = false;
    std::vector<rhi::BufferHandle> frame_buffers;  ///< last frame's per-frame buffers, destroyed next frame
    std::uint64_t samples = 0;
    std::uint64_t scene_hash = 0;
    bool reset = true;
    /// The shader's counters (diagnostics in path_tracer.slang), copied each frame into that frame slot's readback
    /// buffer and checked when the slot comes round again (or at shutdown).
    struct Readback {
        rhi::BufferHandle buffer;
        std::uint64_t frame = 0;    ///< the frame whose counters it holds; 0 = nothing pending
        std::uint64_t samples = 0;  ///< samples per pixel after that frame
    };
    rhi::BufferHandle diagnostics;
    bool diagnostics_written = false;
    std::vector<Readback> readbacks;
    std::array<std::uint32_t, 2> seen{};  ///< counter values already reported
    std::set<std::string> reported;
    std::optional<Error> unavailable;  ///< why this device has no photo mode; reported when a camera asks

    Impl(SceneShaders lib, std::span<const MaterialKind> k, AssetServer& a)
        : shaders(lib), kinds(k.begin(), k.end()), assets(a) {}

    ~Impl() {
        if (device == nullptr) {
            return;
        }
        device->wait_idle();
        std::vector<Readback*> pending;
        for (Readback& r : readbacks) {
            pending.push_back(&r);
        }
        std::ranges::sort(pending, {}, &Readback::frame);
        for (Readback* r : pending) {
            check(*r);
        }
        for (Readback& r : readbacks) {
            destroy_if(r.buffer);
        }
        destroy_if(diagnostics);
        release_frame_buffers();
        for (auto& [id, mesh] : meshes) {
            destroy(mesh);
        }
        for (auto& [id, texture] : textures) {
            device->destroy(texture.texture);
        }
        destroy_if(tlas);
        destroy_if(accumulation);
        destroy_if(pipeline);
        destroy_if(display_pipeline);
        for (auto& [level, s] : anisotropic) {
            if (s != sampler) {
                destroy_if(s);
            }
        }
        destroy_if(sampler);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    template <class H>
    void destroy_if(H& h) {
        if (h.is_valid()) {
            device->destroy(h);
            h = {};
        }
    }
    void destroy(GpuMesh& m) {
        destroy_if(m.vertices);
        destroy_if(m.indices);
        destroy_if(m.slots);
        destroy_if(m.meshlets);
        destroy_if(m.meshlet_vertices);
        destroy_if(m.meshlet_triangles);
        destroy_if(m.meshlet_bounds);
        destroy_if(m.blas);
    }
    /// Reports what the counters in `r` added since the last check. The frame it holds must have completed.
    void check(Readback& r) {
        if (r.frame == 0) {
            return;
        }
        const auto* counters = static_cast<const std::uint32_t*>(device->map(r.buffer));
        if (counters != nullptr) {
            const std::uint32_t lost = counters[0] - seen[0];
            const std::uint32_t nonfinite = counters[1] - seen[1];
            seen = {counters[0], counters[1]};
            if (lost != 0) {
                log::error("render",
                           Error{ErrorCode::render_history_lost,
                                 std::format("{} path-traced pixels read back something other than the accumulation "
                                             "they wrote (frame {}, {} samples per pixel)",
                                             lost, r.frame, r.samples),
                                 "The accumulation memory was lost or overwritten between frames. Rerun with "
                                 "MTL_SHADER_VALIDATION=1 or take a GPU capture of that frame."});
            }
            if (nonfinite != 0) {
                log::error("render",
                           Error{ErrorCode::render_nonfinite_samples,
                                 std::format("{} path-traced samples were NaN or infinite and were left out (frame {})",
                                             nonfinite, r.frame),
                                 "A material, a light (a NaN intensity?) or the path tracer divided by zero."});
            }
        }
        r.frame = 0;
    }

    void release_frame_buffers() {
        for (rhi::BufferHandle& b : frame_buffers) {
            device->destroy(b);
        }
        frame_buffers.clear();
    }

    void report(const std::string& key, const Error& error) {
        if (reported.insert(key).second) {
            log::error("render", error);
        }
    }

    /// A runtime mesh or material in this frame's snapshot, or null.
    [[nodiscard]] std::shared_ptr<const RuntimeMesh> runtime_mesh(AssetId id) const {
        if (!runtime) {
            return nullptr;
        }
        const auto it = runtime->meshes.find(id);
        return it == runtime->meshes.end() ? nullptr : it->second;
    }
    [[nodiscard]] const MaterialAsset* runtime_material(AssetId id) const {
        if (!runtime) {
            return nullptr;
        }
        const auto it = runtime->materials.find(id);
        return it == runtime->materials.end() ? nullptr : it->second.get();
    }

    /// Frees the GPU data of runtime meshes that were removed or replaced since the last snapshot (ADR 0072). The
    /// buffers are retired, not destroyed at once: frames still in flight may read them (ADR 0030).
    void sweep(std::shared_ptr<const RuntimeAssets> now) {
        if (now == runtime) {
            return;  // the store has not changed
        }
        runtime = std::move(now);
        for (auto it = meshes.begin(); it != meshes.end();) {
            GpuMesh& gpu = it->second;
            if (gpu.runtime == nullptr || runtime_mesh(it->first) == gpu.runtime) {
                ++it;
                continue;
            }
            destroy(gpu);
            if (std::ranges::find(tlas_meshes, it->first) != tlas_meshes.end()) {
                tlas_meshes.clear();  // the TLAS names this BLAS: recreate it
            }
            it = meshes.erase(it);
        }
        for (auto it = textures.begin(); it != textures.end();) {
            const bool removed = is_runtime_id(it->first) && !runtime->textures.contains(it->first);
            if (!removed) {
                ++it;
                continue;
            }
            if (it->second.texture.is_valid()) {
                device->destroy(it->second.texture);
            }
            it = textures.erase(it);
        }
    }

    GpuMesh* mesh(AssetId id) {
        const bool made_at_run_time = is_runtime_id(id);
        std::shared_ptr<const RuntimeMesh> made = made_at_run_time ? runtime_mesh(id) : nullptr;
        const MeshAsset* source = made ? &made->mesh : made_at_run_time ? nullptr : assets.get(Handle<MeshAsset>{id});
        if (source == nullptr || source->indices.empty()) {
            return nullptr;
        }
        GpuMesh& gpu = meshes[id];
        if (gpu.source == source) {
            return &gpu;
        }
        destroy(gpu);  // new, reloaded or (a runtime mesh) updated: the old buffers are retired
        gpu.source = source;
        gpu.runtime = std::move(made);
        gpu.built = false;
        // Acceleration-structure input only where photo mode can build one (ADR 0041).
        const rhi::BufferUsage usage =
            unavailable ? rhi::BufferUsage::storage : rhi::BufferUsage::storage | rhi::BufferUsage::accel_input;
        // Runtime meshes have their own names, so memory_usage() (MCP frame_budget) tells them apart.
        const bool is_runtime = gpu.runtime != nullptr;
        auto vertices = device->create_buffer({.size = source->vertices.size() * sizeof(MeshVertex),
                                               .usage = usage,
                                               .debug_name = is_runtime ? "runtime mesh vertices" : "mesh vertices",
                                               .stride = 4},
                                              bytes_of(source->vertices));
        auto indices = device->create_buffer({.size = source->indices.size() * 4,
                                              .usage = usage,
                                              .debug_name = is_runtime ? "runtime mesh indices" : "mesh indices",
                                              .stride = 4},
                                             bytes_of(source->indices));
        auto slots = device->create_buffer({.size = source->triangle_slots.size() * 4,
                                            .debug_name = is_runtime ? "runtime mesh slots" : "mesh slots",
                                            .stride = 4},
                                           bytes_of(source->triangle_slots));
        // Never empty buffers: a mesh without meshlets (an old cook) still gets valid handles.
        const auto or_one = [](std::size_t size) {
            return std::max<std::size_t>(size, 16);
        };
        auto meshlets = device->create_buffer({.size = or_one(source->meshlets.size() * sizeof(Meshlet)),
                                               .debug_name = is_runtime ? "runtime meshlets" : "meshlets",
                                               .stride = 4},
                                              bytes_of(source->meshlets));
        auto meshlet_vertices =
            device->create_buffer({.size = or_one(source->meshlet_vertices.size() * 4),
                                   .debug_name = is_runtime ? "runtime meshlet vertices" : "meshlet vertices",
                                   .stride = 4},
                                  bytes_of(source->meshlet_vertices));
        auto meshlet_triangles =
            device->create_buffer({.size = or_one(source->meshlet_triangles.size() * 4),
                                   .debug_name = is_runtime ? "runtime meshlet triangles" : "meshlet triangles",
                                   .stride = 4},
                                  bytes_of(source->meshlet_triangles));
        auto meshlet_bounds =
            device->create_buffer({.size = or_one(source->meshlet_bounds.size() * sizeof(MeshletBounds)),
                                   .debug_name = is_runtime ? "runtime meshlet bounds" : "meshlet bounds",
                                   .stride = sizeof(math::vec4)},
                                  bytes_of(source->meshlet_bounds));
        if (!vertices || !indices || !slots || !meshlets || !meshlet_vertices || !meshlet_triangles ||
            !meshlet_bounds) {
            report("mesh buffers", Error{ErrorCode::rhi_gpu_error, "scene renderer: cannot create mesh buffers", {}});
            for (auto* created :
                 {&vertices, &indices, &slots, &meshlets, &meshlet_vertices, &meshlet_triangles, &meshlet_bounds}) {
                if (*created) {
                    device->destroy(**created);
                }
            }
            gpu.source = nullptr;  // try again next frame rather than draw with no buffers
            gpu.runtime.reset();
            return nullptr;
        }
        gpu.vertices = *vertices;
        gpu.indices = *indices;
        gpu.slots = *slots;
        gpu.meshlets = *meshlets;
        gpu.meshlet_vertices = *meshlet_vertices;
        gpu.meshlet_triangles = *meshlet_triangles;
        gpu.meshlet_bounds = *meshlet_bounds;
        // A mesh cooked before meshlet bounds were (or with fewer of them) draws nothing rather than read past them.
        gpu.meshlet_count = source->meshlet_bounds.size() == source->meshlets.size()
                                ? static_cast<std::uint32_t>(source->meshlets.size())
                                : 0;
        const math::vec3 center = 0.5f * (source->bounds_min + source->bounds_max);
        gpu.sphere = math::vec4(center, 0.5f * math::length(source->bounds_max - source->bounds_min));
        gpu.slot_buckets.assign(source->slot_count, {});
        for (std::uint32_t m = 0; m < gpu.meshlet_count; ++m) {
            const Meshlet& meshlet = source->meshlets[m];
            if (meshlet.slot < gpu.slot_buckets.size()) {
                ++gpu.slot_buckets[meshlet.slot].at(draw_list_of(false, meshlet.triangle_count));
            }
        }
        gpu.base = {};
        gpu.base.vertices = device->descriptor(gpu.vertices);
        gpu.base.indices = device->descriptor(gpu.indices);
        gpu.base.slots = device->descriptor(gpu.slots);
        gpu.base.meshlets = device->descriptor(gpu.meshlets);
        gpu.base.meshlet_vertices = device->descriptor(gpu.meshlet_vertices);
        gpu.base.meshlet_triangles = device->descriptor(gpu.meshlet_triangles);
        gpu.base.meshlet_bounds = device->descriptor(gpu.meshlet_bounds);
        gpu.base.meshlet_count = gpu.meshlet_count;
        gpu.base.sphere = gpu.sphere;
        tlas_meshes.clear();  // a photo-mode TLAS must be recreated with the new mesh's BLAS
        return &gpu;
    }

    /// The mesh's BLAS, created the first time photo mode needs it (ADR 0041).
    bool ensure_blas(GpuMesh& gpu) {
        if (gpu.blas.is_valid()) {
            return true;
        }
        auto blas =
            device->create_blas({.vertices = gpu.vertices,
                                 .vertex_count = static_cast<std::uint32_t>(gpu.source->vertices.size()),
                                 .vertex_stride = sizeof(MeshVertex),
                                 .indices = gpu.indices,
                                 .index_count = static_cast<std::uint32_t>(gpu.source->indices.size()),
                                 .opaque = true,  // alpha: rays continue past transparent texels (path_tracer.slang)
                                 .debug_name = gpu.runtime ? "runtime mesh BLAS" : "mesh BLAS"});
        if (!blas) {
            report("blas", blas.error());
            return false;
        }
        gpu.blas = *blas;
        gpu.built = false;
        return true;
    }

    /// The texture's handle, or the null texture's when there is none (ADR 0027): a handle in a record is never left
    /// unwritten, since a compiler may load the descriptor before the shader tests the material's flags.
    rhi::DescriptorHandle texture(const std::optional<AssetId>& id, bool& present) {
        present = false;
        if (!id) {
            return device->null_descriptor(rhi::DescriptorKind::texture);
        }
        const TextureAsset* source = nullptr;
        if (is_runtime_id(*id)) {
            if (runtime) {
                const auto it = runtime->textures.find(*id);
                source = it == runtime->textures.end() ? nullptr : it->second.get();
            }
        } else {
            source = assets.get(Handle<TextureAsset>{*id});
        }
        if (source == nullptr) {
            return device->null_descriptor(rhi::DescriptorKind::texture);
        }
        GpuTexture& gpu = textures[*id];
        if (gpu.source != source) {
            if (gpu.texture.is_valid()) {
                device->destroy(gpu.texture);
            }
            std::vector<std::span<const std::byte>> mips;
            for (const auto& mip : source->mips) {
                mips.emplace_back(mip);
            }
            rhi::TextureDesc desc{source->width,
                                  source->height,
                                  to_rhi(source->format),
                                  rhi::TextureUsage::sampled,
                                  is_runtime_id(*id) ? "runtime texture" : "scene texture",
                                  static_cast<std::uint32_t>(source->mips.size())};
            if (source->array) {
                desc.type = rhi::TextureType::d2_array;
                desc.layers = source->layers;
            }
            auto created = device->create_texture(desc, mips);
            if (!created) {
                report("texture " + id->to_string(), created.error());
                gpu = {};
                return device->null_descriptor(rhi::DescriptorKind::texture);
            }
            gpu.source = source;
            gpu.texture = *created;
        }
        present = true;
        return device->descriptor(gpu.texture);
    }

    /// A repeating, trilinear sampler with `anisotropy` (2 to 16), made on first use; the scene's sampler if the
    /// device refuses it.
    rhi::SamplerHandle anisotropic_sampler(std::uint32_t anisotropy) {
        anisotropy = std::clamp(anisotropy, 2u, 16u);
        if (const auto it = anisotropic.find(anisotropy); it != anisotropic.end()) {
            return it->second;
        }
        auto made = device->create_sampler(
            {.filter = rhi::Filter::linear, .address = rhi::AddressMode::repeat, .max_anisotropy = anisotropy});
        if (!made) {
            report("anisotropic sampler", made.error());
            anisotropic[anisotropy] = sampler;
            return sampler;
        }
        anisotropic[anisotropy] = *made;
        return *made;
    }

    std::uint32_t kind_of(const std::string& name) {
        for (std::size_t k = 0; k < kinds.size(); ++k) {
            if (kinds[k].name == name) {
                return static_cast<std::uint32_t>(k);
            }
        }
        std::vector<std::string> names;
        for (const MaterialKind& k : kinds) {
            names.emplace_back(k.name);
        }
        report("kind " + name, Error{ErrorCode::asset_load_failed,
                                     std::format("material kind '{}' is not registered; using StandardMaterial", name),
                                     did_you_mean(name, names) + " Register it with veach_add_material() (ADR 0035)."});
        return 0;
    }

    MaterialRecord record_of(AssetId id) {
        MaterialRecord r;
        r.sampler = device->descriptor(sampler);
        r.base_color_texture = r.metallic_roughness_texture = r.normal_texture = r.emissive_texture = r.array_texture0 =
            r.array_texture1 = device->null_descriptor(rhi::DescriptorKind::texture);
        const MaterialAsset* m = is_runtime_id(id) ? runtime_material(id) : assets.get(Handle<MaterialAsset>{id});
        if (m == nullptr) {
            return r;
        }
        r.kind = kind_of(m->kind);
        if (m->max_anisotropy > 1) {
            r.sampler = device->descriptor(anisotropic_sampler(m->max_anisotropy));
        }
        r.custom0 = m->custom;
        bool array_present = false;
        r.array_texture0 = texture(m->array_textures[0], array_present);
        r.array_texture1 = texture(m->array_textures[1], array_present);
        r.alpha_cutoff = m->alpha_cutoff;
        r.normal_scale = m->normal_scale;
        r.base_color = m->base_color;
        const math::vec3 emission = m->emissive * m->emissive_strength;
        r.emission_metallic = math::vec4(emission, m->metallic);
        r.roughness = m->roughness;
        bool present = false;
        r.base_color_texture = texture(m->base_color_texture, present);
        r.flags |= present ? has_base_color_texture : 0u;
        r.metallic_roughness_texture = texture(m->metallic_roughness_texture, present);
        r.flags |= present ? has_metallic_roughness_texture : 0u;
        r.normal_texture = texture(m->normal_texture, present);
        r.flags |= present ? has_normal_texture : 0u;
        r.emissive_texture = texture(m->emissive_texture, present);
        r.flags |= present ? has_emissive_texture : 0u;
        r.flags |= m->alpha_mode == AlphaMode::mask ? alpha_mask : 0u;
        r.flags |= m->alpha_mode == AlphaMode::blend ? alpha_blend : 0u;
        r.flags |= m->double_sided ? double_sided : 0u;
        const float weight = luminance(emission);
        if (weight > 0.0f || kinds.at(r.kind).emissive) {
            r.flags |= emissive;
            r.emissive_weight = weight > 0.0f ? weight : 1.0f;
        }
        return r;
    }

    template <class T>
    rhi::BufferHandle upload(const std::vector<T>& values, std::uint32_t stride, const char* name) {
        const std::vector<T> one(values.empty() ? 1 : 0);  // never an empty buffer
        const std::vector<T>& data = values.empty() ? one : values;
        auto buffer = device->create_buffer(
            {.size = data.size() * sizeof(T), .memory = rhi::MemoryType::upload, .debug_name = name, .stride = stride},
            bytes_of(data));
        if (!buffer) {
            report(name, buffer.error());
            return {};
        }
        frame_buffers.push_back(*buffer);
        return *buffer;
    }
};

SceneRenderer::SceneRenderer(SceneShaders shaders, std::span<const MaterialKind> kinds, AssetServer& assets)
    : impl_(std::make_unique<Impl>(shaders, kinds, assets)) {}

SceneRenderer::~SceneRenderer() = default;

void SceneRenderer::reset() {
    impl_->reset = true;
}

std::uint64_t SceneRenderer::samples() const {
    return impl_->samples;
}

const CullStats& SceneRenderer::cull_stats() const {
    return impl_->realtime.stats();
}

std::uint64_t SceneRenderer::instance_bytes_uploaded() const {
    return impl_->gpu_scene ? impl_->gpu_scene->uploaded_bytes() : 0;
}

Result<> SceneRenderer::prepare(rhi::Device& device) {
    Impl& s = *impl_;
    s.device = &device;
    auto display = device.create_compute_pipeline({&veach_render_shaders::display(), "display", {8, 8, 1}, "display"});
    if (!display) {
        return std::unexpected(display.error());
    }
    s.display_pipeline = *display;
    if (auto atmosphere = s.atmosphere.prepare(device); !atmosphere) {
        return atmosphere;
    }
    if (auto realtime = s.realtime.prepare(device, s.shaders, static_cast<std::uint32_t>(s.kinds.size())); !realtime) {
        return realtime;
    }
    s.gpu_scene = std::make_unique<GpuScene>(device);
    auto sampler = device.create_sampler({rhi::Filter::linear, rhi::AddressMode::repeat});
    if (!sampler) {
        return std::unexpected(sampler.error());
    }
    s.sampler = *sampler;
    // Photo mode (ADR 0041) needs ray queries; without them every camera renders in real time.
    if (!device.capabilities().hardware_ray_tracing) {
        s.unavailable =
            Error{ErrorCode::rhi_backend_unavailable,
                  std::format("photo mode: '{}' has no hardware ray tracing", device.capabilities().device_name),
                  "Photo mode needs ray queries (Metal ray tracing or DXR 1.1). The camera renders with the "
                  "real-time renderer instead."};
        return {};
    }
    auto pipeline = device.create_compute_pipeline({&s.shaders.path_tracer, "trace_pixels", {8, 8, 1}, "path tracer"});
    if (!pipeline) {
        return std::unexpected(pipeline.error());
    }
    s.pipeline = *pipeline;
    const std::array<std::uint32_t, 4> zeros{};
    auto diagnostics = device.create_buffer({.size = sizeof(zeros),
                                             .usage = rhi::BufferUsage::storage | rhi::BufferUsage::copy_src,
                                             .debug_name = "path tracer diagnostics",
                                             .stride = sizeof(std::uint32_t)},
                                            std::as_bytes(std::span(zeros)));
    if (!diagnostics) {
        return std::unexpected(diagnostics.error());
    }
    s.diagnostics = *diagnostics;
    for (std::uint32_t i = 0; i < device.frames_in_flight(); ++i) {
        auto readback = device.create_buffer({.size = sizeof(zeros),
                                              .usage = rhi::BufferUsage::copy_dst,
                                              .memory = rhi::MemoryType::readback,
                                              .debug_name = "path tracer diagnostics readback"});
        if (!readback) {
            return std::unexpected(readback.error());
        }
        s.readbacks.push_back({*readback});
    }
    return {};
}

void SceneRenderer::record(RenderGraph& graph, const FrameContext& frame) {
    VEACH_ZONE("SceneRenderer::record");
    Impl& s = *impl_;
    const RenderView* view = frame.view;
    s.sweep(frame.world.runtime_assets);  // even when nothing draws: removed meshes free their GPU data
    // A scene with nothing to draw shows the background color, unless the sky has a space backdrop: deep space is a
    // scene of its own.
    const bool backdrop = frame.world.sky.backdrop.has_value() && frame.world.sky.backdrop_settings.visible;
    if (view == nullptr || (frame.world.meshes.empty() && !backdrop)) {
        return;
    }
    // A PhotoMode on the camera selects the path tracer (ADR 0041), where the device can ray trace; otherwise the
    // real-time renderer draws (ADR 0093).
    bool photo = view->photo_mode.has_value();
    if (photo && s.unavailable) {
        s.report("unavailable", *s.unavailable);
        photo = false;
    }
    const PhotoMode settings = view->photo_mode.value_or(PhotoMode{});
    s.release_frame_buffers();  // last frame's: their submission is made, so destroy() waits for the GPU

    // The scene on the GPU: meshes, materials (with textures), instances. Draws
    // with the same materials share one run of material records.
    std::vector<MaterialRecord> materials;
    std::map<std::vector<AssetId>, std::uint32_t> material_runs;  ///< a draw's materials -> their first record
    std::vector<InstanceRecord> instances;
    std::vector<std::uint64_t> entities;
    std::vector<AssetId> used_meshes;
    std::unordered_set<AssetId> used;
    std::vector<const MeshDraw*> draws;
    const std::vector<AssetId>* last_materials = nullptr;
    std::uint32_t last_slots = 0;
    std::uint32_t last_base = 0;
    std::uint32_t task_count = 0;
    std::array<std::uint32_t, draw_list_count> list_capacity{};
    for (const MeshDraw& draw : frame.world.meshes) {
        Impl::GpuMesh* mesh = s.mesh(draw.mesh);
        if (mesh == nullptr) {
            continue;
        }
        if (used.insert(draw.mesh).second) {
            used_meshes.push_back(draw.mesh);
        }
        InstanceRecord inst = mesh->base;
        const std::uint32_t slot_count = mesh->source->slot_count;
        // Consecutive draws of one model share its materials: a one-entry cache before the map.
        if (last_materials != nullptr && last_slots == slot_count && *last_materials == draw.materials) {
            inst.material_base = last_base;
        } else {
            std::vector<AssetId> key(slot_count);
            for (std::uint32_t slot = 0; slot < slot_count; ++slot) {
                key[slot] = slot < draw.materials.size() ? draw.materials[slot] : AssetId{};
            }
            const auto [run, added] =
                material_runs.try_emplace(std::move(key), static_cast<std::uint32_t>(materials.size()));
            if (added) {
                for (const AssetId& id : run->first) {
                    materials.push_back(s.record_of(id));
                }
            }
            inst.material_base = run->second;
            last_materials = &draw.materials;
            last_slots = slot_count;
            last_base = run->second;
        }
        // The draw lists this instance's meshlets land in, if they all survive
        // culling.
        for (std::uint32_t slot = 0; slot < slot_count; ++slot) {
            const bool masked = (materials[inst.material_base + slot].flags & (alpha_mask | alpha_blend)) != 0;
            inst.flags |= masked ? instance_has_alpha : 0u;
            for (std::uint32_t bucket = 0; bucket < draw_bucket_count; ++bucket) {
                list_capacity.at(draw_list_of(masked, 0) + bucket) += mesh->slot_buckets[slot].at(bucket);
            }
        }
        task_count += inst.meshlet_count > 1 ? (inst.meshlet_count + 63) / 64 : 0;
        // Normal cones hold under a rotation, a translation and a uniform scale that
        // does not mirror.
        const math::mat3 linear(draw.world);
        const math::vec3 scale(math::length(linear[0]), math::length(linear[1]), math::length(linear[2]));
        const float largest = std::max({scale.x, scale.y, scale.z});
        if (math::determinant(linear) > 0.0f && largest - std::min({scale.x, scale.y, scale.z}) <= 1e-3f * largest) {
            inst.flags |= instance_cone_culling;
        }
        inst.object_to_world = rows_of(draw.world);
        inst.normal_to_world = rows_of(math::mat4(math::transpose(math::inverse(linear))));
        inst.id = surface_id(draw.entity);
        inst.material_origin = draw.material_origin;
        instances.push_back(inst);
        entities.push_back(draw.entity);
        draws.push_back(&draw);
    }
    if (instances.empty() && !backdrop) {
        return;
    }
    photo = photo && !instances.empty();  // the path tracer needs geometry for its
                                          // acceleration structure
    const std::uint32_t scene_flags =
        std::ranges::any_of(instances, [](const InstanceRecord& i) { return (i.flags & instance_has_alpha) != 0; })
            ? scene_has_alpha
            : 0u;

    std::vector<LightRecord> lights;
    for (const RenderLight& l : frame.world.lights) {
        LightRecord r;
        r.position = math::vec4(l.position, 1.0f);
        r.direction = math::vec4(l.direction, 0.0f);
        r.radiance = math::vec4(l.radiance, 0.0f);
        r.cos_inner = l.cos_inner;
        r.cos_outer = l.cos_outer;
        r.kind = l.kind == LightKind::directional ? 0u : l.kind == LightKind::point ? 1u : 2u;
        r.size = l.size;
        lights.push_back(r);
    }

    // The GPU scene: last frame's records stay on the GPU; only changed ones are
    // uploaded (gpu_scene.hpp).
    const GraphResource instance_data =
        s.gpu_scene->update(graph, *frame.uploads, instances, entities, frame.world.space);
    const rhi::BufferHandle material_buffer = s.upload(materials, sizeof(MaterialRecord), "scene materials");
    const rhi::BufferHandle light_buffer = s.upload(lights, sizeof(LightRecord), "scene lights");
    if (!instance_data.is_valid() || !material_buffer.is_valid() || !light_buffer.is_valid()) {
        return;
    }

    std::vector<GraphResource> buffers;
    for (const AssetId id : used_meshes) {
        const Impl::GpuMesh& mesh = s.meshes.at(id);
        for (const rhi::BufferHandle b : {mesh.vertices, mesh.indices, mesh.slots, mesh.meshlets, mesh.meshlet_vertices,
                                          mesh.meshlet_triangles, mesh.meshlet_bounds}) {
            buffers.push_back(graph.import(b, "mesh data", rhi::ResourceState::shader_read));
        }
    }
    std::vector<GraphResource> textures;
    for (const auto& [id, texture] : s.textures) {
        if (texture.texture.is_valid()) {
            textures.push_back(graph.import(texture.texture, "scene texture", rhi::ResourceState::shader_read));
        }
    }
    const GraphResource material_data =
        graph.import(material_buffer, "scene materials", rhi::ResourceState::shader_read);
    const GraphResource light_data = graph.import(light_buffer, "scene lights", rhi::ResourceState::shader_read);
    const auto read_meshes = [buffers, instance_data, material_data](PassBuilder& b) {
        for (const GraphResource& r : buffers) {
            b.read(r);
        }
        b.read(instance_data);
        b.read(material_data);
    };
    const auto read_geometry = [read_meshes, textures, light_data](PassBuilder& b) {
        read_meshes(b);
        for (const GraphResource& r : textures) {
            b.read(r);
        }
        b.read(light_data);
    };
    // The sky (ADR 0038): its LUTs for this view. A lit scene without one is dark
    // wherever lights do not reach.
    const ResolvedSky& resolved_sky = frame.world.sky;
    if (!s.warned_no_sky && !frame.world.lights.empty() &&
        (resolved_sky.mode == SkyMode::none ||
         (resolved_sky.mode == SkyMode::constant && resolved_sky.settings.color == math::vec3(0.0f)))) {
        log::warn("render",
                  "the scene has lights but no sky (Sky.mode = none): "
                  "whatever they do not reach directly "
                  "renders black. Leave Sky out for the physical sky, or "
                  "set a constant one.");
        s.warned_no_sky = true;
    }
    const SkyResources sky = s.atmosphere.record(graph, resolved_sky, *view);
    if (!sky.read) {
        return;
    }

    if (!photo) {
        // ---- the real-time renderer (ADR 0093).
        const RealtimeScene realtime{.read_meshes = read_meshes,
                                     .read_geometry = read_geometry,
                                     .instances = instance_data,
                                     .instance_count = static_cast<std::uint32_t>(instances.size()),
                                     .task_count = task_count,
                                     .list_capacity = list_capacity,
                                     .materials = material_buffer,
                                     .lights = light_buffer,
                                     .light_count = static_cast<std::uint32_t>(lights.size()),
                                     .sky = sky.frame,
                                     .read_sky = sky.read};
        s.realtime.record(graph, frame, *view, realtime);
        return;
    }

    // ---- photo mode (ADR 0041): BLASes, the TLAS, emissive triangles as lights, then the path tracer.
    for (const AssetId id : used_meshes) {
        if (!s.ensure_blas(s.meshes.at(id))) {
            return;
        }
    }
    if (s.tlas_meshes != used_meshes || s.tlas_capacity < instances.size()) {
        s.destroy_if(s.tlas);
        std::vector<rhi::AccelHandle> blases;
        for (const AssetId id : used_meshes) {
            blases.push_back(s.meshes.at(id).blas);
        }
        s.tlas_capacity = std::max<std::uint32_t>(16, static_cast<std::uint32_t>(instances.size()) * 2);
        auto tlas =
            s.device->create_tlas({.max_instances = s.tlas_capacity, .blases = blases, .debug_name = "scene TLAS"});
        if (!tlas) {
            s.report("tlas", tlas.error());
            return;
        }
        s.tlas = *tlas;
        s.tlas_meshes = used_meshes;
        s.tlas_built = false;
    }
    std::vector<rhi::TlasInstance> tlas_instances;
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const InstanceRecord& inst = instances[i];
        rhi::TlasInstance t;
        t.blas_index =
            static_cast<std::uint32_t>(std::ranges::find(s.tlas_meshes, draws[i]->mesh) - s.tlas_meshes.begin());
        t.mask = (inst.flags & instance_has_alpha) != 0 ? instance_mask_alpha : instance_mask_opaque;
        t.opaque = (inst.flags & instance_has_alpha) == 0;  // alpha-tested: candidates for visibility rays
        for (std::size_t r = 0; r < 3; ++r) {
            t.transform.at(r) = {inst.object_to_world.at(r).x, inst.object_to_world.at(r).y,
                                 inst.object_to_world.at(r).z, inst.object_to_world.at(r).w};
        }
        tlas_instances.push_back(t);
    }

    // Every triangle whose material emits is a light.
    std::vector<EmissiveTriangle> emitters;
    float total = 0.0f;
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const MeshAsset& mesh = *s.meshes.at(draws[i]->mesh).source;
        for (std::size_t t = 0; t < mesh.triangle_slots.size(); ++t) {
            const MaterialRecord& m = materials[instances[i].material_base + mesh.triangle_slots[t]];
            if ((m.flags & emissive) == 0) {
                continue;
            }
            const auto world = [&](std::size_t corner) {
                const auto& p = mesh.vertices[mesh.indices[t * 3 + corner]].position;
                return math::vec3(draws[i]->world * math::vec4(p[0], p[1], p[2], 1.0f));
            };
            const std::array<math::vec3, 3> corners{world(0), world(1), world(2)};
            const float area = 0.5f * math::length(math::cross(corners[1] - corners[0], corners[2] - corners[0]));
            total += area * m.emissive_weight;
            emitters.push_back({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(t), total, 0.0f,
                                math::vec4(corners[0], m.emissive_weight),
                                math::vec4(corners[1], (m.flags & double_sided) != 0 ? 1.0f : 0.0f),
                                math::vec4(corners[2], 0.0f)});
        }
    }
    for (EmissiveTriangle& e : emitters) {
        e.cdf /= total;
    }
    const rhi::BufferHandle emissive_buffer = s.upload(emitters, sizeof(EmissiveTriangle), "scene emitters");
    if (!emissive_buffer.is_valid()) {
        return;
    }
    const GraphResource emissive_data =
        graph.import(emissive_buffer, "scene emitters", rhi::ResourceState::shader_read);

    // Restart the accumulation when anything it depends on changed.
    const auto [width, height] = std::pair{frame.width, frame.height};
    if (!s.accumulation.is_valid() || s.width != width || s.height != height) {
        s.destroy_if(s.accumulation);
        auto texture = s.device->create_texture({width, height, rhi::Format::rgba32_float,
                                                 rhi::TextureUsage::storage | rhi::TextureUsage::sampled,
                                                 "path tracer accumulation"});
        if (!texture) {
            s.report("accumulation", texture.error());
            return;
        }
        s.accumulation = *texture;
        s.width = width;
        s.height = height;
        s.accumulation_written = false;
        s.reset = true;
    }
    Fnv64 hash;
    hash.add(&view->view_projection, sizeof(math::mat4));
    // Not samples_per_frame: it sets the pace, not the image (MCP photo_mode changes it mid-accumulation).
    hash.add(&settings.max_bounces, sizeof(settings.max_bounces));
    hash.add(&resolved_sky.mode, sizeof(resolved_sky.mode));
    hash.add(&resolved_sky.settings, sizeof(resolved_sky.settings));
    hash.add(&resolved_sky.sun_direction, sizeof(resolved_sky.sun_direction));
    hash.add(&resolved_sky.sun_illuminance_top, sizeof(resolved_sky.sun_illuminance_top));
    hash.add(&view->exposure, sizeof(float));
    hash.add_all(materials);
    hash.add_all(lights);
    for (const InstanceRecord& inst : instances) {
        hash.add(&inst.object_to_world, sizeof(inst.object_to_world));
        hash.add(&inst.material_base, sizeof(std::uint32_t));
    }
    for (const AssetId id : used_meshes) {
        hash.add(&id, sizeof(id));
        const MeshAsset* source = s.meshes.at(id).source;
        hash.add(&source, sizeof(source));  // a reload swaps the data
    }
    if (hash.h != s.scene_hash || s.reset) {
        s.scene_hash = hash.h;
        s.samples = 0;
        s.reset = false;
    }

    std::vector<GraphResource> blases;
    std::vector<rhi::AccelHandle> to_build;
    for (const AssetId id : used_meshes) {
        Impl::GpuMesh& mesh = s.meshes.at(id);
        blases.push_back(graph.import(mesh.blas, "mesh BLAS",
                                      mesh.built ? rhi::ResourceState::accel_read : rhi::ResourceState::undefined));
        if (!mesh.built) {
            to_build.push_back(mesh.blas);
            mesh.built = true;
        }
    }
    if (!to_build.empty()) {
        graph.add_transfer_pass(
            "scene: build BLASes",
            [&](PassBuilder& b) {
                for (const GraphResource& r : buffers) {
                    b.read(r);
                }
                for (std::size_t i = 0; i < used_meshes.size(); ++i) {
                    if (std::ranges::find(to_build, s.meshes.at(used_meshes[i]).blas) != to_build.end()) {
                        b.write(blases[i], rhi::ResourceState::accel_build);
                    }
                }
            },
            [to_build](PassContext& ctx) {
                for (const rhi::AccelHandle blas : to_build) {
                    ctx.cmd.build_blas(blas);
                }
            });
    }
    const GraphResource tlas = graph.import(
        s.tlas, "scene TLAS", s.tlas_built ? rhi::ResourceState::accel_read : rhi::ResourceState::undefined);
    s.tlas_built = true;
    graph.add_transfer_pass(
        "scene: build TLAS",
        [&](PassBuilder& b) {
            for (const GraphResource& r : blases) {
                b.read(r, rhi::ResourceState::accel_read);
            }
            b.write(tlas, rhi::ResourceState::accel_build);
        },
        [tlas_handle = s.tlas, tlas_instances](PassContext& ctx) { ctx.cmd.build_tlas(tlas_handle, tlas_instances); });

    const GraphResource accumulation =
        graph.import(s.accumulation, "path tracer accumulation",
                     s.accumulation_written ? rhi::ResourceState::shader_read : rhi::ResourceState::undefined);
    s.accumulation_written = true;
    // The counters: this slot's last frame has completed (begin_frame waited for it), so its readback is ready.
    const rhi::FrameInfo info = s.device->current_frame();
    Impl::Readback& slot = s.readbacks.at(info.slot % s.readbacks.size());
    s.check(slot);
    const GraphResource diagnostics =
        graph.import(s.diagnostics, "path tracer diagnostics",
                     s.diagnostics_written ? rhi::ResourceState::copy_src : rhi::ResourceState::undefined);
    s.diagnostics_written = true;

    PathTraceParams params{};
    params.inverse_view_projection = view->inverse_view_projection;
    params.camera_position_exposure = math::vec4(view->position, view->exposure);
    params.sky_bounces = math::vec4(0.0f, 0.0f, 0.0f, static_cast<float>(settings.max_bounces));
    params.sky = s.device->descriptor(sky.frame);
    params.scene_flags = scene_flags;
    params.size = {width, height};
    params.sample_index = static_cast<std::uint32_t>(s.samples);
    params.samples_per_frame = std::max(1u, settings.samples_per_frame);
    params.light_count = static_cast<std::uint32_t>(lights.size());
    params.emissive_count = static_cast<std::uint32_t>(emitters.size());
    params.emissive_total_weight = total;
    s.samples += params.samples_per_frame;

    graph.add_compute_pass(
        "path tracer",
        [&](PassBuilder& b) {
            sky.read(b);
            b.read(tlas, rhi::ResourceState::accel_read);
            for (const GraphResource& r : blases) {
                b.read(r, rhi::ResourceState::accel_read);
            }
            read_geometry(b);
            b.read(emissive_data);
            b.write(accumulation, rhi::ResourceState::shader_write);
            b.write(diagnostics, rhi::ResourceState::shader_write);
        },
        [params, pipeline = s.pipeline, tlas, accumulation, instance_data, material_buffer, light_buffer,
         emissive_buffer, diagnostics](PassContext& ctx) mutable {
            params.diagnostics = ctx.device.descriptor(ctx.buffer(diagnostics), rhi::View::storage);
            params.accumulation = ctx.device.descriptor(ctx.texture(accumulation), rhi::View::storage);
            params.scene = ctx.device.descriptor(ctx.accel(tlas));
            params.instances = ctx.device.descriptor(ctx.buffer(instance_data));
            params.materials = ctx.device.descriptor(material_buffer);
            params.lights = ctx.device.descriptor(light_buffer);
            params.emissive = ctx.device.descriptor(emissive_buffer);
            ctx.cmd.set_pipeline(pipeline);
            ctx.cmd.set_parameters(params);
            ctx.cmd.dispatch((params.size[0] + 7) / 8, (params.size[1] + 7) / 8);
        });

    // The accumulation is the image: exposure and AgX turn it into scene_color (ADR 0037).
    DisplayParams display{};
    display.size = {width, height};
    display.input_size = {width, height};
    display.exposure = view->exposure;
    display.tonemap = 1;
    graph.add_compute_pass(
        "display",
        [&](PassBuilder& b) {
            b.read(accumulation);
            b.write(frame.scene_color, rhi::ResourceState::shader_write);
        },
        [display, pipeline = s.display_pipeline, accumulation, color = frame.scene_color](PassContext& ctx) mutable {
            display.radiance = ctx.device.descriptor(ctx.texture(accumulation));
            display.output = ctx.device.descriptor(ctx.texture(color), rhi::View::storage);
            ctx.cmd.set_pipeline(pipeline);
            ctx.cmd.set_parameters(display);
            ctx.cmd.dispatch((display.size[0] + 7) / 8, (display.size[1] + 7) / 8);
        });

    const GraphResource readback =
        graph.import(slot.buffer, "path tracer diagnostics readback", rhi::ResourceState::undefined);
    graph.add_transfer_pass(
        "path tracer diagnostics",
        [&](PassBuilder& b) {
            b.read(diagnostics, rhi::ResourceState::copy_src);
            b.write(readback, rhi::ResourceState::copy_dst);
        },
        [diagnostics, readback](PassContext& ctx) {
            ctx.cmd.copy_buffer(ctx.buffer(diagnostics), ctx.buffer(readback), 4 * sizeof(std::uint32_t));
        });
    slot.frame = info.index;
    slot.samples = s.samples;
}

}  // namespace veach::render
