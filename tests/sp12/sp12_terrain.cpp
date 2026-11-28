// veach: canonical
// Spike SP-12 (VCH-088), throwaway: see sp12_terrain.hpp.

#include "sp12/sp12_terrain.hpp"

#include <veach/core/hash.hpp>
#include <veach/core/profile.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace veach::sp12 {

using namespace voxel;

namespace {

double smooth(double t) {
    return t * t * (3.0 - 2.0 * t);
}

double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}

}  // namespace

double PlanetField::distance(double px, double py, double pz) const {
    const auto lattice = [&](std::uint64_t octave, std::int64_t x, std::int64_t y, std::int64_t z) {
        return unit_double(hash(seed, octave, static_cast<std::uint64_t>(x), static_cast<std::uint64_t>(y),
                                static_cast<std::uint64_t>(z)));
    };
    // Value noise in [-1, 1) with wavelength 1 in the units of p.
    const auto noise = [&](std::uint64_t octave, double x, double y, double z) {
        const double fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
        const auto ix = static_cast<std::int64_t>(fx), iy = static_cast<std::int64_t>(fy),
                   iz = static_cast<std::int64_t>(fz);
        const double tx = smooth(x - fx), ty = smooth(y - fy), tz = smooth(z - fz);
        const auto lerp = [](double a, double b, double t) {
            return a + (b - a) * t;
        };
        const double x00 = lerp(lattice(octave, ix, iy, iz), lattice(octave, ix + 1, iy, iz), tx);
        const double x10 = lerp(lattice(octave, ix, iy + 1, iz), lattice(octave, ix + 1, iy + 1, iz), tx);
        const double x01 = lerp(lattice(octave, ix, iy, iz + 1), lattice(octave, ix + 1, iy, iz + 1), tx);
        const double x11 = lerp(lattice(octave, ix, iy + 1, iz + 1), lattice(octave, ix + 1, iy + 1, iz + 1), tx);
        return 2.0 * lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz) - 1.0;
    };
    const double r = std::sqrt(px * px + py * py + pz * pz);
    const double n = 6.0 * noise(1, px / 48.0, py / 48.0, pz / 48.0) + 1.5 * noise(2, px / 12.0, py / 12.0, pz / 12.0) +
                     0.35 * noise(3, px / 3.0, py / 3.0, pz / 3.0);
    return r - radius + n;
}

Sample PlanetField::sample(std::int64_t x, std::int64_t y, std::int64_t z) const {
    const double px = static_cast<double>(x) * lod0_cell_meters, py = static_cast<double>(y) * lod0_cell_meters,
                 pz = static_cast<double>(z) * lod0_cell_meters;
    const double d = distance(px, py, pz);
    const double q = std::clamp(std::floor(d / distance_quantum + 0.5), -32767.0, 32767.0);
    return Sample{static_cast<std::int16_t>(q), static_cast<std::uint8_t>(d < -3.0 ? 2 : 1)};  // rock under regolith
}

bool PlanetField::may_have_surface(const ChunkKey& k) const {
    const auto o = chunk_origin(k);
    double near2 = 0.0, far2 = 0.0;
    for (std::size_t a = 0; a < 3; ++a) {
        const double lo = static_cast<double>(o[a] - 1) * lod0_cell_meters;
        const double hi = static_cast<double>(o[a] + chunk_units(k.level) + 1) * lod0_cell_meters;
        const double n = lo > 0.0 ? lo : (hi < 0.0 ? -hi : 0.0);
        const double f = std::max(std::abs(lo), std::abs(hi));
        near2 += n * n;
        far2 += f * f;
    }
    const double margin = relief + 1.0;
    return std::sqrt(near2) <= radius + margin && std::sqrt(far2) >= radius - margin;
}

std::array<std::int64_t, 3> site_offset_mm(std::int64_t east, std::int64_t north, std::int64_t up) {
    return {dig_site_mm[0] + east * 4 / 5 + up * 3 / 5, dig_site_mm[1] - east * 3 / 5 + up * 4 / 5,
            dig_site_mm[2] + north};
}

