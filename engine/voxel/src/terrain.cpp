// A streamed terrain body (terrain.hpp), promoted from spikes SP-09 (docs/research/spikes/sp-09-descent.md) and
// SP-12 (docs/research/spikes/sp-12-dig-collide.md). Sampling, edits and meshing are canonical; selection and
// scheduling are not, and never change what a chunk holds.

#include <veach/core/hash.hpp>
#include <veach/core/jobs.hpp>
#include <veach/core/profile.hpp>
#include <veach/voxel/terrain.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <unordered_set>

namespace veach::voxel {

namespace {

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::uint64_t absorb(std::uint64_t h, std::uint64_t w) {
    return mix64(h ^ mix64(w + hash_golden));
}

constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}

/// True if `box` holds a lattice point of the chunk's samples (its grid, or a face at half spacing).
bool box_holds_sample(const ChunkKey& key, const LatticeBox& box) {
    const LatticeBox sb = sample_box(key);
    if (!sb.intersects(box)) {
        return false;
    }
    // Face samples sit at half spacing; level 0 has none, so its spacing is the cell.
    const std::int64_t step = key.level == 0 ? 1 : cell_units(key.level) / 2;
    for (std::size_t a = 0; a < 3; ++a) {
        const std::int64_t lo = std::max(sb.lo[a], box.lo[a]);
        const std::int64_t hi = std::min(sb.hi[a], box.hi[a]);
        const std::int64_t first = sb.lo[a] + (floor_div(lo - sb.lo[a] + step - 1, step)) * step;
        if (first > hi) {
            return false;
        }
    }
    return true;
}

}  // namespace

TerrainStreamingBudget TerrainStreamingBudget::from_rate(double chunks_per_second, double frames_per_second) {
    TerrainStreamingBudget b;
    b.jobs_per_frame = static_cast<std::uint32_t>(std::max(1.0, std::floor(chunks_per_second / frames_per_second)));
    return b;
}

WeldReport weld_check(std::span<const ChunkMesh* const> meshes, const std::optional<LatticeBox>& domain) {
    const auto on_boundary = [&](const std::array<std::int64_t, 3>& a, const std::array<std::int64_t, 3>& b) {
        if (!domain) {
            return false;
        }
        for (std::size_t i = 0; i < 3; ++i) {
            for (const std::int64_t plane : {domain->lo[i] * fixed_one, domain->hi[i] * fixed_one}) {
                if (a[i] == plane && b[i] == plane) {
                    return true;
                }
            }
        }
        return false;
    };
    std::vector<std::uint64_t> edges;
    const std::uint64_t seed = hash_id("veach:voxel.edge");
    WeldReport report;
    for (const ChunkMesh* m : meshes) {
        report.triangles += m->indices.size() / 3;
        for (std::size_t t = 0; t + 2 < m->indices.size(); t += 3) {
            for (std::size_t k = 0; k < 3; ++k) {
                const auto& a = m->fixed[m->indices[t + k]];
                const auto& b = m->fixed[m->indices[t + (k + 1) % 3]];
                if (a == b || on_boundary(a, b)) {
                    continue;
                }
                const auto& lo = a < b ? a : b;
                const auto& hi = a < b ? b : a;
                std::uint64_t h = seed;
                for (const std::int64_t v : {lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]}) {
                    h = absorb(h, static_cast<std::uint64_t>(v));
                }
                edges.push_back(h);
            }
        }
    }
    std::ranges::sort(edges);
    for (std::size_t i = 0; i < edges.size();) {
        std::size_t j = i;
        while (j < edges.size() && edges[j] == edges[i]) {
            ++j;
        }
        ++report.edges;
        report.open += j - i == 1 ? 1 : 0;
        report.non_manifold += j - i > 2 ? 1 : 0;
        i = j;
    }
    return report;
}

struct TerrainBody::Job {
    ChunkSlot slot;
    ChunkRecord record;
    double sample_ms = 0.0;
    double mesh_ms = 0.0;
};

