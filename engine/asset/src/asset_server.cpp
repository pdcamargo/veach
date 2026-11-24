#include <veach/asset/asset_server.hpp>

#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>
#include <veach/core/suggest.hpp>
#include <veach/ecs/json.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <format>

namespace veach {

AssetModule::AssetModule(World& world) {
    world.module<AssetModule>("veach::asset");
    world.component<AssetMeta>()
        .member(&AssetMeta::version, "version")
        .member(&AssetMeta::id, "id")
        .member(&AssetMeta::loader, "loader");
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
    entries_.emplace(id, Entry{path, type, AssetState::failed, nullptr, std::move(error)});
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

    auto bytes = fs::read_file(absolute);
    if (!bytes) {
        return fail(path, type, bytes.error());
    }
    auto data = loader->load_erased(LoadContext{id, path, absolute, *bytes});

    Entry entry{path, type, AssetState::loaded, nullptr, std::nullopt};
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
    return id;
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

std::optional<fs::Path> AssetServer::path(AssetId id) const {
    const auto it = entries_.find(id);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second.path;
}

}  // namespace veach