std::array<std::int64_t, 3> site_offset_mm(const Site& site, std::int64_t east, std::int64_t north, std::int64_t up) {
    std::array<std::int64_t, 3> p{};
    for (std::size_t a = 0; a < 3; ++a) {
        const double d = site.east[a] * static_cast<double>(east) + site.north[a] * static_cast<double>(north) +
                         site.up[a] * static_cast<double>(up);
        p[a] = site.center_mm[a] + static_cast<std::int64_t>(std::nearbyint(d));
    }
    return p;
}

ChunkKey chunk_of_mm(const std::array<std::int64_t, 3>& p) {
    constexpr std::int64_t chunk_mm = chunk_cells * lod0_cell_mm;
    return ChunkKey{0, static_cast<std::int32_t>(floor_div(p[0], chunk_mm)),
                    static_cast<std::int32_t>(floor_div(p[1], chunk_mm)),
                    static_cast<std::int32_t>(floor_div(p[2], chunk_mm))};
}

physics::ChunkKey physics_key(const ChunkKey& key) {
    return {.level = key.level, .x = key.x, .y = key.y, .z = key.z};
}

physics::ColliderMesh collider_mesh(const ChunkMesh& mesh) {
    physics::ColliderMesh out;
    out.key = physics_key(mesh.key);
    out.origin = chunk_origin(mesh.key);
    out.extent = chunk_units(mesh.key.level);
    out.vertices = mesh.fixed;
    out.indices = mesh.indices;
    return out;
}

Terrain::Terrain(const PlanetField& field, ChunkKey center, int half) : field_(field), center_(center) {
    for (int x = -half; x <= half; ++x) {
        for (int y = -half; y <= half; ++y) {
            for (int z = -half; z <= half; ++z) {
                Chunk c;
                c.samples.key = ChunkKey{0, center.x + x, center.y + y, center.z + z};
                c.mesh.key = c.samples.key;
                index_.emplace(c.samples.key, chunks_.size());
                chunks_.push_back(std::move(c));
            }
        }
    }
}

IsoMesher& Terrain::mesher(JobSystem& jobs) {
    return *meshers_[jobs.thread_index()];
}

void Terrain::produce(std::size_t index, bool sample, IsoMesher& mesher) {
    Chunk& c = chunks_[index];
    if (sample) {
        VEACH_ZONE("sp12 sample chunk");
        const double t0 = now_ms();
        sample_chunk(field_, c.samples.key, 0, c.samples);
        c.sampled = true;
        c.times.sample = now_ms() - t0;
    }
    VEACH_ZONE("sp12 mesh chunk");
    const double t0 = now_ms();
    mesher.mesh(c.samples, ChunkNeighbors{}, c.mesh);
    c.times.mesh = now_ms() - t0;
}

void Terrain::load(JobSystem& jobs, bool eager) {
    while (meshers_.size() < jobs.thread_count()) {
        meshers_.push_back(make_transvoxel_mesher({}));
    }
    std::vector<std::size_t> todo;
    for (std::size_t i = 0; i < chunks_.size(); ++i) {
        if (!chunks_[i].sampled && (eager || field_.may_have_surface(chunks_[i].samples.key))) {
            todo.push_back(i);
        }
    }
    jobs.parallel_for("sp12 load chunks", todo.size(), 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            produce(todo[i], true, mesher(jobs));
        }
    });
}