TerrainBody::TerrainBody(std::shared_ptr<const DensitySource> source, TerrainSettings settings)
    : source_(std::move(source)),
      settings_(std::move(settings)),
      selector_(settings_.octree),
      edits_(source_->encoding()),
      lambda_(settings_.lambda) {
    settings_.mesher.encoding = source_->encoding();
}

TerrainBody::~TerrainBody() {
    if (group_) {
        group_->wait();
    }
}

void TerrainBody::set_payload(ChunkPayload payload) {
    wait();
    settings_.payload = std::move(payload);
}

void TerrainBody::set_viewers(std::span<const std::array<double, 3>> viewers) {
    viewers_.assign(viewers.begin(), viewers.end());
}

IsoMesher& TerrainBody::mesher(std::size_t thread) {
    return *meshers_[thread];
}

BoxClass TerrainBody::classify_key(const ChunkKey& key) const {
    const LatticeBox box = sample_box(key);
    if (edits_.touches(box)) {
        return BoxClass::surface;  // edited samples: only sampling can tell
    }
    return source_->classify(box);
}

void TerrainBody::classify(std::span<const ChunkKey> keys, std::span<BoxClass> out, JobSystem& jobs) const {
    jobs.parallel_for("voxel: classify", keys.size(), 8, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            out[i] = classify_key(keys[i]);
        }
    });
}

void TerrainBody::run_job(Job& job, IsoMesher& mesher) const {
    VEACH_ZONE("voxel: chunk job");
    const ChunkNeighbors neighbors{.finer = job.slot.finer};
    const double t0 = now_ms();
    ChunkSamples samples;
    source_->sample(job.slot.key, mesher.face_samples_needed(neighbors), samples);
    edits_.overlay(samples);
    const double t1 = now_ms();
    ChunkRecord& r = job.record;
    r.slot = job.slot;
    if (samples.has_surface) {
        mesher.mesh(samples, neighbors, r.mesh);
    } else {
        r.mesh.clear();
        r.mesh.key = job.slot.key;
    }
    r.mesh_hash = mesh_hash(r.mesh);
    if (settings_.payload && !r.mesh.indices.empty()) {
        r.payload = settings_.payload(r);
    }
    job.sample_ms = t1 - t0;
    job.mesh_ms = now_ms() - t1;
}

ChunkRecord TerrainBody::build(const ChunkSlot& slot) const {
    auto m = make_transvoxel_mesher(settings_.mesher);
    Job job{.slot = slot};
    run_job(job, *m);
    return std::move(job.record);
}

ChunkSamples TerrainBody::samples(const ChunkKey& key, std::uint8_t face_mask) const {
    ChunkSamples s;
    source_->sample(key, face_mask, s);
    edits_.overlay(s);
    return s;
}

void TerrainBody::collect(StreamStats* stats) {
    if (!group_) {
        return;
    }
    group_->wait();
    for (auto& job : in_flight_) {
        job->record.serial = ++serial_;
        if (stats != nullptr) {
            ++stats->completed;
            stats->empty += job->record.mesh.indices.empty() ? 1 : 0;
            stats->sample_ms += job->sample_ms;
            stats->mesh_ms += job->mesh_ms;
        }
        auto ref = std::make_shared<const ChunkRecord>(std::move(job->record));
        Entry& e = cache_[job->slot];
        if (e.record) {
            evicted_.push_back(e.record);  // the stale mesh it replaces
        }
        e = Entry{ref, false};
        ready_.push_back(std::move(ref));
    }
    in_flight_.clear();
    group_.reset();
}

void TerrainBody::wait() {
    collect(nullptr);
}

