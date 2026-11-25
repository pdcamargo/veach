#include <veach/asset/asset_server.hpp>

#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>
#include <veach/core/suggest.hpp>
#include <veach/ecs/json.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>

namespace veach {

AssetModule::AssetModule(World& world) {
    world.module<AssetModule>("veach::asset");
    world.component<AssetMeta>()
        .member(&AssetMeta::version, "version")
        .member(&AssetMeta::id, "id")
        .member(&AssetMeta::loader, "loader")
        .member(&AssetMeta::max_texture_size, "max_texture_size");
}

AssetServer::AssetServer(World& world, fs::Path root) : world_(world.c_ptr()), root_(std::move(root)) {}

void AssetServer::add_loader(std::unique_ptr<AssetLoader> loader) {
    log::debug("asset", "loader '{}' registered", loader->name());
    loaders_.push_back(std::move(loader));
}

const AssetLoader* AssetServer::find_loader(const fs::Path& path) const {
    const std::string ext = path.extension().string();
    for (const auto& loader : loaders_) {
        for (std::string_view claimed : loader->extensions()) {
            if (claimed == ext) {
                return loader.get();
            }
        }
    }
    return nullptr;
}

AssetId AssetServer::fail(const fs::Path& path, AssetTypeKey type, Error error) {
    log::error("asset", error);
    const AssetId id = AssetId::generate();  // transient: failed assets have no stable identity
    entries_.emplace(id, Entry{.path = path, .type = type, .state = AssetState::failed, .error = std::move(error)});
    return id;
}

Result<AssetMeta> AssetServer::read_or_create_meta(const fs::Path& relative_path, std::string_view loader_name) {
    const fs::Path meta_path = (root_ / relative_path).concat(".meta");
    const std::string display = relative_path.generic_string() + ".meta";
    AssetMeta meta;
    if (!fs::exists(meta_path)) {
        meta.id = AssetId::generate().to_string();
        meta.loader = std::string(loader_name);
        if (auto written = fs::write_file(meta_path, to_json(world_, world_.id<AssetMeta>(), &meta).dump(2) + "\n");
            !written) {
            return std::unexpected(written.error());
        }
        log::info("asset", "created {} (id {})", display, meta.id);
        return meta;
    }
    auto text = fs::read_file(meta_path);
    if (!text) {
        return std::unexpected(text.error());
    }
    const nlohmann::json j = nlohmann::json::parse(*text, nullptr, false);
    if (j.is_discarded()) {
        return make_error(ErrorCode::asset_meta_invalid, std::format("{} is not valid JSON", display),
                          "Fix the file, or delete it to mint a new id (this breaks references to the asset).");
    }
    if (auto r = from_json(world_, meta, j, display); !r) {
        return std::unexpected(r.error());
    }
    if (!AssetId::parse(meta.id)) {
        return make_error(ErrorCode::asset_meta_invalid, std::format("{} has an invalid id '{}'", display, meta.id),
                          "Ids are UUIDs like 3f2b1c4e-8d6a-4b7f-9e01-2a3b4c5d6e7f; restore it from version control.");
    }
    return meta;
}

AssetId AssetServer::load_erased(std::string_view path_text, AssetTypeKey type) {
    VEACH_ZONE("AssetServer::load");
    const fs::Path path = fs::Path(path_text).lexically_normal();
    const std::string key = path.generic_string();
    if (const auto it = by_path_.find(key); it != by_path_.end()) {
        return it->second;
    }

    const fs::Path absolute = root_ / path;
    if (!fs::exists(absolute)) {
        std::vector<std::string> siblings;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(absolute.parent_path(), ec)) {
            siblings.push_back((path.parent_path() / entry.path().filename()).generic_string());
        }
        std::string hint = did_you_mean(key, siblings);
        hint += std::format("{}Asset paths are relative to the project directory '{}'.", hint.empty() ? "" : " ",
                            root_.generic_string());
        return fail(path, type, Error{ErrorCode::asset_not_found, std::format("asset '{}' not found", key), hint});
    }

    const AssetLoader* loader = find_loader(path);
    if (!loader || loader->asset_type() != type) {
        return fail(
            path, type,
            Error{ErrorCode::asset_no_loader, std::format("no loader for '{}' produces the requested asset type", key),
                  "Register a loader for this extension with AssetServer::add_loader()."});
    }

    auto meta = read_or_create_meta(path, loader->name());
    if (!meta) {
        return fail(path, type, meta.error());
    }
    const AssetId id = *AssetId::parse(meta->id);

    const auto stamp = fs::stamp(absolute);  // before reading: a write during the read shows up next poll
    auto bytes = fs::read_file(absolute);
    if (!bytes) {
        return fail(path, type, bytes.error());
    }
    auto data = loader->load_erased(LoadContext{.id = id,
                                                .path = path,
                                                .absolute_path = absolute,
                                                .bytes = *bytes,
                                                .cache_dir = root_ / ".veach" / "cache" / id.to_string(),
                                                .max_texture_size = meta->max_texture_size});

    Entry entry{.path = path, .type = type, .state = AssetState::loaded, .loader = loader, .stamp = stamp};
    if (data) {
        entry.data = std::move(*data);
        log::debug("asset", "loaded '{}' ({}, id {})", key, loader->name(), meta->id);
    } else {
        log::error("asset", data.error());
        entry.state = AssetState::failed;
        entry.error = std::move(data.error());
    }
    entries_.insert_or_assign(id, std::move(entry));
    by_path_.emplace(key, id);
    register_sub_assets(id, entries_.at(id));
    return id;
}

