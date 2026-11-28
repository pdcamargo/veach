// glTF 2.0 import (ADR 0011): fastgltf parses, we cook. Meshes and textures,
// the expensive parts, are cached in <project>/.veach/cache/<id>/<key>/, where
// the key hashes everything the cook reads; the scene and its materials are
// rebuilt from the (already parsed) glTF on every load.

#include <veach/asset/scene.hpp>

#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>
#include <veach/platform/fs.hpp>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <variant>

namespace veach {

namespace {

constexpr std::uint32_t cooker_version = 3;        ///< bump when cooked data changes, to invalidate every cache entry
constexpr std::uint32_t vmesh_magic = 0x48534D56;  // "VMSH"
constexpr std::uint32_t vmesh_version = 2;         // 2: meshlets

/// FNV-1a, 64 bits: a cache key, not a security boundary.
struct Fnv64 {
    std::uint64_t h = 14695981039346656037ULL;
    void add(std::span<const std::byte> bytes) {
        for (const std::byte b : bytes) {
            h = (h ^ static_cast<std::uint8_t>(b)) * 1099511628211ULL;
        }
    }
    void add(std::uint64_t v) {
        std::array<std::byte, 8> bytes{};
        std::memcpy(bytes.data(), &v, sizeof(v));
        add(bytes);
    }
};

std::span<const std::byte> source_bytes(const fastgltf::DataSource& source) {
    if (const auto* array = std::get_if<fastgltf::sources::Array>(&source)) {
        return {array->bytes.data(), array->bytes.size()};
    }
    if (const auto* view = std::get_if<fastgltf::sources::ByteView>(&source)) {
        return view->bytes;
    }
    return {};
}

std::span<const std::byte> image_bytes(const fastgltf::Asset& asset, const fastgltf::Image& image) {
    if (const auto* view = std::get_if<fastgltf::sources::BufferView>(&image.data)) {
        const fastgltf::BufferView& bv = asset.bufferViews[view->bufferViewIndex];
        return source_bytes(asset.buffers[bv.bufferIndex].data).subspan(bv.byteOffset, bv.byteLength);
    }
    return source_bytes(image.data);
}

/// The glTF JSON text, also inside a .glb, for what fastgltf does not expose (extras).
std::string_view json_chunk(std::string_view bytes) {
    if (bytes.size() >= 20 && bytes.substr(0, 4) == "glTF") {
        std::uint32_t length = 0;
        std::memcpy(&length, bytes.data() + 12, 4);
        return bytes.substr(20, std::min<std::size_t>(length, bytes.size() - 20));
    }
    return bytes;
}

// ---- meshes ------------------------------------------------------------------------------------------------

/// Per-vertex tangents from UV derivatives, accumulated over triangles and orthonormalized (w = handedness).
void generate_tangents(MeshAsset& mesh, std::span<const std::uint8_t> needs) {
    std::vector<math::vec3> tan(mesh.vertices.size(), math::vec3(0.0f));
    std::vector<math::vec3> bitan(mesh.vertices.size(), math::vec3(0.0f));
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const std::array<std::uint32_t, 3> i{mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]};
        const auto p = [&](std::size_t k) {
            return math::vec3(mesh.vertices[i[k]].position[0], mesh.vertices[i[k]].position[1],
                              mesh.vertices[i[k]].position[2]);
        };
        const auto uv = [&](std::size_t k) {
            return math::vec2(mesh.vertices[i[k]].uv[0], mesh.vertices[i[k]].uv[1]);
        };
        const math::vec3 e1 = p(1) - p(0);
        const math::vec3 e2 = p(2) - p(0);
        const math::vec2 d1 = uv(1) - uv(0);
        const math::vec2 d2 = uv(2) - uv(0);
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12f) {
            continue;
        }
        const float r = 1.0f / det;
        const math::vec3 sdir = (e1 * d2.y - e2 * d1.y) * r;
        const math::vec3 tdir = (e2 * d1.x - e1 * d2.x) * r;
        for (const std::uint32_t v : i) {
            tan[v] += sdir;
            bitan[v] += tdir;
        }
    }
    for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
        if (needs[v] == 0) {
            continue;
        }
        MeshVertex& vertex = mesh.vertices[v];
        const math::vec3 n(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        math::vec3 t = tan[v] - n * math::dot(n, tan[v]);
        if (math::dot(t, t) < 1e-12f) {  // no usable UVs: any vector perpendicular to the normal
            t = std::abs(n.x) < 0.9f ? math::cross(n, math::vec3(1, 0, 0)) : math::cross(n, math::vec3(0, 1, 0));
        }
        t = math::normalize(t);
        const float w = math::dot(math::cross(n, t), bitan[v]) < 0.0f ? -1.0f : 1.0f;
        vertex.tangent = {t.x, t.y, t.z, w};
    }
}