void TerrainBody::launch(std::vector<ChunkSlot> slots, JobSystem& jobs) {
    if (slots.empty()) {
        return;
    }
    while (meshers_.size() < jobs.thread_count()) {
        meshers_.push_back(make_transvoxel_mesher(settings_.mesher));
    }
    group_ = std::make_unique<TaskGroup>(jobs);
    for (const ChunkSlot& slot : slots) {
        in_flight_.push_back(std::make_unique<Job>(Job{.slot = slot}));
    }
    for (auto& job : in_flight_) {
        Job* j = job.get();
        group_->run("voxel: chunk", [this, j, &jobs] { run_job(*j, mesher(jobs.thread_index())); });
    }
}

StreamStats TerrainBody::update(JobSystem& jobs) {
    VEACH_ZONE("voxel: terrain update");
    const double t0 = now_ms();
    StreamStats stats;
    stats.update = ++updates_;
    ready_.clear();
    evicted_.clear();
    collect(&stats);

    const std::uint64_t classified_before = selector_.classified();
    const double ts = now_ms();
    std::vector<ChunkSlot> target =
        selector_.select(viewers_, lambda_, settings_.hysteresis,
                         [&](std::span<const ChunkKey> keys, std::span<BoxClass> out) { classify(keys, out, jobs); });
    stats.select_ms = now_ms() - ts;
    stats.classified = static_cast<std::uint32_t>(selector_.classified() - classified_before);
    stats.target = static_cast<std::uint32_t>(target.size());
    stats.lambda = lambda_;

    // Demand: what the selection asks for that the previous one did not.
    {
        std::unordered_set<ChunkKey, ChunkKeyHash> previous_keys;
        for (const ChunkSlot& s : previous_target_) {
            previous_keys.insert(s.key);
        }
        const std::unordered_set<ChunkSlot, ChunkSlotHash> previous(previous_target_.begin(), previous_target_.end());
        for (const ChunkSlot& s : target) {
            if (!previous.contains(s)) {
                ++stats.requests;
                stats.remeshes += previous_keys.contains(s.key) ? 1 : 0;
            }
        }
    }

    // Missing entries, coarse levels first (they cover the most), then nearest the viewers, then by key.
    std::vector<std::pair<double, ChunkSlot>> missing;
    for (const ChunkSlot& s : target) {
        const auto it = cache_.find(s);
        if (it == cache_.end() || it->second.stale) {
            const auto o = chunk_origin(s.key);
            const double half = 0.5 * static_cast<double>(chunk_units(s.key.level));
            double best = 0.0;
            for (std::size_t v = 0; v < viewers_.size(); ++v) {
                double d2 = 0.0;
                for (std::size_t a = 0; a < 3; ++a) {
                    const double d = (static_cast<double>(o[a]) + half) * lod0_cell_meters - viewers_[v][a];
                    d2 += d * d;
                }
                best = v == 0 ? d2 : std::min(best, d2);
            }
            missing.emplace_back(best, s);
        }
    }
    std::ranges::sort(missing, [](const auto& a, const auto& b) {
        if (a.second.key.level != b.second.key.level) {
            return a.second.key.level > b.second.key.level;
        }
        if (a.first != b.first) {
            return a.first < b.first;
        }
        return a.second < b.second;
    });
    const std::size_t count = std::min<std::size_t>(missing.size(), settings_.budget.jobs_per_frame);
    std::vector<ChunkSlot> now;
    now.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        now.push_back(missing[i].second);
    }
    stats.launched = static_cast<std::uint32_t>(count);
    stats.backlog = static_cast<std::uint32_t>(missing.size() - count);
    launch(std::move(now), jobs);
    if (!settings_.asynchronous) {
        collect(&stats);
    }

    // Never a hole: switch only to a complete selection.
    bool complete = true;
    for (const ChunkSlot& s : target) {
        const auto it = cache_.find(s);
        if (it == cache_.end() || it->second.stale) {
            complete = false;
            break;
        }
    }
    if (complete) {
        drawn_.clear();
        drawn_.reserve(target.size());
        for (const ChunkSlot& s : target) {
            drawn_.push_back(cache_.at(s).record);
        }
        stale_ = 0;
    } else {
        ++stale_;
    }
    // A drawn set lagging past the budget's patience lowers the effective lambda, so the selection needs fewer fine
    // chunks and can complete; it recovers slowly once the stream keeps up (ADR 0053, 0079 rule 3).
    const TerrainStreamingBudget& b = settings_.budget;
    if (stale_ > b.patience) {
        lambda_ = std::max(b.min_lambda, lambda_ * b.lower);
    } else if (stale_ == 0) {
        lambda_ = std::min(settings_.lambda, lambda_ * b.recover);
    }
    stats.stale = stale_;

    // Evict what neither the drawn set nor the selection uses (and nothing in flight).
    {
        std::unordered_set<ChunkSlot, ChunkSlotHash> keep(target.begin(), target.end());
        for (const ChunkRef& r : drawn_) {
            keep.insert(r->slot);
        }
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (!keep.contains(it->first)) {
                if (it->second.record) {
                    evicted_.push_back(it->second.record);
                }
                it = cache_.erase(it);
            } else {
                ++it;
            }
        }
    }
    previous_target_ = std::move(target);
    stats.drawn = static_cast<std::uint32_t>(drawn_.size());
    stats.cached = static_cast<std::uint32_t>(cache_.size());
    stats.update_ms = now_ms() - t0;
    return stats;
}

