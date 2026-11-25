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
#include <functional>
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
    std::string id;                      ///< AssetId as a UUID string. Never edit by hand.
    std::string loader;                  ///< Loader that imports the asset, e.g. "luau-script".
    std::uint32_t max_texture_size = 0;  ///< Importers that cook textures halve them until they fit; 0 = no limit.
};

/// One asset, as list() reports it.
struct AssetInfo {
    AssetId id;
    fs::Path path;
    std::string loader;
    AssetState state = AssetState::loading;
    std::uint32_t reloads = 0;          ///< successful reloads so far
    std::optional<Error> error;         ///< the initial load's error
    std::optional<Error> reload_error;  ///< the last reload's error, cleared by the next successful one
};

/// What one reload did.
struct ReloadResult {
    AssetId id;
    fs::Path path;
    bool reloaded = false;  ///< the new data is in place (listeners may still have failed: see error)
    std::optional<Error> error;
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

    // ---- hot reload (docs/specs/hot-reload/spec.md)

    /// Reloads the asset at `path` now, changed or not. The id and every handle stay the same; pointers from
    /// get() are invalid afterwards, so keep handles, not pointers. If loading fails, the old data stays.
    ReloadResult reload(std::string_view path);
    /// Reloads every loaded asset whose file changed (write time or size) since it was last read.
    std::vector<ReloadResult> reload_changed();
    /// Called after an asset's new data is in place, e.g. to re-run a script. A failure is reported in the
    /// ReloadResult and logged; the new data stays.
    using ReloadListener = std::function<Result<>(AssetId id, AssetTypeKey type, const void* data)>;
    void on_reloaded(ReloadListener listener) { listeners_.push_back(std::move(listener)); }
    [[nodiscard]] std::vector<AssetInfo> list() const;

    /// Reads `<path>.meta`, creating it with a fresh id if it does not exist.
    [[nodiscard]] Result<AssetMeta> read_or_create_meta(const fs::Path& relative_path, std::string_view loader_name);

private:
    struct Entry {
        fs::Path path;
        AssetTypeKey type = nullptr;
        AssetState state = AssetState::loading;
        std::shared_ptr<const void> data;
        std::optional<Error> error;
        const AssetLoader* loader = nullptr;
        std::optional<fs::FileStamp> stamp;  ///< the file as last read
        std::uint32_t reloads = 0;
        std::optional<Error> reload_error;
    };

    ReloadResult reload_entry(AssetId id, Entry& entry);
    /// Registers (or, on reload, swaps) the sub-assets of a freshly loaded asset.
    void register_sub_assets(AssetId parent, const Entry& entry);

    AssetId load_erased(std::string_view path, AssetTypeKey type);
    [[nodiscard]] const void* get_erased(AssetId id, AssetTypeKey type) const;
    [[nodiscard]] const AssetLoader* find_loader(const fs::Path& path) const;
    AssetId fail(const fs::Path& path, AssetTypeKey type, Error error);

    World world_;
    fs::Path root_;
    std::vector<std::unique_ptr<AssetLoader>> loaders_;
    std::unordered_map<AssetId, Entry> entries_;
    std::unordered_map<std::string, AssetId> by_path_;
    std::vector<ReloadListener> listeners_;
};

}  // namespace veach