Result<MeshAsset> build_mesh(const fastgltf::Asset& asset, const fastgltf::Mesh& gltf_mesh, std::string_view context) {
    MeshAsset mesh;
    std::vector<std::uint8_t> needs_tangent;
    bool first = true;
    for (std::size_t slot = 0; slot < gltf_mesh.primitives.size(); ++slot) {
        const fastgltf::Primitive& prim = gltf_mesh.primitives[slot];
        if (prim.type != fastgltf::PrimitiveType::Triangles) {
            log::warn("asset", "{}: mesh '{}' primitive {} is not triangles; skipped", context, gltf_mesh.name, slot);
            continue;
        }
        const auto position = prim.findAttribute("POSITION");
        if (position == prim.attributes.end() || !prim.indicesAccessor) {
            return make_error(ErrorCode::asset_load_failed, std::format("{}: mesh '{}' primitive {} has no positions",
                                                                        context, gltf_mesh.name, slot));
        }
        std::vector<MeshVertex> vertices(asset.accessors[position->accessorIndex].count);
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
            asset, asset.accessors[position->accessorIndex],
            [&](fastgltf::math::fvec3 v, std::size_t i) { vertices[i].position = {v.x(), v.y(), v.z()}; });
        const auto normal = prim.findAttribute("NORMAL");
        const bool has_normals = normal != prim.attributes.end();
        if (has_normals) {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                asset, asset.accessors[normal->accessorIndex],
                [&](fastgltf::math::fvec3 v, std::size_t i) { vertices[i].normal = {v.x(), v.y(), v.z()}; });
        }
        const auto tangent = prim.findAttribute("TANGENT");
        const bool has_tangents = tangent != prim.attributes.end() && has_normals;
        if (has_tangents) {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
                asset, asset.accessors[tangent->accessorIndex],
                [&](fastgltf::math::fvec4 v, std::size_t i) { vertices[i].tangent = {v.x(), v.y(), v.z(), v.w()}; });
        }
        if (const auto uv = prim.findAttribute("TEXCOORD_0"); uv != prim.attributes.end()) {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
                asset, asset.accessors[uv->accessorIndex],
                [&](fastgltf::math::fvec2 v, std::size_t i) { vertices[i].uv = {v.x(), v.y()}; });
        }
        std::vector<std::uint32_t> indices(asset.accessors[*prim.indicesAccessor].count);
        fastgltf::copyFromAccessor<std::uint32_t>(asset, asset.accessors[*prim.indicesAccessor], indices.data());

        if (!has_normals) {
            // glTF: without normals, clients must use flat normals. That needs one vertex per corner.
            std::vector<MeshVertex> flat;
            for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
                const MeshVertex a = vertices[indices[t]];
                const MeshVertex b = vertices[indices[t + 1]];
                const MeshVertex c = vertices[indices[t + 2]];
                const math::vec3 pa(a.position[0], a.position[1], a.position[2]);
                const math::vec3 n =
                    math::normalize(math::cross(math::vec3(b.position[0], b.position[1], b.position[2]) - pa,
                                                math::vec3(c.position[0], c.position[1], c.position[2]) - pa));
                for (MeshVertex v : {a, b, c}) {
                    v.normal = {n.x, n.y, n.z};
                    flat.push_back(v);
                }
            }
            vertices = std::move(flat);
            indices.resize(vertices.size());
            for (std::size_t i = 0; i < indices.size(); ++i) {
                indices[i] = static_cast<std::uint32_t>(i);
            }
        }

        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        for (const MeshVertex& v : vertices) {
            const math::vec3 p(v.position[0], v.position[1], v.position[2]);
            mesh.bounds_min = first ? p : math::min(mesh.bounds_min, p);
            mesh.bounds_max = first ? p : math::max(mesh.bounds_max, p);
            first = false;
        }
        mesh.vertices.insert(mesh.vertices.end(), vertices.begin(), vertices.end());
        needs_tangent.insert(needs_tangent.end(), vertices.size(), has_tangents ? 0 : 1);
        for (const std::uint32_t i : indices) {
            mesh.indices.push_back(base + i);
        }
        mesh.triangle_slots.insert(mesh.triangle_slots.end(), indices.size() / 3, static_cast<std::uint32_t>(slot));
    }
    mesh.slot_count = static_cast<std::uint32_t>(gltf_mesh.primitives.size());
    generate_tangents(mesh, needs_tangent);
    build_meshlets(mesh);
    return mesh;
}