std::vector<ChunkKey> Terrain::apply(const BrushOp& op, JobSystem& jobs, bool reverse) {
    while (meshers_.size() < jobs.thread_count()) {
        meshers_.push_back(make_transvoxel_mesher({}));
    }
    const std::vector<ChunkKey> touched = touched_chunks(op);
    std::vector<std::size_t> todo;
    for (const ChunkKey& k : touched) {
        const auto it = index_.find(k);
        if (it == index_.end()) {
            ++outside_;  // the tests check it stays 0
            continue;
        }
        todo.push_back(it->second);
    }
    if (reverse) {
        std::reverse(todo.begin(), todo.end());
    }
    std::vector<std::uint8_t> changed(todo.size(), 0);
    std::vector<std::uint8_t> sampled(todo.size(), 0);
    jobs.parallel_for("sp12 apply brush", todo.size(), 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            Chunk& c = chunks_[todo[i]];
            const bool sample = !c.sampled;
            sampled[i] = sample ? 1 : 0;
            double t0 = 0.0;
            if (sample) {
                VEACH_ZONE("sp12 sample chunk");
                t0 = now_ms();
                sample_chunk(field_, c.samples.key, 0, c.samples);
                c.sampled = true;
                c.times.sample = now_ms() - t0;
            }
            {
                VEACH_ZONE("sp12 apply brush");
                t0 = now_ms();
                changed[i] = apply_brush(op, c.samples, c.mask) != 0 ? 1 : 0;
                c.times.edit = now_ms() - t0;
            }
            if (changed[i] != 0 || sample) {
                produce(todo[i], false, mesher(jobs));
            }
        }
    });
    std::vector<ChunkKey> out;
    for (std::size_t i = 0; i < todo.size(); ++i) {
        materialized_ += sampled[i];
        if (changed[i] != 0) {
            out.push_back(chunks_[todo[i]].samples.key);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

const Chunk* Terrain::find(const ChunkKey& key) const {
    const auto it = index_.find(key);
    return it == index_.end() ? nullptr : &chunks_[it->second];
}

std::uint64_t Terrain::samples_hash() const {
    std::vector<std::uint64_t> hashes;
    for (const Chunk& c : chunks_) {
        if (c.sampled) {
            hashes.push_back(voxel::samples_hash(c.samples));
        }
    }
    return combine_hashes(hashes);
}

std::uint64_t Terrain::mesh_hash() const {
    std::vector<std::uint64_t> hashes;
    for (const Chunk& c : chunks_) {
        if (c.sampled) {
            hashes.push_back(voxel::mesh_hash(c.mesh));
        }
    }
    return combine_hashes(hashes);
}

std::size_t Terrain::sampled_count() const {
    return static_cast<std::size_t>(
        std::count_if(chunks_.begin(), chunks_.end(), [](const Chunk& c) { return c.sampled; }));
}

std::size_t Terrain::override_samples() const {
    std::size_t n = 0;
    for (const Chunk& c : chunks_) {
        if (c.mask.empty()) {
            continue;
        }
        for (int z = 0; z < chunk_cells; ++z) {
            for (int y = 0; y < chunk_cells; ++y) {
                for (int x = 0; x < chunk_cells; ++x) {
                    n += c.mask[ChunkSamples::grid_index(x, y, z)];
                }
            }
        }
    }
    return n;
}

std::size_t Terrain::meshed_count() const {
    return static_cast<std::size_t>(
        std::count_if(chunks_.begin(), chunks_.end(), [](const Chunk& c) { return !c.mesh.indices.empty(); }));
}

std::size_t Terrain::shared_sample_mismatches() const {
    std::size_t mismatches = 0;
    for (const Chunk& c : chunks_) {
        if (!c.sampled) {
            continue;
        }
        const auto o = chunk_origin(c.samples.key);
        for (int axis = 0; axis < 3; ++axis) {
            ChunkKey nk = c.samples.key;
            (axis == 0 ? nk.x : axis == 1 ? nk.y : nk.z) += 1;
            const Chunk* n = find(nk);
            if (n == nullptr || !n->sampled) {
                continue;
            }
            const auto no = chunk_origin(nk);
            bool differ = false;
            // The overlap along `axis`: this chunk's grid 31..33 is the neighbor's -1..1.
            for (int z = -1; z <= chunk_cells + 1 && !differ; ++z) {
                for (int y = -1; y <= chunk_cells + 1 && !differ; ++y) {
                    for (int x = -1; x <= chunk_cells + 1 && !differ; ++x) {
                        const std::array<std::int64_t, 3> p{o[0] + x, o[1] + y, o[2] + z};
                        std::array<std::int64_t, 3> q{};
                        bool inside = true;
                        for (std::size_t a = 0; a < 3; ++a) {
                            q[a] = p[a] - no[a];
                            inside = inside && q[a] >= -1 && q[a] <= chunk_cells + 1;
                        }
                        if (!inside) {
                            continue;
                        }
                        const Sample& s0 = c.samples.at(x, y, z);
                        const Sample& s1 =
                            n->samples.at(static_cast<int>(q[0]), static_cast<int>(q[1]), static_cast<int>(q[2]));
                        differ = s0.distance != s1.distance || s0.material != s1.material;
                    }
                }
            }
            mismatches += differ ? 1 : 0;
        }
    }
    return mismatches;
}

}  // namespace veach::sp12
