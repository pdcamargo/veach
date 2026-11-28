// The binding between veach::voxel terrain bodies, reference frames and the renderer (terrain.hpp).

#include <veach/runtime/terrain.hpp>

#include <veach/core/hash.hpp>
#include <veach/core/jobs.hpp>
#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>
#include <veach/ecs/components.hpp>
#include <veach/ecs/frames.hpp>
#include <veach/ecs/phases.hpp>
#include <veach/ecs/services.hpp>
#include <veach/ecs/transform.hpp>
#include <veach/render/mesh_store.hpp>
#include <veach/render/renderer.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace veach {

namespace vx = voxel;

struct TerrainBodies::Body {
    std::uint64_t frame = 0;
    std::shared_ptr<vx::TerrainBody> body;
    TerrainLook look;
    std::unordered_map<std::uint64_t, AssetId> meshes;  ///< by ChunkRecord::serial
    vx::StreamStats stats;
};

TerrainBodies::TerrainBodies() = default;
TerrainBodies::~TerrainBodies() {
    for (auto& [frame, b] : bodies_) {
        b->body->wait();
    }
}

namespace {

/// A chunk's render mesh, built on its job: positions relative to the chunk origin (exact from the lattice
/// coordinates), unit normals, a tangent, and one material slot per triangle (its vertices' highest material id).
vx::ChunkPayload render_payload(std::vector<AssetId> materials) {
    return [materials = std::move(materials)](const vx::ChunkRecord& record) -> std::shared_ptr<const void> {
        const vx::ChunkMesh& m = record.mesh;
        render::MeshData data;
        data.vertices.resize(m.fixed.size());
        for (std::size_t v = 0; v < m.fixed.size(); ++v) {
            MeshVertex& out = data.vertices[v];
            out.position = m.position[v];
            math::vec3 n(m.normal[v][0], m.normal[v][1], m.normal[v][2]);
            const float length = math::length(n);
            n = length > 0.0f ? n / length : math::vec3(0.0f, 1.0f, 0.0f);
            out.normal = {n.x, n.y, n.z};
            const math::vec3 t = math::normalize(
                math::cross(std::abs(n.y) < 0.9f ? math::vec3(0.0f, 1.0f, 0.0f) : math::vec3(1.0f, 0.0f, 0.0f), n));
            out.tangent = {t.x, t.y, t.z, 1.0f};
        }
        data.indices = m.indices;
        const std::size_t triangles = m.indices.size() / 3;
        const std::uint32_t last = materials.empty() ? 0 : static_cast<std::uint32_t>(materials.size() - 1);
        data.triangle_slots.resize(triangles);
        for (std::size_t t = 0; t < triangles; ++t) {
            const std::uint32_t slot = std::max(
                {m.material[m.indices[3 * t]], m.material[m.indices[3 * t + 1]], m.material[m.indices[3 * t + 2]]});
            data.triangle_slots[t] = std::min(slot, last);
        }
        data.materials = materials;
        Result<render::RuntimeMesh> built = render::build_runtime_mesh(std::move(data));
        if (!built) {
            log::error("terrain", built.error());
            return nullptr;
        }
        return std::make_shared<const render::RuntimeMesh>(std::move(*built));
    };
}

/// A draw id per body and chunk, stable while the chunk is drawn.
std::uint64_t draw_id(std::uint64_t frame, const vx::ChunkKey& key) {
    std::uint64_t h = mix64(frame ^ 0x7E44'0000'0000'0000ULL);
    h = mix64(h ^ key.level);
    h = mix64(h ^ static_cast<std::uint32_t>(key.x));
    h = mix64(h ^ (std::uint64_t{static_cast<std::uint32_t>(key.y)} << 32));
    return mix64(h ^ static_cast<std::uint32_t>(key.z));
}

}  // namespace

vx::TerrainBody& TerrainBodies::add(Entity frame, std::shared_ptr<vx::TerrainBody> body, TerrainLook look) {
    const std::uint64_t id = frame.is_valid() ? frame.id() : 0;
    body->set_payload(render_payload(look.materials));
    auto b = std::make_unique<Body>();
    b->frame = id;
    b->body = std::move(body);
    b->look = std::move(look);
    vx::TerrainBody& out = *b->body;
    bodies_[id] = std::move(b);
    return out;
}

void TerrainBodies::remove(Entity frame) {
    bodies_.erase(frame.is_valid() ? frame.id() : 0);
}

vx::TerrainBody* TerrainBodies::find(Entity frame) const {
    const auto it = bodies_.find(frame.is_valid() ? frame.id() : 0);
    return it == bodies_.end() ? nullptr : it->second->body.get();
}

const vx::StreamStats* TerrainBodies::stats(Entity frame) const {
    const auto it = bodies_.find(frame.is_valid() ? frame.id() : 0);
    return it == bodies_.end() ? nullptr : &it->second->stats;
}

std::vector<std::uint64_t> TerrainBodies::frames() const {
    std::vector<std::uint64_t> out;
    for (const auto& [frame, b] : bodies_) {
        out.push_back(frame);
    }
    return out;
}

TerrainModule::TerrainModule(World& world) {
    world.module<TerrainModule>();
    world.component<TerrainViewer>().add<Scriptable>();

    auto bodies = std::make_shared<TerrainBodies>();
    services(world).provide<TerrainBodies>(*bodies);
    auto viewers = world.query_builder<const FrameTransform>().with<TerrainViewer>().build();

    // After the last propagation of the frame: viewers in, meshes to the MeshStore.
    world.system("TerrainStream").kind<phase::PreExtract>().run([bodies, viewers](Iter& it) {
        VEACH_ZONE("terrain: stream");
        World w = it.world();
        Services& registry = services(w);
        auto& jobs = registry.get<JobSystem>();
        auto* store = registry.find<render::MeshStore>();
        for (auto& [frame_id, b] : bodies->bodies()) {
            const Entity frame = frame_id != 0 ? w.entity(frame_id) : Entity{};
            std::vector<std::array<double, 3>> positions;
            std::unordered_map<std::uint64_t, std::optional<FramePose>> poses;
            viewers.each([&](Entity, const FrameTransform& pose) {
                auto found = poses.find(pose.frame);
                if (found == poses.end()) {
                    const Entity from = pose.frame != 0 ? w.entity(pose.frame) : Entity{};
                    Result<FramePose> p = frame_pose(w, from, frame);
                    found = poses.emplace(pose.frame, p ? std::optional<FramePose>(*p) : std::nullopt).first;
                }
                if (found->second) {
                    const math::dvec3 p = found->second->point(pose.translation);
                    positions.push_back({p.x, p.y, p.z});
                }
            });
            b->body->set_viewers(positions);
            b->stats = b->body->update(jobs);
            if (store == nullptr) {
                continue;  // no renderer (a server): nothing to upload
            }
            for (const vx::ChunkRef& r : b->body->ready()) {
                if (r->payload) {
                    b->meshes[r->serial] = store->add(std::static_pointer_cast<const render::RuntimeMesh>(r->payload));
                }
            }
            for (const vx::ChunkRef& r : b->body->evicted()) {
                if (const auto m = b->meshes.find(r->serial); m != b->meshes.end()) {
                    static_cast<void>(store->remove(m->second));
                    b->meshes.erase(m);
                }
            }
        }
    });
    world.system("TerrainShutdown").kind<phase::Shutdown>().run([bodies](Iter& it) {
        services(it.world()).revoke<TerrainBodies>();
        for (auto& [frame, b] : bodies->bodies()) {
            b->body->wait();
        }
    });

    if (auto* renderer = find_services(world) ? find_services(world)->find<render::Renderer>() : nullptr) {
        renderer->add_extractor("terrain", [bodies](const World& w, render::RenderWorld& out) {
            if (!out.runtime_assets) {
                return;
            }
            render::RenderSpaceMap space(w, out.space);
            for (auto& [frame_id, b] : bodies->bodies()) {
                const std::optional<math::dmat3> rotation = space.rotation(frame_id);
                if (!rotation) {
                    continue;  // the frame is gone
                }
                const math::mat3 axes(*rotation);
                for (const vx::ChunkRef& r : b->body->drawn()) {
                    const auto id = b->meshes.find(r->serial);
                    if (id == b->meshes.end()) {
                        continue;  // no triangles
                    }
                    const auto mesh = out.runtime_assets->meshes.find(id->second);
                    if (mesh == out.runtime_assets->meshes.end()) {
                        continue;
                    }
                    // The chunk origin: lattice integers to the frame's meters in double, then render space.
                    const auto o = vx::chunk_origin(r->slot.key);
                    const math::dvec3 origin(static_cast<double>(o[0]) * vx::lod0_cell_meters,
                                             static_cast<double>(o[1]) * vx::lod0_cell_meters,
                                             static_cast<double>(o[2]) * vx::lod0_cell_meters);
                    const std::optional<math::vec3> at = space.point(frame_id, origin);
                    if (!at) {
                        continue;
                    }
                    math::mat4 matrix(axes);
                    matrix[3] = math::vec4(*at, 1.0f);
                    out.meshes.push_back({.mesh = id->second,
                                          .materials = mesh->second->materials,
                                          .world = matrix,
                                          .entity = draw_id(frame_id, r->slot.key)});
                }
            }
        });
    }
}

}  // namespace veach