std::uint64_t TerrainBody::drawn_hash() const {
    std::vector<std::uint64_t> hashes;
    hashes.reserve(drawn_.size());
    for (const ChunkRef& r : drawn_) {
        hashes.push_back(r->mesh_hash);
    }
    return combine_hashes(hashes);
}

WeldReport TerrainBody::weld() const {
    std::vector<const ChunkMesh*> meshes;
    meshes.reserve(drawn_.size());
    for (const ChunkRef& r : drawn_) {
        meshes.push_back(&r->mesh);
    }
    const OctreeBounds& o = settings_.octree;
    const std::int64_t e = chunk_units(o.root_level);
    LatticeBox domain;
    for (std::size_t a = 0; a < 3; ++a) {
        domain.lo[a] = o.root_min[a] * e;
        domain.hi[a] = (o.root_max[a] + 1) * e;
    }
    return weld_check(meshes, domain);
}

void TerrainBody::invalidate(const LatticeBox& box) {
    selector_.invalidate(box);
    for (auto& [slot, entry] : cache_) {
        if (box_holds_sample(slot.key, box)) {
            entry.stale = true;
        }
    }
    for (auto& [key, chunk] : collider_meshes_) {
        if (box_holds_sample(key, box)) {
            chunk.stale = true;
        }
    }
}

Result<EditResult> TerrainBody::apply(const BrushOp& op, JobSystem& jobs) {
    wait();  // jobs in flight read the store
    auto result = edits_.apply(op, *source_, jobs);
    if (result && !result->changed.empty()) {
        invalidate(result->box);
    }
    return result;
}

bool TerrainBody::restore(const OverrideRecord& record) {
    wait();
    const bool matched = edits_.restore(record, *source_);
    const auto o = chunk_origin(record.key);
    invalidate({o, {o[0] + chunk_cells - 1, o[1] + chunk_cells - 1, o[2] + chunk_cells - 1}});
    return matched;
}

