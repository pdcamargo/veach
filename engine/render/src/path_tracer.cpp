#include <veach/render/path_tracer.hpp>

#include <veach/asset/asset_server.hpp>
#include <veach/asset/scene.hpp>
#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>
#include <veach/core/suggest.hpp>

#include "veach_render_shaders.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <set>
#include <unordered_map>

namespace veach::render {

namespace {

// Flag bits, as in shaders/veach/material.slang.
constexpr std::uint32_t has_base_color_texture = 1u << 0;
constexpr std::uint32_t has_metallic_roughness_texture = 1u << 1;
constexpr std::uint32_t has_normal_texture = 1u << 2;
constexpr std::uint32_t has_emissive_texture = 1u << 3;
constexpr std::uint32_t alpha_mask = 1u << 4;
constexpr std::uint32_t alpha_blend = 1u << 5;
constexpr std::uint32_t double_sided = 1u << 6;
constexpr std::uint32_t emissive = 1u << 7;

/// Mirrors InstanceRecord in shaders/path_tracer.slang.
struct InstanceRecord {
    rhi::DescriptorHandle vertices;
    rhi::DescriptorHandle indices;
    rhi::DescriptorHandle slots;
    std::uint32_t material_base = 0;
    std::uint32_t flags = 0;
    std::array<math::vec4, 3> object_to_world{};
    std::array<math::vec4, 3> normal_to_world{};
};
static_assert(sizeof(InstanceRecord) == 128);

/// Mirrors LightRecord.
struct LightRecord {
    math::vec4 position{0.0f};
    math::vec4 direction{0.0f};
    math::vec4 radiance{0.0f};
    float cos_inner = 1.0f;
    float cos_outer = 0.0f;
    std::uint32_t kind = 0;
    std::uint32_t pad = 0;
};
static_assert(sizeof(LightRecord) == 64);

/// Mirrors EmissiveTriangle.
struct EmissiveTriangle {
    std::uint32_t instance = 0;
    std::uint32_t primitive = 0;
    float cdf = 0.0f;
    float pad = 0.0f;
};
static_assert(sizeof(EmissiveTriangle) == 16);

/// Mirrors `Params` in shaders/path_tracer.slang. Metal rounds it to 16 bytes (a float4x4 inside).
struct alignas(16) PathTraceParams {
    math::mat4 inverse_view_projection;
    math::vec4 camera_position_exposure;
    math::vec4 sky_bounces;
    rhi::DescriptorHandle output;
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
    std::uint32_t pad;
};
static_assert(rhi::parameters_match<PathTraceParams>(
    veach_render_shaders::layout::path_tracer,
    {VEACH_PARAM(PathTraceParams, inverse_view_projection), VEACH_PARAM(PathTraceParams, camera_position_exposure),
     VEACH_PARAM(PathTraceParams, sky_bounces), VEACH_PARAM(PathTraceParams, output),
     VEACH_PARAM(PathTraceParams, accumulation), VEACH_PARAM(PathTraceParams, scene),
     VEACH_PARAM(PathTraceParams, instances), VEACH_PARAM(PathTraceParams, materials),
     VEACH_PARAM(PathTraceParams, lights), VEACH_PARAM(PathTraceParams, emissive), VEACH_PARAM(PathTraceParams, size),
     VEACH_PARAM(PathTraceParams, sample_index), VEACH_PARAM(PathTraceParams, samples_per_frame),
     VEACH_PARAM(PathTraceParams, light_count), VEACH_PARAM(PathTraceParams, emissive_count),
     VEACH_PARAM(PathTraceParams, emissive_total_weight), VEACH_PARAM(PathTraceParams, pad)}));

float luminance(math::vec3 c) {
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;  // GLM_FORCE_XYZW_ONLY
}

rhi::Format to_rhi(TextureFormat format) {
    switch (format) {
        case TextureFormat::rgba8_unorm: return rhi::Format::rgba8_unorm;
        case TextureFormat::rgba8_srgb: return rhi::Format::rgba8_srgb;
        case TextureFormat::bc7_unorm: return rhi::Format::bc7_unorm;
        case TextureFormat::bc7_srgb: return rhi::Format::bc7_srgb;
    }
    return rhi::Format::rgba8_unorm;
}

/// The rows of the upper 3x4 of a column-major matrix.
std::array<math::vec4, 3> rows_of(const math::mat4& m) {
    return {math::vec4(m[0][0], m[1][0], m[2][0], m[3][0]), math::vec4(m[0][1], m[1][1], m[2][1], m[3][1]),
            math::vec4(m[0][2], m[1][2], m[2][2], m[3][2])};
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

struct PathTracerFeature::Impl {
    struct GpuMesh {
        const MeshAsset* source = nullptr;  ///< the asset data it was made from; changes on hot reload
        rhi::BufferHandle vertices;
        rhi::BufferHandle indices;
        rhi::BufferHandle slots;
        rhi::AccelHandle blas;
        bool built = false;
    };
    struct GpuTexture {
        const TextureAsset* source = nullptr;
        rhi::TextureHandle texture;
    };

    const rhi::ShaderLibrary& library;
    std::vector<MaterialKind> kinds;
    AssetServer& assets;
    rhi::Device* device = nullptr;
    rhi::PipelineHandle pipeline;
    rhi::SamplerHandle sampler;
    std::unordered_map<AssetId, GpuMesh> meshes;
    std::unordered_map<AssetId, GpuTexture> textures;
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
    std::set<std::string> reported;
    std::optional<Error> unavailable;  ///< why this device cannot path trace; reported when a camera asks

    Impl(const rhi::ShaderLibrary& lib, std::span<const MaterialKind> k, AssetServer& a)
        : library(lib), kinds(k.begin(), k.end()), assets(a) {}

    ~Impl() {
        if (device == nullptr) {
            return;
        }
        device->wait_idle();
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
        destroy_if(m.blas);
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

    GpuMesh* mesh(AssetId id) {
        const MeshAsset* source = assets.get(Handle<MeshAsset>{id});
        if (source == nullptr || source->indices.empty()) {
            return nullptr;
        }
        GpuMesh& gpu = meshes[id];
        if (gpu.source == source) {
            return &gpu;
        }
        destroy(gpu);  // new or reloaded
        gpu.source = source;
        gpu.built = false;
        auto vertices = device->create_buffer({.size = source->vertices.size() * sizeof(MeshVertex),
                                               .usage = rhi::BufferUsage::storage | rhi::BufferUsage::accel_input,
                                               .debug_name = "mesh vertices",
                                               .stride = 4},
                                              bytes_of(source->vertices));
        auto indices = device->create_buffer({.size = source->indices.size() * 4,
                                              .usage = rhi::BufferUsage::storage | rhi::BufferUsage::accel_input,
                                              .debug_name = "mesh indices",
                                              .stride = 4},
                                             bytes_of(source->indices));
        auto slots =
            device->create_buffer({.size = source->triangle_slots.size() * 4, .debug_name = "mesh slots", .stride = 4},
                                  bytes_of(source->triangle_slots));
        if (!vertices || !indices || !slots) {
            report("mesh buffers", Error{ErrorCode::rhi_gpu_error, "path tracer: cannot create mesh buffers", {}});
            return nullptr;
        }
        gpu.vertices = *vertices;
        gpu.indices = *indices;
        gpu.slots = *slots;
        auto blas =
            device->create_blas({.vertices = gpu.vertices,
                                 .vertex_count = static_cast<std::uint32_t>(source->vertices.size()),
                                 .vertex_stride = sizeof(MeshVertex),
                                 .indices = gpu.indices,
                                 .index_count = static_cast<std::uint32_t>(source->indices.size()),
                                 .opaque = true,  // alpha: rays continue past transparent texels (path_tracer.slang)
                                 .debug_name = "mesh BLAS"});
        if (!blas) {
            report("blas", blas.error());
            return nullptr;
        }
        gpu.blas = *blas;
        tlas_meshes.clear();  // the TLAS must be recreated with the new BLAS
        return &gpu;
    }

    rhi::DescriptorHandle texture(const std::optional<AssetId>& id, bool& present) {
        present = false;
        if (!id) {
            return {};
        }
        const TextureAsset* source = assets.get(Handle<TextureAsset>{*id});
        if (source == nullptr) {
            return {};
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
            auto created = device->create_texture(
                {source->width, source->height, to_rhi(source->format), rhi::TextureUsage::sampled, "scene texture",
                 static_cast<std::uint32_t>(source->mips.size())},
                mips);
            if (!created) {
                report("texture " + id->to_string(), created.error());
                gpu = {};
                return {};
            }
            gpu.source = source;
            gpu.texture = *created;
        }
        present = true;
        return device->descriptor(gpu.texture);
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
                                     did_you_mean(name, names) + " Register it with veach_add_material() (ADR 0033)."});
        return 0;
    }

    MaterialRecord record_of(AssetId id) {
        MaterialRecord r;
        r.sampler = device->descriptor(sampler);
        const MaterialAsset* m = assets.get(Handle<MaterialAsset>{id});
        if (m == nullptr) {
            return r;
        }
        r.kind = kind_of(m->kind);
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

PathTracerFeature::PathTracerFeature(const rhi::ShaderLibrary& library, std::span<const MaterialKind> kinds,
                                     AssetServer& assets)
    : impl_(std::make_unique<Impl>(library, kinds, assets)) {}

PathTracerFeature::~PathTracerFeature() = default;

void PathTracerFeature::reset() {
    impl_->reset = true;
}

std::uint64_t PathTracerFeature::samples() const {
    return impl_->samples;
}

Result<> PathTracerFeature::prepare(rhi::Device& device) {
    Impl& s = *impl_;
    s.device = &device;
    if (!device.capabilities().hardware_ray_tracing) {
        s.unavailable = Error{ErrorCode::rhi_backend_unavailable,
                              std::format("'{}' has no hardware ray tracing", device.capabilities().device_name),
                              "The reference path tracer needs ray queries (Metal ray tracing or DXR 1.1)."};
        return {};
    }
    auto pipeline = device.create_compute_pipeline({&s.library, "trace_pixels", {8, 8, 1}, "path tracer"});
    if (!pipeline) {
        return std::unexpected(pipeline.error());
    }
    s.pipeline = *pipeline;
    auto sampler = device.create_sampler({rhi::Filter::linear, rhi::AddressMode::repeat});
    if (!sampler) {
        return std::unexpected(sampler.error());
    }
    s.sampler = *sampler;
    return {};
}

void PathTracerFeature::record(RenderGraph& graph, const FrameContext& frame) {
    VEACH_ZONE("PathTracerFeature::record");
    Impl& s = *impl_;
    const RenderView* view = frame.view;
    if (view == nullptr || !view->path_tracer || frame.world.meshes.empty()) {
        return;
    }
    if (s.unavailable) {
        s.report("unavailable", *s.unavailable);
        return;
    }
    const PathTracer& settings = *view->path_tracer;
    s.release_frame_buffers();  // last frame's: their submission is made, so destroy() waits for the GPU

    // The scene on the GPU: meshes and their BLASes, materials (with textures), instances.
    std::vector<MaterialRecord> materials;
    std::vector<InstanceRecord> instances;
    std::vector<rhi::TlasInstance> tlas_instances;
    std::vector<AssetId> used_meshes;
    std::vector<const MeshDraw*> draws;
    for (const MeshDraw& draw : frame.world.meshes) {
        Impl::GpuMesh* mesh = s.mesh(draw.mesh);
        if (mesh == nullptr) {
            continue;
        }
        if (std::ranges::find(used_meshes, draw.mesh) == used_meshes.end()) {
            used_meshes.push_back(draw.mesh);
        }
        InstanceRecord inst;
        inst.vertices = s.device->descriptor(mesh->vertices);
        inst.indices = s.device->descriptor(mesh->indices);
        inst.slots = s.device->descriptor(mesh->slots);
        inst.material_base = static_cast<std::uint32_t>(materials.size());
        for (std::uint32_t slot = 0; slot < mesh->source->slot_count; ++slot) {
            materials.push_back(slot < draw.materials.size() ? s.record_of(draw.materials[slot]) : MaterialRecord{});
        }
        inst.object_to_world = rows_of(draw.world);
        inst.normal_to_world = rows_of(math::mat4(math::transpose(math::inverse(math::mat3(draw.world)))));
        instances.push_back(inst);
        draws.push_back(&draw);
        rhi::TlasInstance t;
        const auto rows = inst.object_to_world;
        for (std::size_t r = 0; r < 3; ++r) {
            t.transform.at(r) = {rows.at(r).x, rows.at(r).y, rows.at(r).z, rows.at(r).w};
        }
        tlas_instances.push_back(t);  // blas_index set below, once the BLAS list is final
    }
    if (instances.empty()) {
        return;
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
    for (std::size_t i = 0; i < draws.size(); ++i) {
        tlas_instances[i].blas_index =
            static_cast<std::uint32_t>(std::ranges::find(s.tlas_meshes, draws[i]->mesh) - s.tlas_meshes.begin());
    }

    // Lights: punctual ones, and every triangle whose material emits.
    std::vector<LightRecord> lights;
    for (const RenderLight& l : frame.world.lights) {
        LightRecord r;
        r.position = math::vec4(l.position, 1.0f);
        r.direction = math::vec4(l.direction, 0.0f);
        r.radiance = math::vec4(l.radiance, 0.0f);
        r.cos_inner = l.cos_inner;
        r.cos_outer = l.cos_outer;
        r.kind = l.kind == LightKind::directional ? 0u : l.kind == LightKind::point ? 1u : 2u;
        lights.push_back(r);
    }
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
            const float area = 0.5f * math::length(math::cross(world(1) - world(0), world(2) - world(0)));
            total += area * m.emissive_weight;
            emitters.push_back({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(t), total, 0.0f});
        }
    }
    for (EmissiveTriangle& e : emitters) {
        e.cdf /= total;
    }

    // Restart the accumulation when anything it depends on changed.
    const auto [width, height] = std::pair{frame.width, frame.height};
    if (!s.accumulation.is_valid() || s.width != width || s.height != height) {
        s.destroy_if(s.accumulation);
        auto texture = s.device->create_texture(
            {width, height, rhi::Format::rgba32_float, rhi::TextureUsage::storage, "path tracer accumulation"});
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
    // Not samples_per_frame: it sets the pace, not the image (MCP path_trace changes it mid-accumulation).
    hash.add(&settings.max_bounces, sizeof(settings.max_bounces));
    hash.add(&settings.sky, sizeof(settings.sky));
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

    const rhi::BufferHandle instance_buffer = s.upload(instances, sizeof(InstanceRecord), "path tracer instances");
    const rhi::BufferHandle material_buffer = s.upload(materials, sizeof(MaterialRecord), "path tracer materials");
    const rhi::BufferHandle light_buffer = s.upload(lights, sizeof(LightRecord), "path tracer lights");
    const rhi::BufferHandle emissive_buffer = s.upload(emitters, sizeof(EmissiveTriangle), "path tracer emitters");
    if (!instance_buffer.is_valid() || !material_buffer.is_valid() || !light_buffer.is_valid() ||
        !emissive_buffer.is_valid()) {
        return;
    }

    // ---- passes: build new BLASes, build the TLAS, trace.
    std::vector<GraphResource> blases;
    std::vector<GraphResource> buffers;
    std::vector<rhi::AccelHandle> to_build;
    for (const AssetId id : used_meshes) {
        Impl::GpuMesh& mesh = s.meshes.at(id);
        blases.push_back(graph.import(mesh.blas, "mesh BLAS",
                                      mesh.built ? rhi::ResourceState::accel_read : rhi::ResourceState::undefined));
        for (const rhi::BufferHandle b : {mesh.vertices, mesh.indices, mesh.slots}) {
            buffers.push_back(graph.import(b, "mesh data", rhi::ResourceState::shader_read));
        }
        if (!mesh.built) {
            to_build.push_back(mesh.blas);
            mesh.built = true;
        }
    }
    if (!to_build.empty()) {
        graph.add_transfer_pass(
            "path tracer: build BLASes",
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
        "path tracer: build TLAS",
        [&](PassBuilder& b) {
            for (const GraphResource& r : blases) {
                b.read(r, rhi::ResourceState::accel_read);
            }
            b.write(tlas, rhi::ResourceState::accel_build);
        },
        [tlas_handle = s.tlas, tlas_instances](PassContext& ctx) { ctx.cmd.build_tlas(tlas_handle, tlas_instances); });

    std::vector<GraphResource> textures;
    for (const auto& [id, texture] : s.textures) {
        if (texture.texture.is_valid()) {
            textures.push_back(graph.import(texture.texture, "scene texture", rhi::ResourceState::shader_read));
        }
    }
    std::vector<GraphResource> frame_data;
    for (const rhi::BufferHandle b : {instance_buffer, material_buffer, light_buffer, emissive_buffer}) {
        frame_data.push_back(graph.import(b, "path tracer frame data", rhi::ResourceState::shader_read));
    }
    const GraphResource accumulation =
        graph.import(s.accumulation, "path tracer accumulation",
                     s.accumulation_written ? rhi::ResourceState::shader_write : rhi::ResourceState::undefined);
    s.accumulation_written = true;

    PathTraceParams params{};
    params.inverse_view_projection = view->inverse_view_projection;
    params.camera_position_exposure = math::vec4(view->position, view->exposure);
    params.sky_bounces = math::vec4(settings.sky, static_cast<float>(settings.max_bounces));
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
            b.read(tlas, rhi::ResourceState::accel_read);
            for (const GraphResource& r : blases) {
                b.read(r, rhi::ResourceState::accel_read);
            }
            for (const GraphResource& r : buffers) {
                b.read(r);
            }
            for (const GraphResource& r : textures) {
                b.read(r);
            }
            for (const GraphResource& r : frame_data) {
                b.read(r);
            }
            b.write(accumulation, rhi::ResourceState::shader_write);
            b.write(frame.scene_color, rhi::ResourceState::shader_write);
        },
        [params, pipeline = s.pipeline, tlas, accumulation, color = frame.scene_color, instance_buffer, material_buffer,
         light_buffer, emissive_buffer](PassContext& ctx) mutable {
            params.output = ctx.device.descriptor(ctx.texture(color), rhi::View::storage);
            params.accumulation = ctx.device.descriptor(ctx.texture(accumulation), rhi::View::storage);
            params.scene = ctx.device.descriptor(ctx.accel(tlas));
            params.instances = ctx.device.descriptor(instance_buffer);
            params.materials = ctx.device.descriptor(material_buffer);
            params.lights = ctx.device.descriptor(light_buffer);
            params.emissive = ctx.device.descriptor(emissive_buffer);
            ctx.cmd.set_pipeline(pipeline);
            ctx.cmd.set_parameters(params);
            ctx.cmd.dispatch((params.size[0] + 7) / 8, (params.size[1] + 7) / 8);
        });
}

}  // namespace veach::render