void AssetServer::register_sub_assets(AssetId parent, const Entry& entry) {
    if (!entry.data || entry.loader == nullptr) {
        return;
    }
    for (const SubAsset& sub : entry.loader->sub_assets(entry.data.get())) {
        const AssetId id = AssetId::derive(parent, sub.key);
        const fs::Path path = fs::Path(entry.path.generic_string() + "#" + sub.key);
        entries_.insert_or_assign(id,
                                  Entry{.path = path, .type = sub.type, .state = AssetState::loaded, .data = sub.data});
        by_path_.insert_or_assign(path.generic_string(), id);
    }
}

const void* AssetServer::get_erased(AssetId id, AssetTypeKey type) const {
    const auto it = entries_.find(id);
    if (it == entries_.end() || it->second.type != type) {
        return nullptr;
    }
    return it->second.data.get();
}

AssetState AssetServer::state(AssetId id) const {
    const auto it = entries_.find(id);
    return it == entries_.end() ? AssetState::failed : it->second.state;
}

const Error* AssetServer::error(AssetId id) const {
    const auto it = entries_.find(id);
    if (it == entries_.end()) {
        static const Error unknown{ErrorCode::asset_not_found, "unknown asset id", "Use a handle returned by load()."};
        return &unknown;
    }
    return it->second.error ? &*it->second.error : nullptr;
}

ReloadResult AssetServer::reload_entry(AssetId id, Entry& entry) {
    VEACH_ZONE("AssetServer::reload");
    ReloadResult result{.id = id, .path = entry.path};
    const std::string key = entry.path.generic_string();
    const fs::Path absolute = root_ / entry.path;
    entry.stamp = fs::stamp(absolute);
    auto bytes = fs::read_file(absolute);
    auto meta = read_or_create_meta(entry.path, entry.loader->name());
    auto data = !bytes ? Result<std::shared_ptr<const void>>(std::unexpected(bytes.error()))
                : !meta
                    ? Result<std::shared_ptr<const void>>(std::unexpected(meta.error()))
                    : entry.loader->load_erased(LoadContext{.id = id,
                                                            .path = entry.path,
                                                            .absolute_path = absolute,
                                                            .bytes = *bytes,
                                                            .cache_dir = root_ / ".veach" / "cache" / id.to_string(),
                                                            .max_texture_size = meta->max_texture_size});
    if (!data) {
        log::error("asset", Error{data.error().code, std::format("reloading '{}': {}", key, data.error().message),
                                  "The previous version stays in use until the file loads again."});
        entry.reload_error = data.error();
        result.error = std::move(data.error());
        return result;
    }
    entry.data = std::move(*data);
    entry.state = AssetState::loaded;
    entry.error.reset();
    entry.reload_error.reset();
    ++entry.reloads;
    register_sub_assets(id, entry);
    result.reloaded = true;
    log::info("asset", "reloaded '{}' (reload {})", key, entry.reloads);
    for (const ReloadListener& listener : listeners_) {
        if (auto applied = listener(id, entry.type, entry.data.get()); !applied) {
            entry.reload_error = applied.error();
            result.error = std::move(applied.error());
            break;
        }
    }
    return result;
}

ReloadResult AssetServer::reload(std::string_view path_text) {
    const std::string key = fs::Path(path_text).lexically_normal().generic_string();
    const auto it = by_path_.find(key);
    if (it == by_path_.end()) {
        std::vector<std::string> loaded;
        for (const auto& [p, id] : by_path_) {
            loaded.push_back(p);
        }
        return {.path = key,
                .error = Error{ErrorCode::asset_not_found, std::format("'{}' is not a loaded asset", key),
                               did_you_mean(key, loaded)}};
    }
    return reload_entry(it->second, entries_.at(it->second));
}

std::vector<ReloadResult> AssetServer::reload_changed() {
    // Collect first: a reload registers sub-assets, which may add paths (and rehash by_path_).
    std::vector<AssetId> changed;
    for (const auto& [key, id] : by_path_) {
        const Entry& entry = entries_.at(id);
        if (entry.loader != nullptr && fs::stamp(root_ / entry.path) != entry.stamp) {
            changed.push_back(id);
        }
    }
    std::vector<ReloadResult> results;
    for (const AssetId id : changed) {
        results.push_back(reload_entry(id, entries_.at(id)));
    }
    return results;
}

std::vector<AssetInfo> AssetServer::list() const {
    std::vector<AssetInfo> out;
    for (const auto& [key, id] : by_path_) {
        const Entry& e = entries_.at(id);
        out.push_back({.id = id,
                       .path = e.path,
                       .loader = e.loader ? std::string(e.loader->name()) : std::string(),
                       .state = e.state,
                       .reloads = e.reloads,
                       .error = e.error,
                       .reload_error = e.reload_error});
    }
    std::ranges::sort(out, {}, [](const AssetInfo& a) { return a.path.generic_string(); });
    return out;
}

std::optional<fs::Path> AssetServer::path(AssetId id) const {
    const auto it = entries_.find(id);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second.path;
}

}  // namespace veach