namespace {

/// Distance from a point (LOD0 units) to a chunk's box.
double box_distance(const ChunkKey& k, const std::array<double, 3>& p) {
    const auto o = chunk_origin(k);
    const auto e = static_cast<double>(chunk_units(k.level));
    double d2 = 0.0;
    for (std::size_t a = 0; a < 3; ++a) {
        const auto lo = static_cast<double>(o[a]);
        const double d = p[a] < lo ? lo - p[a] : (p[a] > lo + e ? p[a] - lo - e : 0.0);
        d2 += d * d;
    }
    return std::sqrt(d2);
}

/// Distance from a segment to a chunk's box (convex in t: ternary search).
double segment_distance(const ChunkKey& k, const std::array<double, 3>& a, const std::array<double, 3>& b) {
    const auto at = [&](double t) {
        return box_distance(k, {a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), a[2] + t * (b[2] - a[2])});
    };
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 40; ++i) {
        const double m1 = lo + (hi - lo) / 3.0, m2 = hi - (hi - lo) / 3.0;
        if (at(m1) <= at(m2)) {
            hi = m2;
        } else {
            lo = m1;
        }
    }
    return std::min({at(0.0), at(1.0), at(0.5 * (lo + hi))});
}

}  // namespace

ColliderStats TerrainBody::update_colliders(std::span<const ColliderProbe> probes, std::uint64_t tick,
                                            JobSystem& jobs) {
    VEACH_ZONE("voxel: colliders");
    ColliderStats stats;
    // The chunks within reach of each probe's path over the lookahead.
    std::map<ChunkKey, bool> wanted;  // key -> may hold a surface
    const double reach = settings_.collider_reach_m / lod0_cell_meters;
    for (const ColliderProbe& p : probes) {
        std::array<double, 3> a{}, b{};
        for (std::size_t i = 0; i < 3; ++i) {
            a[i] = p.position_m[i] / lod0_cell_meters;
            b[i] = (p.position_m[i] + p.velocity_m_s[i] * settings_.collider_lookahead_s) / lod0_cell_meters;
        }
        std::array<std::int64_t, 3> lo{}, hi{};
        for (std::size_t i = 0; i < 3; ++i) {
            lo[i] = floor_div(static_cast<std::int64_t>(std::floor(std::min(a[i], b[i]) - reach)), chunk_cells);
            hi[i] = floor_div(static_cast<std::int64_t>(std::floor(std::max(a[i], b[i]) + reach)), chunk_cells);
        }
        for (std::int64_t z = lo[2]; z <= hi[2]; ++z) {
            for (std::int64_t y = lo[1]; y <= hi[1]; ++y) {
                for (std::int64_t x = lo[0]; x <= hi[0]; ++x) {
                    const ChunkKey k{0, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                                     static_cast<std::int32_t>(z)};
                    if (!wanted.contains(k) && segment_distance(k, a, b) <= reach) {
                        wanted.emplace(k, false);
                    }
                }
            }
        }
    }
    // Their neighbors feed the rings.
    std::map<ChunkKey, bool> needed = wanted;
    for (const auto& [k, unused] : wanted) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    needed.emplace(ChunkKey{0, k.x + dx, k.y + dy, k.z + dz}, false);
                }
            }
        }
    }
    std::vector<ChunkKey> keys;
    for (const auto& [k, unused] : needed) {
        keys.push_back(k);
    }
    std::vector<BoxClass> classes(keys.size(), BoxClass::surface);
    classify(keys, classes, jobs);
    std::vector<ChunkKey> to_mesh;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        needed[keys[i]] = classes[i] == BoxClass::surface;
        if (const auto w = wanted.find(keys[i]); w != wanted.end()) {
            w->second = classes[i] == BoxClass::surface;
        }
        const auto it = collider_meshes_.find(keys[i]);
        if (classes[i] == BoxClass::surface && (it == collider_meshes_.end() || it->second.stale)) {
            to_mesh.push_back(keys[i]);
        }
    }
    // LOD0 meshes, one chunk per job, results in indexed slots.
    while (meshers_.size() < jobs.thread_count()) {
        meshers_.push_back(make_transvoxel_mesher(settings_.mesher));
    }
    std::vector<Job> meshed(to_mesh.size());
    jobs.parallel_for("voxel: collider meshes", to_mesh.size(), 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            meshed[i].slot = {to_mesh[i], 0};
            run_job(meshed[i], mesher(jobs.thread_index()));
        }
    });
    for (Job& j : meshed) {
        collider_meshes_[j.slot.key] = {std::move(j.record.mesh), j.record.mesh_hash, false};
    }
    stats.meshed = static_cast<std::uint32_t>(to_mesh.size());
    std::erase_if(collider_meshes_, [&](const auto& e) {
        const auto it = needed.find(e.first);
        return it == needed.end() || !it->second;
    });

    // Each wanted chunk's collider: its triangles, then its neighbors' triangles touching its box (ADR 0083).
    ColliderUpdate update{.tick = tick, .install_tick = tick + settings_.collider_delay_ticks};
    for (const auto& [k, surface] : wanted) {
        const auto own = collider_meshes_.find(k);
        if (!surface || own == collider_meshes_.end() || own->second.mesh.indices.empty()) {
            continue;
        }
        ++stats.wanted;
        ColliderMesh c{.key = k};
        std::map<std::array<std::int64_t, 3>, std::uint32_t> index;
        const auto vertex = [&](const std::array<std::int64_t, 3>& p) {
            const auto [it, inserted] = index.emplace(p, static_cast<std::uint32_t>(c.positions.size()));
            if (inserted) {
                c.positions.push_back(p);
            }
            return it->second;
        };
        const auto o = chunk_origin(k);
        const std::int64_t e = chunk_units(0);
        const auto touches = [&](const std::array<std::int64_t, 3>& p) {
            bool on_face = false;
            for (std::size_t a = 0; a < 3; ++a) {
                const std::int64_t lo = o[a] * fixed_one, hi = (o[a] + e) * fixed_one;
                if (p[a] < lo || p[a] > hi) {
                    return false;
                }
                on_face = on_face || p[a] == lo || p[a] == hi;
            }
            return on_face;
        };
        std::uint64_t h = absorb(hash_id("veach:voxel.collider"), own->second.hash);
        for (int pass = 0; pass < 2; ++pass) {
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const bool self = dx == 0 && dy == 0 && dz == 0;
                        if (self != (pass == 0)) {
                            continue;
                        }
                        const auto it = collider_meshes_.find({0, k.x + dx, k.y + dy, k.z + dz});
                        if (it == collider_meshes_.end()) {
                            continue;
                        }
                        const ChunkMesh& m = it->second.mesh;
                        for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
                            const auto& a = m.fixed[m.indices[t]];
                            const auto& b = m.fixed[m.indices[t + 1]];
                            const auto& d = m.fixed[m.indices[t + 2]];
                            if (self || touches(a) || touches(b) || touches(d)) {
                                c.indices.insert(c.indices.end(), {vertex(a), vertex(b), vertex(d)});
                            }
                        }
                        if (!self) {
                            h = absorb(h, it->second.hash);
                        }
                    }
                }
            }
            if (pass == 0) {
                c.own_triangles = static_cast<std::uint32_t>(c.indices.size() / 3);
            }
        }
        c.hash = absorb(h, c.indices.size());
        const auto installed = installed_.find(k);
        if (installed == installed_.end() || installed->second != c.hash) {
            installed_[k] = c.hash;
            update.install.push_back(std::move(c));
        }
    }
    for (auto it = installed_.begin(); it != installed_.end();) {
        const auto w = wanted.find(it->first);
        const auto own = collider_meshes_.find(it->first);
        if (w == wanted.end() || !w->second || own == collider_meshes_.end() || own->second.mesh.indices.empty()) {
            update.remove.push_back(it->first);
            it = installed_.erase(it);
        } else {
            ++it;
        }
    }
    stats.installed = static_cast<std::uint32_t>(update.install.size());
    stats.removed = static_cast<std::uint32_t>(update.remove.size());
    if (sink_ != nullptr && (!update.install.empty() || !update.remove.empty())) {
        sink_->submit(std::move(update));
    }
    return stats;
}

}  // namespace veach::voxel
