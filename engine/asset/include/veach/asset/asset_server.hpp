#pragma once

// The asset registry (ADR 0007, docs/specs/asset-system-core/spec.md).
//
//   auto& assets = veach::services(world).get<veach::AssetServer>();
//   veach::Handle<veach::ScriptAsset> h = assets.load<veach::ScriptAsset>("scripts/hello.luau");
//   if (const auto* script = assets.get(h)) { ... }
//
// load() is the async-ready shape: it returns a handle immediately and the
// state/result are queried separately. v0 loads synchronously inside load();
// callers that need the data now use wait(), which will block once loading is
// asynchronous. Every source asset gets a .meta sidecar holding its stable id.

#include <veach/asset/handle.hpp>
#include <veach/asset/loader.hpp>
#include <veach/core/error.hpp>
#include <veach/ecs/flecs.hpp>
#include <veach/platform/fs.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace veach {

enum class AssetState : std::uint8_t { loading, loaded, failed };

/// Contents of a `<asset>.meta` sidecar. Reflected; schema in schemas/asset.meta.schema.json.
struct AssetMeta {
    std::uint32_t version = 1;
    std::string id;      ///< AssetId as a UUID string. Never edit by hand.
    std::string loader;  ///< Loader that imports the asset, e.g. "luau-script".
};

/// Registers AssetMeta reflection. Imported by the engine.
struct AssetModule {
    explicit AssetModule(World& world);
};

class AssetServer {
public:
    /// `root` is the project directory; asset paths are relative to it.
    AssetServer(World& world, fs::Path root);

    void add_loader(std::unique_ptr<AssetLoader> loader);

    template <class T>
    [[nodiscard]] Handle<T> load(std::string_view path) {
        return Handle<T>{load_erased(path, asset_type_key<T>())};
    }

    /// The asset data, or nullptr while loading or after a failure.
    template <class T>
    [[nodiscard]] const T* get(Handle<T> handle) const {
        return static_cast<const T*>(get_erased(handle.id, asset_type_key<T>()));
    }

    /// Waits for the load to finish and returns the data or the load error.
    template <class T>
    [[nodiscard]] Result<const T*> wait(Handle<T> handle) const {
        if (const Error* err = error(handle.id)) {
            return std::unexpected(*err);
        }
        if (const T* data = get(handle)) {
            return data;
        }
        return make_error(ErrorCode::internal, "asset is neither loaded nor failed");
    }

    [[nodiscard]] AssetState state(AssetId id) const;
    [[nodiscard]] const Error* error(AssetId id) const;
    [[nodiscard]] std::optional<fs::Path> path(AssetId id) const;
    [[nodiscard]] const fs::Path& root() const { return root_; }
    [[nodiscard]] std::size_t count() const { return entries_.size(); }

    /// Reads `<path>.meta`, creating it with a fresh id if it does not exist.
    [[nodiscard]] Result<AssetMeta> read_or_create_meta(const fs::Path& relative_path, std::string_view loader_name);

private:
    struct Entry {
        fs::Path path;
        AssetTypeKey type = nullptr;
        AssetState state = AssetState::loading;
        std::shared_ptr<const void> data;
        std::optional<Error> error;
    };

    AssetId load_erased(std::string_view path, AssetTypeKey type);
    [[nodiscard]] const void* get_erased(AssetId id, AssetTypeKey type) const;
    [[nodiscard]] const AssetLoader* find_loader(const fs::Path& path) const;
    AssetId fail(const fs::Path& path, AssetTypeKey type, Error error);

    World world_;
    fs::Path root_;
    std::vector<std::unique_ptr<AssetLoader>> loaders_;
    std::unordered_map<AssetId, Entry> entries_;
    std::unordered_map<std::string, AssetId> by_path_;
};

}  // namespace veach