std::string write_vmesh(const MeshAsset& mesh) {
    std::string out;
    const auto put = [&](const void* data, std::size_t size) {
        out.append(static_cast<const char*>(data), size);
    };
    const std::array<std::uint32_t, 9> header{vmesh_magic,
                                              vmesh_version,
                                              static_cast<std::uint32_t>(mesh.vertices.size()),
                                              static_cast<std::uint32_t>(mesh.indices.size()),
                                              static_cast<std::uint32_t>(mesh.triangle_slots.size()),
                                              mesh.slot_count,
                                              static_cast<std::uint32_t>(mesh.meshlets.size()),
                                              static_cast<std::uint32_t>(mesh.meshlet_vertices.size()),
                                              static_cast<std::uint32_t>(mesh.meshlet_triangles.size())};
    put(header.data(), sizeof(header));
    put(&mesh.bounds_min, sizeof(math::vec3));
    put(&mesh.bounds_max, sizeof(math::vec3));
    put(mesh.vertices.data(), mesh.vertices.size() * sizeof(MeshVertex));
    put(mesh.indices.data(), mesh.indices.size() * 4);
    put(mesh.triangle_slots.data(), mesh.triangle_slots.size() * 4);
    put(mesh.meshlets.data(), mesh.meshlets.size() * sizeof(Meshlet));
    put(mesh.meshlet_vertices.data(), mesh.meshlet_vertices.size() * 4);
    put(mesh.meshlet_triangles.data(), mesh.meshlet_triangles.size() * 4);
    put(mesh.meshlet_bounds.data(), mesh.meshlet_bounds.size() * sizeof(MeshletBounds));
    return out;
}

std::optional<MeshAsset> read_vmesh(std::string_view in) {
    std::array<std::uint32_t, 9> header{};
    if (in.size() < sizeof(header) + 24) {
        return std::nullopt;
    }
    std::memcpy(header.data(), in.data(), sizeof(header));
    if (header[0] != vmesh_magic || header[1] != vmesh_version) {
        return std::nullopt;
    }
    MeshAsset mesh;
    mesh.vertices.resize(header[2]);
    mesh.indices.resize(header[3]);
    mesh.triangle_slots.resize(header[4]);
    mesh.slot_count = header[5];
    mesh.meshlets.resize(header[6]);
    mesh.meshlet_vertices.resize(header[7]);
    mesh.meshlet_triangles.resize(header[8]);
    mesh.meshlet_bounds.resize(header[6]);
    const std::size_t needed = sizeof(header) + 24 + mesh.vertices.size() * sizeof(MeshVertex) +
                               mesh.indices.size() * 4 + mesh.triangle_slots.size() * 4 +
                               mesh.meshlets.size() * (sizeof(Meshlet) + sizeof(MeshletBounds)) +
                               mesh.meshlet_vertices.size() * 4 + mesh.meshlet_triangles.size() * 4;
    if (in.size() != needed) {
        return std::nullopt;
    }
    std::size_t at = sizeof(header);
    const auto get = [&](void* data, std::size_t size) {
        std::memcpy(data, in.data() + at, size);
        at += size;
    };
    get(&mesh.bounds_min, sizeof(math::vec3));
    get(&mesh.bounds_max, sizeof(math::vec3));
    get(mesh.vertices.data(), mesh.vertices.size() * sizeof(MeshVertex));
    get(mesh.indices.data(), mesh.indices.size() * 4);
    get(mesh.triangle_slots.data(), mesh.triangle_slots.size() * 4);
    get(mesh.meshlets.data(), mesh.meshlets.size() * sizeof(Meshlet));
    get(mesh.meshlet_vertices.data(), mesh.meshlet_vertices.size() * 4);
    get(mesh.meshlet_triangles.data(), mesh.meshlet_triangles.size() * 4);
    get(mesh.meshlet_bounds.data(), mesh.meshlet_bounds.size() * sizeof(MeshletBounds));
    return mesh;
}

