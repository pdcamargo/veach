// The terrain layer library (terrain_surface.hpp): scanned PBR sets cooked to two BC7 arrays, with a cache and a
// procedural fallback per layer.

#include <veach/runtime/terrain_surface.hpp>

#include <veach/core/hash.hpp>
#include <veach/core/jobs.hpp>
#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>
#include <veach/render/mesh_store.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>

namespace veach {

namespace {

namespace stdfs = std::filesystem;

/// Bump when the cooked bytes change for the same sources.
constexpr std::uint64_t cook_version = 3;

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::optional<std::string> read_file(const stdfs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream s;
    s << in.rdbuf();
    return std::move(s).str();
}

/// The maps of a scanned set, found by their names' endings.
struct ScanFiles {
    stdfs::path color, normal, roughness, gloss, ao, height, bump, json;
    [[nodiscard]] bool complete() const { return !color.empty() && !normal.empty(); }
};

ScanFiles find_scan(const stdfs::path& folder) {
    ScanFiles f;
    std::error_code ec;
    if (folder.empty() || !stdfs::is_directory(folder, ec)) {
        return f;
    }
    std::vector<stdfs::path> files;
    for (const auto& entry : stdfs::directory_iterator(folder, ec)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);  // a fixed order whatever the file system lists first
    for (const stdfs::path& p : files) {
        const std::string ext = lower(p.extension().string());
        const std::string stem = lower(p.stem().string());
        if (ext == ".json") {
            f.json = p;
            continue;
        }
        if (ext != ".jpg" && ext != ".jpeg" && ext != ".png") {
            continue;
        }
        const auto ends = [&](std::string_view suffix) {
            return stem.ends_with(suffix);
        };
        if (ends("_basecolor") || ends("_albedo")) {
            f.color = p;
        } else if (ends("_normal")) {
            f.normal = p;
        } else if (ends("_roughness")) {
            f.roughness = p;
        } else if (ends("_gloss")) {
            f.gloss = p;
        } else if (ends("_ao")) {
            f.ao = p;
        } else if (ends("_displacement")) {
            f.height = p;
        } else if (ends("_bump")) {
            f.bump = p;
        }
    }
    return f;
}

/// The scan's tile from a Megascans JSON ("scanArea": "2x2 m"), or 0.
float scan_area(const stdfs::path& json_path) {
    if (json_path.empty()) {
        return 0.0f;
    }
    const auto text = read_file(json_path);
    if (!text) {
        return 0.0f;
    }
    const nlohmann::json j = nlohmann::json::parse(*text, nullptr, false);
    if (j.is_discarded() || !j.contains("meta") || !j["meta"].is_array()) {
        return 0.0f;
    }
    for (const auto& m : j["meta"]) {
        if (m.value("key", "") == "scanArea" && m.contains("value") && m["value"].is_string()) {
            const std::string v = m["value"].get<std::string>();
            return static_cast<float>(std::strtod(v.c_str(), nullptr));
        }
    }
    return 0.0f;
}

std::optional<ImageRgba8> load_image(const stdfs::path& path, std::uint32_t size) {
    if (path.empty()) {
        return std::nullopt;
    }
    const auto bytes = read_file(path);
    if (!bytes) {
        return std::nullopt;
    }
    auto image = decode_image(std::as_bytes(std::span(bytes->data(), bytes->size())), path.generic_string());
    if (!image) {
        log::warn("terrain", "{}", image.error().message);
        return std::nullopt;
    }
    if (image->width != size || image->height != size) {
        *image = resample_image(*image, size, size, false);
    }
    return std::move(*image);
}

/// The cooked layer before encoding: albedo (sRGB) and height, normal xy (-dh/du, -dh/dv at 0.5 = flat), roughness, AO.
struct LayerImages {
    ImageRgba8 albedo_height;
    ImageRgba8 detail;
};

/// Halves the detail map: normals averaged and renormalized (in xy), roughness widened by how much the four normals
/// disagree (Toksvig), so distant bumps turn into roughness instead of sparkle.
ImageRgba8 halve_detail(const ImageRgba8& in) {
    ImageRgba8 out;
    out.width = std::max(1u, in.width / 2);
    out.height = std::max(1u, in.height / 2);
    out.pixels.resize(std::size_t{out.width} * out.height * 4);
    for (std::uint32_t y = 0; y < out.height; ++y) {
        for (std::uint32_t x = 0; x < out.width; ++x) {
            math::vec3 n_sum(0.0f);
            float r_sum = 0.0f;
            float ao_sum = 0.0f;
            for (std::uint32_t k = 0; k < 4; ++k) {
                const std::uint32_t sx = std::min(2 * x + (k & 1u), in.width - 1);
                const std::uint32_t sy = std::min(2 * y + (k >> 1), in.height - 1);
                const std::uint8_t* p = &in.pixels[(std::size_t{sy} * in.width + sx) * 4];
                const float nx = static_cast<float>(p[0]) / 127.5f - 1.0f;
                const float ny = static_cast<float>(p[1]) / 127.5f - 1.0f;
                const float nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
                n_sum += math::vec3(nx, ny, nz);
                r_sum += static_cast<float>(p[2]) / 255.0f;
                ao_sum += static_cast<float>(p[3]);
            }
            const float length = math::length(n_sum) / 4.0f;
            const math::vec3 n = length > 0.0f ? n_sum / (4.0f * length) : math::vec3(0.0f, 0.0f, 1.0f);
            const float r = r_sum / 4.0f;
            const float variance = std::clamp((1.0f - length) / std::max(length, 1e-3f), 0.0f, 1.0f);
            const float r_wide = std::min(1.0f, std::sqrt(r * r + variance));
            std::uint8_t* o = &out.pixels[(std::size_t{y} * out.width + x) * 4];
            o[0] = static_cast<std::uint8_t>(std::lround(std::clamp(n.x * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f));
            o[1] = static_cast<std::uint8_t>(std::lround(std::clamp(n.y * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f));
            o[2] = static_cast<std::uint8_t>(std::lround(r_wide * 255.0f));
            o[3] = static_cast<std::uint8_t>(std::lround(ao_sum / 4.0f));
        }
    }
    return out;
}

/// The full mip chains of a layer, encoded as BC7 (or kept RGBA8).
struct CookedLayer {
    TextureAsset albedo_height;
    TextureAsset detail;
    TerrainLayerInfo info;
};

CookedLayer encode_layer(const LayerImages& images, bool compress, JobSystem& jobs) {
    CookedLayer out;
    out.albedo_height.format = compress ? TextureFormat::bc7_srgb : TextureFormat::rgba8_srgb;
    out.detail.format = compress ? TextureFormat::bc7_unorm : TextureFormat::rgba8_unorm;
    out.albedo_height.width = out.detail.width = images.albedo_height.width;
    out.albedo_height.height = out.detail.height = images.albedo_height.height;
    ImageRgba8 a = images.albedo_height;
    ImageRgba8 d = images.detail;
    while (true) {
        if (compress) {
            out.albedo_height.mips.push_back(encode_bc7(a, true, &jobs));
            out.detail.mips.push_back(encode_bc7(d, false, &jobs));
        } else {
            const auto* ab = reinterpret_cast<const std::byte*>(a.pixels.data());
            const auto* db = reinterpret_cast<const std::byte*>(d.pixels.data());
            out.albedo_height.mips.emplace_back(ab, ab + a.pixels.size());
            out.detail.mips.emplace_back(db, db + d.pixels.size());
        }
        if (a.width == 1 && a.height == 1) {
            break;
        }
        a = halve_image(a, true);
        d = halve_detail(d);
    }
    return out;
}

/// Means of a layer, from its level-0 images (linear color).
void measure(const LayerImages& images, TerrainLayerInfo& info) {
    double r = 0.0, g = 0.0, b = 0.0, rough = 0.0, h = 0.0;
    const std::size_t count = images.albedo_height.pixels.size() / 4;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* p = &images.albedo_height.pixels[i * 4];
        r += srgb_to_linear(static_cast<float>(p[0]) / 255.0f);
        g += srgb_to_linear(static_cast<float>(p[1]) / 255.0f);
        b += srgb_to_linear(static_cast<float>(p[2]) / 255.0f);
        h += static_cast<float>(p[3]) / 255.0f;
        rough += static_cast<float>(images.detail.pixels[i * 4 + 2]) / 255.0f;
    }
    const double n = static_cast<double>(std::max<std::size_t>(count, 1));
    info.mean_color = math::vec3(static_cast<float>(r / n), static_cast<float>(g / n), static_cast<float>(b / n));
    info.mean_luminance = 0.2126f * info.mean_color.x + 0.7152f * info.mean_color.y + 0.0722f * info.mean_color.z;
    info.mean_roughness = static_cast<float>(rough / n);
    info.mean_height = static_cast<float>(h / n);
}

/// The scan's maps combined into the two images. Normals are brought to the shader's convention by checking them
/// against the height map's slopes (scans come in both the OpenGL and the DirectX green).
std::optional<LayerImages> combine_scan(const ScanFiles& files, std::uint32_t size, const std::string& name) {
    auto color = load_image(files.color, size);
    auto normal = load_image(files.normal, size);
    if (!color || !normal) {
        return std::nullopt;
    }
    auto roughness = load_image(files.roughness, size);
    const bool from_gloss = !roughness && !files.gloss.empty();
    if (from_gloss) {
        roughness = load_image(files.gloss, size);
    }
    auto ao = load_image(files.ao, size);
    auto height = load_image(!files.height.empty() ? files.height : files.bump, size);
    const std::size_t count = std::size_t{size} * size;

    // The height, stretched to 0..1 between its 1st and 99th percentiles.
    std::vector<float> h(count, 0.5f);
    if (height) {
        std::array<std::size_t, 256> histogram{};
        for (std::size_t i = 0; i < count; ++i) {
            ++histogram.at(height->pixels[i * 4]);
        }
        std::size_t acc = 0;
        int lo = 0, hi = 255;
        for (int v = 0; v < 256; ++v) {
            acc += histogram.at(static_cast<std::size_t>(v));
            if (acc <= count / 100) {
                lo = v;
            }
            if (acc < count - count / 100) {
                hi = v + 1;
            }
        }
        hi = std::max(hi, lo + 1);
        for (std::size_t i = 0; i < count; ++i) {
            h[i] = std::clamp(
                (static_cast<float>(height->pixels[i * 4]) - static_cast<float>(lo)) / static_cast<float>(hi - lo),
                0.0f, 1.0f);
        }
    }

    // Which way the normal map's red and green point: (R - 0.5) must fall where the height rises along the
    // columns, (G - 0.5) where it rises along the rows (the shader adds -dh/du and -dh/dv).
    bool flip_r = false, flip_g = false;
    if (height) {
        double cr = 0.0, cg = 0.0;
        for (std::uint32_t y = 1; y + 1 < size; y += 3) {
            for (std::uint32_t x = 1; x + 1 < size; x += 3) {
                const std::size_t i = std::size_t{y} * size + x;
                const double dhdx = h[i + 1] - h[i - 1];
                const double dhdy = h[i + size] - h[i - size];
                cr += (static_cast<double>(normal->pixels[i * 4]) - 127.5) * dhdx;
                cg += (static_cast<double>(normal->pixels[i * 4 + 1]) - 127.5) * dhdy;
            }
        }
        flip_r = cr > 0.0;
        flip_g = cg > 0.0;
        log::debug("terrain", "{}: normal map red {} green {} (against the height's slopes)", name,
                   flip_r ? "flipped" : "kept", flip_g ? "flipped" : "kept");
    }

    LayerImages out;
    out.albedo_height = {size, size, std::vector<std::uint8_t>(count * 4)};
    out.detail = {size, size, std::vector<std::uint8_t>(count * 4)};
    for (std::size_t i = 0; i < count; ++i) {
        std::uint8_t* a = &out.albedo_height.pixels[i * 4];
        std::uint8_t* d = &out.detail.pixels[i * 4];
        a[0] = color->pixels[i * 4];
        a[1] = color->pixels[i * 4 + 1];
        a[2] = color->pixels[i * 4 + 2];
        a[3] = static_cast<std::uint8_t>(std::lround(h[i] * 255.0f));
        const std::uint8_t nr = normal->pixels[i * 4];
        const std::uint8_t ng = normal->pixels[i * 4 + 1];
        d[0] = flip_r ? static_cast<std::uint8_t>(255 - nr) : nr;
        d[1] = flip_g ? static_cast<std::uint8_t>(255 - ng) : ng;
        const std::uint8_t r = roughness ? roughness->pixels[i * 4] : std::uint8_t{204};
        d[2] = from_gloss ? static_cast<std::uint8_t>(255 - r) : r;
        d[3] = ao ? ao->pixels[i * 4] : std::uint8_t{255};
    }
    return out;
}

// ---- the procedural fallback: tileable value noise --------------------------------------------------------------

float lattice(std::uint64_t seed, std::uint32_t x, std::uint32_t y) {
    return unit_float(hash(seed, x, y));
}

/// Value noise of `cells` cells across the tile, wrapping at its edges.
float tile_noise(std::uint64_t seed, float u, float v, std::uint32_t cells) {
    const float x = u * static_cast<float>(cells);
    const float y = v * static_cast<float>(cells);
    const auto x0 = static_cast<std::uint32_t>(std::floor(x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(y));
    const float fx = x - std::floor(x);
    const float fy = y - std::floor(y);
    const float sx = fx * fx * (3.0f - 2.0f * fx);
    const float sy = fy * fy * (3.0f - 2.0f * fy);
    const std::uint32_t x1 = (x0 + 1) % cells;
    const std::uint32_t y1 = (y0 + 1) % cells;
    const float a = lattice(seed, x0 % cells, y0 % cells);
    const float b = lattice(seed, x1, y0 % cells);
    const float c = lattice(seed, x0 % cells, y1);
    const float d = lattice(seed, x1, y1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

LayerImages procedural_layer(const TerrainLayerSource& source, float tile_m, std::uint32_t size) {
    const TerrainFallback& f = source.fallback;
    const std::uint64_t seed = hash_id(source.name);
    const auto base_cells = static_cast<std::uint32_t>(std::clamp(std::lround(tile_m / f.grain_m), 2L, 64L));
    const std::size_t count = std::size_t{size} * size;
    std::vector<float> h(count);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            float value = 0.0f, amplitude = 0.5f, total = 0.0f;
            std::uint32_t cells = base_cells;
            for (std::uint64_t octave = 0; octave < 4 && cells <= size / 2; ++octave) {
                value += tile_noise(seed + octave, u, v, cells) * amplitude;
                total += amplitude;
                amplitude *= 0.5f;
                cells *= 2;
            }
            h[std::size_t{y} * size + x] = value / std::max(total, 1e-6f);
        }
    }
    LayerImages out;
    out.albedo_height = {size, size, std::vector<std::uint8_t>(count * 4)};
    out.detail = {size, size, std::vector<std::uint8_t>(count * 4)};
    const float slope_scale = f.relief * 6.0f * static_cast<float>(base_cells) / static_cast<float>(size) * 16.0f;
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const std::size_t i = std::size_t{y} * size + x;
            const float hx =
                h[std::size_t{y} * size + (x + 1) % size] - h[std::size_t{y} * size + (x + size - 1) % size];
            const float hy =
                h[std::size_t{(y + 1) % size} * size + x] - h[std::size_t{(y + size - 1) % size} * size + x];
            math::vec3 n(-hx * slope_scale, -hy * slope_scale, 1.0f);
            n = math::normalize(n);
            const float shade = 0.72f + 0.56f * h[i];
            std::uint8_t* a = &out.albedo_height.pixels[i * 4];
            for (int c = 0; c < 3; ++c) {
                a[c] = static_cast<std::uint8_t>(std::lround(std::clamp(f.color[c] * shade, 0.0f, 1.0f) * 255.0f));
            }
            a[3] = static_cast<std::uint8_t>(std::lround(h[i] * 255.0f));
            std::uint8_t* d = &out.detail.pixels[i * 4];
            d[0] = static_cast<std::uint8_t>(std::lround((n.x * 0.5f + 0.5f) * 255.0f));
            d[1] = static_cast<std::uint8_t>(std::lround((n.y * 0.5f + 0.5f) * 255.0f));
            d[2] = static_cast<std::uint8_t>(
                std::lround(std::clamp(f.roughness + (0.5f - h[i]) * 0.12f, 0.0f, 1.0f) * 255.0f));
            d[3] = static_cast<std::uint8_t>(std::lround(std::clamp(0.55f + 0.6f * h[i], 0.0f, 1.0f) * 255.0f));
        }
    }
    return out;
}

/// The cache key of a layer: its sources' names, sizes and times, the settings and the cook version.
std::uint64_t cache_key(const ScanFiles& files, const TerrainLayerSource& source, std::uint32_t size, bool compress,
                        bool scanned) {
    std::uint64_t h = hash(cook_version, hash_id(source.name), size, compress ? 1u : 0u, scanned ? 1u : 0u);
    for (const stdfs::path* p :
         {&files.color, &files.normal, &files.roughness, &files.gloss, &files.ao, &files.height, &files.bump}) {
        if (p->empty() || !scanned) {
            continue;
        }
        std::error_code ec;
        const auto bytes = stdfs::file_size(*p, ec);
        const auto time = stdfs::last_write_time(*p, ec).time_since_epoch().count();
        h = hash(h, hash_id(p->filename().generic_string()), static_cast<std::uint64_t>(bytes),
                 static_cast<std::uint64_t>(time));
    }
    if (!scanned) {
        const TerrainFallback& f = source.fallback;
        for (const float v : {f.color.x, f.color.y, f.color.z, f.roughness, f.relief, f.grain_m, source.tile_m}) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v, 4);
            h = hash(h, bits);
        }
    }
    return h;
}