/// Reads a cooked file, or cooks it and writes it. `cooked` reports which happened.
template <class T, class Read, class Cook, class Write>
Result<T> cached(const fs::Path& file, Read read, Cook cook, Write write, bool& cooked) {
    if (fs::exists(file)) {
        if (auto bytes = fs::read_file(file)) {
            if (auto value = read(*bytes)) {
                return std::move(*value);
            }
        }
        log::warn("asset", "cache entry {} is unreadable; cooking again", file.generic_string());
    }
    cooked = true;
    auto value = cook();
    if (!value) {
        return std::unexpected(value.error());
    }
    if (auto written = fs::write_file(file, write(*value)); !written) {
        log::warn("asset", "cannot write the cook cache: {}", written.error().message);
    }
    return std::move(*value);
}

AlphaMode alpha_mode(fastgltf::AlphaMode mode) {
    switch (mode) {
        case fastgltf::AlphaMode::Mask: return AlphaMode::mask;
        case fastgltf::AlphaMode::Blend: return AlphaMode::blend;
        case fastgltf::AlphaMode::Opaque: break;
    }
    return AlphaMode::opaque;
}

}  // namespace

Result<SceneAsset> GltfLoader::load(const LoadContext& ctx) const {
    VEACH_ZONE("GltfLoader::load");
    const std::string context = ctx.path.generic_string();
    fastgltf::Parser parser(fastgltf::Extensions::KHR_lights_punctual |
                            fastgltf::Extensions::KHR_materials_emissive_strength);
    auto data =
        fastgltf::GltfDataBuffer::FromBytes(reinterpret_cast<const std::byte*>(ctx.bytes.data()), ctx.bytes.size());
    if (data.error() != fastgltf::Error::None) {
        return make_error(ErrorCode::asset_load_failed,
                          std::format("{}: {}", context, fastgltf::getErrorMessage(data.error())));
    }
    auto parsed =
        parser.loadGltf(data.get(), ctx.absolute_path.parent_path(),
                        fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages |
                            fastgltf::Options::DecomposeNodeMatrices | fastgltf::Options::GenerateMeshIndices);
    if (parsed.error() != fastgltf::Error::None) {
        return make_error(ErrorCode::asset_load_failed,
                          std::format("{}: {}", context, fastgltf::getErrorMessage(parsed.error())),
                          "Check the file with the Khronos glTF validator.");
    }
    const fastgltf::Asset& asset = parsed.get();
    // fastgltf trusts the declared sizes; a short buffer (often a Git LFS pointer in place of the .bin) would be read
    // past its end.
    for (std::size_t i = 0; i < asset.buffers.size(); ++i) {
        const std::size_t loaded = source_bytes(asset.buffers[i].data).size();
        if (loaded < asset.buffers[i].byteLength) {
            return make_error(ErrorCode::asset_load_failed,
                              std::format("{}: buffer {} has {} bytes, the file declares {}", context, i, loaded,
                                          asset.buffers[i].byteLength),
                              "If the buffer is a Git LFS file, fetch it (git lfs pull); otherwise re-export the "
                              "asset.");
        }
    }
    for (std::size_t i = 0; i < asset.bufferViews.size(); ++i) {
        const fastgltf::BufferView& bv = asset.bufferViews[i];
        if (bv.bufferIndex >= asset.buffers.size() ||
            bv.byteOffset + bv.byteLength > asset.buffers[bv.bufferIndex].byteLength) {
            return make_error(ErrorCode::asset_load_failed,
                              std::format("{}: buffer view {} lies outside its buffer", context, i),
                              "Check the file with the Khronos glTF validator.");
        }
    }
    if (!asset.skins.empty() || !asset.animations.empty() || !asset.cameras.empty()) {
        log::warn("asset", "{}: skins, animations and cameras are not imported yet", context);
    }

    // The cache key covers every byte the cook reads, the settings and the cooker itself.
    Fnv64 hash;
    hash.add(std::as_bytes(std::span(ctx.bytes.data(), ctx.bytes.size())));
    for (const fastgltf::Buffer& buffer : asset.buffers) {
        hash.add(source_bytes(buffer.data));
    }
    for (const fastgltf::Image& image : asset.images) {
        hash.add(image_bytes(asset, image));
    }
    hash.add(ctx.max_texture_size);
    hash.add(cooker_version);
    const fs::Path cache = ctx.cache_dir / std::format("{:016x}", hash.h);
    std::error_code ec;
    std::filesystem::create_directories(cache, ec);

    SceneAsset scene;
    bool cooked_anything = false;

    // extras.veach_material: fastgltf keeps no extras, so read them from the JSON.
    std::vector<std::string> kinds(asset.materials.size(), "StandardMaterial");
    if (const nlohmann::json json = nlohmann::json::parse(json_chunk(ctx.bytes), nullptr, false);
        json.is_object() && json.contains("materials")) {
        for (std::size_t i = 0; i < kinds.size() && i < json["materials"].size(); ++i) {
            const nlohmann::json& m = json["materials"][i];
            if (m.contains("extras") && m["extras"].contains("veach_material") &&
                m["extras"]["veach_material"].is_string()) {
                kinds[i] = m["extras"]["veach_material"].get<std::string>();
            }
        }
    }

    // Textures: one per (image, sRGB) pair that a material uses.
    std::map<std::pair<std::size_t, bool>, std::uint32_t> texture_index;
    const auto texture_for = [&](const fastgltf::TextureInfo& info, bool srgb) -> std::optional<AssetId> {
        const fastgltf::Texture& texture = asset.textures[info.textureIndex];
        if (!texture.imageIndex) {
            log::warn("asset", "{}: texture {} has no PNG/JPEG image; ignored", context, info.textureIndex);
            return std::nullopt;
        }
        const auto key = std::make_pair(*texture.imageIndex, srgb);
        auto [it, inserted] = texture_index.emplace(key, static_cast<std::uint32_t>(texture_index.size()));
        return AssetId::derive(ctx.id, std::format("texture/{}", it->second));
    };
    std::vector<std::shared_ptr<MaterialAsset>> materials;
    for (std::size_t i = 0; i < asset.materials.size(); ++i) {
        const fastgltf::Material& m = asset.materials[i];
        auto material = std::make_shared<MaterialAsset>();
        material->name = std::string(m.name);
        material->kind = kinds[i];
        const auto& c = m.pbrData.baseColorFactor;
        material->base_color = math::vec4(c[0], c[1], c[2], c[3]);
        material->metallic = m.pbrData.metallicFactor;
        material->roughness = m.pbrData.roughnessFactor;
        material->emissive = math::vec3(m.emissiveFactor[0], m.emissiveFactor[1], m.emissiveFactor[2]);
        material->emissive_strength = m.emissiveStrength;
        material->alpha_mode = alpha_mode(m.alphaMode);
        material->alpha_cutoff = m.alphaCutoff;
        material->double_sided = m.doubleSided;
        if (m.pbrData.baseColorTexture) {
            material->base_color_texture = texture_for(*m.pbrData.baseColorTexture, true);
        }
        if (m.pbrData.metallicRoughnessTexture) {
            material->metallic_roughness_texture = texture_for(*m.pbrData.metallicRoughnessTexture, false);
        }
        if (m.normalTexture) {
            material->normal_texture = texture_for(*m.normalTexture, false);
            material->normal_scale = m.normalTexture->scale;
        }
        if (m.emissiveTexture) {
            material->emissive_texture = texture_for(*m.emissiveTexture, true);
        }
        materials.push_back(std::move(material));
    }
    const auto default_material = static_cast<std::uint32_t>(materials.size());  // for primitives without one
    materials.push_back(std::make_shared<MaterialAsset>(MaterialAsset{.name = "default"}));

    for (const auto& [key, index] : texture_index) {
        const fastgltf::Image& image = asset.images[key.first];
        const std::string label = image.name.empty() ? std::format("image {}", key.first) : std::string(image.name);
        auto texture = cached<TextureAsset>(
            cache / std::format("texture_{}.ktx2", index),
            [&](const std::string& bytes) -> std::optional<TextureAsset> {
                auto t = read_ktx2(bytes, "cache");
                return t ? std::optional<TextureAsset>(std::move(*t)) : std::nullopt;
            },
            [&]() -> Result<TextureAsset> {
                auto decoded = decode_image(image_bytes(asset, image), std::format("{}: {}", context, label));
                if (!decoded) {
                    return std::unexpected(decoded.error());
                }
                return cook_texture(*decoded, {.srgb = key.second, .compress = true, .max_size = ctx.max_texture_size});
            },
            [](const TextureAsset& t) { return write_ktx2(t); }, cooked_anything);
        if (!texture) {
            return std::unexpected(texture.error());
        }
        scene.textures.push_back(AssetId::derive(ctx.id, std::format("texture/{}", index)));
        scene.sub_assets.push_back({std::format("texture/{}", index), asset_type_key<TextureAsset>(),
                                    std::make_shared<const TextureAsset>(std::move(*texture))});
    }

    for (std::size_t i = 0; i < asset.meshes.size(); ++i) {
        auto mesh = cached<MeshAsset>(
            cache / std::format("mesh_{}.vmesh", i), [](const std::string& bytes) { return read_vmesh(bytes); },
            [&]() { return build_mesh(asset, asset.meshes[i], context); }, write_vmesh, cooked_anything);
        if (!mesh) {
            return std::unexpected(mesh.error());
        }
        scene.meshes.push_back(AssetId::derive(ctx.id, std::format("mesh/{}", i)));
        scene.sub_assets.push_back({std::format("mesh/{}", i), asset_type_key<MeshAsset>(),
                                    std::make_shared<const MeshAsset>(std::move(*mesh))});
    }
    for (std::size_t i = 0; i < materials.size(); ++i) {
        const std::string key = i == default_material ? "material/default" : std::format("material/{}", i);
        scene.materials.push_back(AssetId::derive(ctx.id, key));
        scene.sub_assets.push_back({key, asset_type_key<MaterialAsset>(), std::move(materials[i])});
    }

    for (const fastgltf::Light& light : asset.lights) {
        SceneLight out;
        out.name = std::string(light.name);
        out.kind = light.type == fastgltf::LightType::Directional ? LightKind::directional
                   : light.type == fastgltf::LightType::Spot      ? LightKind::spot
                                                                  : LightKind::point;
        out.color = math::vec3(light.color[0], light.color[1], light.color[2]);
        out.intensity = light.intensity;
        out.inner_cone = light.innerConeAngle.value_or(0.0f);
        out.outer_cone = light.outerConeAngle.value_or(0.7853982f);
        scene.lights.push_back(std::move(out));
    }
    for (const fastgltf::Node& node : asset.nodes) {
        SceneNode out;
        out.name = std::string(node.name);
        if (const auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
            out.translation = math::vec3(trs->translation[0], trs->translation[1], trs->translation[2]);
            out.rotation = math::quat(trs->rotation[3], trs->rotation[0], trs->rotation[1], trs->rotation[2]);
            out.scale = math::vec3(trs->scale[0], trs->scale[1], trs->scale[2]);
        }
        if (node.meshIndex) {
            out.mesh = static_cast<std::uint32_t>(*node.meshIndex);
            for (const fastgltf::Primitive& prim : asset.meshes[*node.meshIndex].primitives) {
                out.materials.push_back(prim.materialIndex ? static_cast<std::uint32_t>(*prim.materialIndex)
                                                           : default_material);
            }
        }
        if (node.lightIndex) {
            out.light = static_cast<std::uint32_t>(*node.lightIndex);
        }
        for (const std::size_t child : node.children) {
            out.children.push_back(static_cast<std::uint32_t>(child));
        }
        scene.nodes.push_back(std::move(out));
    }
    const std::size_t scene_index = asset.defaultScene.value_or(0);
    if (scene_index < asset.scenes.size()) {
        for (const std::size_t root : asset.scenes[scene_index].nodeIndices) {
            scene.roots.push_back(static_cast<std::uint32_t>(root));
        }
    }
    scene.from_cache = !cooked_anything;
    log::debug("asset", "imported {}: {} nodes, {} meshes, {} materials, {} textures, {} lights ({})", context,
               scene.nodes.size(), scene.meshes.size(), scene.materials.size(), scene.textures.size(),
               scene.lights.size(), scene.from_cache ? "cooked data from the cache" : "cooked now");
    return scene;
}

}  // namespace veach