std::optional<CookedLayer> read_cache(const stdfs::path& base) {
    const auto a = read_file(base.string() + "-albedo.ktx2");
    const auto d = read_file(base.string() + "-detail.ktx2");
    const auto info = read_file(base.string() + ".json");
    if (!a || !d || !info) {
        return std::nullopt;
    }
    auto ta = read_ktx2(*a, base.string());
    auto td = read_ktx2(*d, base.string());
    const nlohmann::json j = nlohmann::json::parse(*info, nullptr, false);
    if (!ta || !td || j.is_discarded()) {
        return std::nullopt;
    }
    CookedLayer out{std::move(*ta), std::move(*td), {}};
    const auto& c = j.value("mean_color", std::vector<float>{0.5f, 0.5f, 0.5f});
    out.info.mean_color = math::vec3(c.at(0), c.at(1), c.at(2));
    out.info.mean_luminance = j.value("mean_luminance", 0.5f);
    out.info.mean_roughness = j.value("mean_roughness", 0.8f);
    out.info.mean_height = j.value("mean_height", 0.5f);
    out.info.tile_m = j.value("tile_m", 2.0f);
    return out;
}

void write_cache(const stdfs::path& base, const CookedLayer& layer) {
    std::error_code ec;
    stdfs::create_directories(base.parent_path(), ec);
    const auto write = [](const stdfs::path& path, const std::string& bytes) {
        std::ofstream out(path, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(out);
    };
    nlohmann::json j;
    j["mean_color"] = {layer.info.mean_color.x, layer.info.mean_color.y, layer.info.mean_color.z};
    j["mean_luminance"] = layer.info.mean_luminance;
    j["mean_roughness"] = layer.info.mean_roughness;
    j["mean_height"] = layer.info.mean_height;
    j["tile_m"] = layer.info.tile_m;
    if (!write(base.string() + "-albedo.ktx2", write_ktx2(layer.albedo_height)) ||
        !write(base.string() + "-detail.ktx2", write_ktx2(layer.detail)) || !write(base.string() + ".json", j.dump())) {
        log::warn("terrain", "cannot write the terrain layer cache at {}", base.generic_string());
    }
}

}  // namespace

float terrain_tile(float tile_m) {
    if (!(tile_m > 0.0f)) {
        return 2.0f;
    }
    const long n = std::clamp(std::lround(16.0f / tile_m), 1L, 64L);
    return 16.0f / static_cast<float>(n);
}

TerrainLayerLibrary::TerrainLayerLibrary(std::vector<TerrainLayerInfo> layers, TextureAsset albedo_height,
                                         TextureAsset detail)
    : layers_(std::move(layers)), albedo_height_(std::move(albedo_height)), detail_(std::move(detail)) {}

std::optional<std::uint32_t> TerrainLayerLibrary::find(std::string_view name) const {
    for (std::size_t i = 0; i < layers_.size(); ++i) {
        if (layers_[i].name == name) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return std::nullopt;
}

bool TerrainLayerLibrary::scanned() const {
    return std::ranges::any_of(layers_, [](const TerrainLayerInfo& l) { return l.scanned; });
}

Result<std::array<AssetId, 2>> TerrainLayerLibrary::textures(render::MeshStore& store) const {
    const std::scoped_lock lock(mutex_);
    if (store_ == &store) {
        return ids_;
    }
    auto a = store.add_texture(albedo_height_);
    if (!a) {
        return std::unexpected(a.error());
    }
    auto d = store.add_texture(detail_);
    if (!d) {
        return std::unexpected(d.error());
    }
    ids_ = {*a, *d};
    store_ = &store;
    return ids_;
}

Result<std::shared_ptr<const TerrainLayerLibrary>> load_terrain_library(std::span<const TerrainLayerSource> layers,
                                                                        const TerrainLibrarySettings& settings,
                                                                        JobSystem& jobs) {
    VEACH_ZONE("terrain: load layer library");
    if (layers.empty()) {
        return make_error(ErrorCode::render_mesh_invalid, "a terrain layer library with no layers",
                          "List at least one layer (a scan folder or a procedural fallback).");
    }
    std::vector<ScanFiles> files;
    bool any_scan = false;
    for (const TerrainLayerSource& l : layers) {
        files.push_back(find_scan(l.folder));
        any_scan = any_scan || files.back().complete();
    }
    // One format and size for the whole array: scans at the library's size in BC7; without any scan, small RGBA8
    // procedural layers (fast to make in CI and Debug builds).
    std::uint32_t size = 256;
    if (any_scan) {
        size = 4;
        while (size < settings.size && size < 8192) {
            size *= 2;
        }
    }
    const bool compress = any_scan;
    std::vector<CookedLayer> cooked(layers.size());
    std::vector<std::string> notes(layers.size());
    jobs.parallel_for("terrain layers", layers.size(), 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const TerrainLayerSource& source = layers[i];
            const ScanFiles& f = files[i];
            const bool scanned = f.complete();
            float tile = source.tile_m > 0.0f ? source.tile_m : scanned ? scan_area(f.json) : 0.0f;
            tile = terrain_tile(tile > 0.0f ? tile : 2.0f);
            const stdfs::path base = settings.cache.empty()
                                         ? stdfs::path{}
                                         : settings.cache / std::format("{}-{:016x}", source.name,
                                                                        cache_key(f, source, size, compress, scanned));
            std::optional<CookedLayer> layer = base.empty() || !compress ? std::nullopt : read_cache(base);
            if (!layer) {
                std::optional<LayerImages> images = scanned ? combine_scan(f, size, source.name) : std::nullopt;
                const bool from_scan = images.has_value();
                if (!images) {
                    images = procedural_layer(source, tile, size);
                }
                layer = encode_layer(*images, compress, jobs);
                measure(*images, layer->info);
                layer->info.tile_m = tile;
                layer->info.scanned = from_scan;
                if (!base.empty() && compress) {
                    write_cache(base, *layer);
                }
                notes[i] = from_scan ? "cooked" : "procedural";
            } else {
                layer->info.scanned = scanned;
                layer->info.tile_m = tile;  // the config may change it without recooking
                notes[i] = "cached";
            }
            layer->info.name = source.name;
            cooked[i] = std::move(*layer);
        }
    });
    std::vector<TerrainLayerInfo> infos;
    std::vector<TextureAsset> albedo, detail;
    std::string summary;
    for (std::size_t i = 0; i < cooked.size(); ++i) {
        infos.push_back(cooked[i].info);
        albedo.push_back(std::move(cooked[i].albedo_height));
        detail.push_back(std::move(cooked[i].detail));
        summary += std::format("{}{} ({}, {:.2f} m)", i == 0 ? "" : ", ", layers[i].name, notes[i], infos[i].tile_m);
    }
    auto a = stack_layers(albedo, "terrain albedo layers");
    if (!a) {
        return std::unexpected(a.error());
    }
    auto d = stack_layers(detail, "terrain detail layers");
    if (!d) {
        return std::unexpected(d.error());
    }
    log::info("terrain", "terrain layers at {}x{}{}: {}", size, size, compress ? " BC7" : " procedural", summary);
    return std::make_shared<const TerrainLayerLibrary>(std::move(infos), std::move(*a), std::move(*d));
}

Result<TerrainLibraryConfig> read_terrain_library_config(std::string_view json, const stdfs::path& base,
                                                         std::span<const TerrainLayerSource> defaults) {
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("layers") || !j["layers"].is_array()) {
        return make_error(ErrorCode::data_parse_failed, "a terrain library config is not JSON with a \"layers\" array",
                          "See docs/guides/terrain-materials.md for the format.");
    }
    TerrainLibraryConfig out;
    const auto path_of = [&](const std::string& p) {
        return p.empty() ? stdfs::path{} : (base / p).lexically_normal();
    };
    const stdfs::path scans = path_of(j.value("scans", ""));
    out.settings.cache = path_of(j.value("cache", ""));
    out.settings.size = j.value("size", 1024u);
    for (const auto& l : j["layers"]) {
        TerrainLayerSource s;
        s.name = l.value("name", "");
        if (s.name.empty()) {
            return make_error(ErrorCode::data_parse_failed, "a terrain library layer has no name",
                              "Give every layer a \"name\": surfaces find layers by it.");
        }
        const std::string scan = l.value("scan", "");
        s.folder = scan.empty() || scans.empty() ? stdfs::path{} : scans / scan;
        s.tile_m = l.value("tile_m", 0.0f);
        for (const TerrainLayerSource& d : defaults) {
            if (d.name == s.name) {
                s.fallback = d.fallback;
            }
        }
        if (l.contains("fallback")) {
            const auto& f = l["fallback"];
            if (f.contains("color") && f["color"].is_array() && f["color"].size() == 3) {
                s.fallback.color =
                    math::vec3(f["color"][0].get<float>(), f["color"][1].get<float>(), f["color"][2].get<float>());
            }
            s.fallback.roughness = f.value("roughness", s.fallback.roughness);
            s.fallback.relief = f.value("relief", s.fallback.relief);
            s.fallback.grain_m = f.value("grain_m", s.fallback.grain_m);
        }
        out.layers.push_back(std::move(s));
    }
    return out;
}

}  // namespace veach
