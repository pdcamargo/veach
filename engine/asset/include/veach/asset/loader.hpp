#pragma once

// Asset loaders turn bytes into typed asset data. One loader per asset type;
// it claims file extensions. Loaders must not touch the ECS world: they are
// written so they can move to worker threads when async loading lands (VCH-031).

#include <veach/core/error.hpp>
#include <veach/core/ids.hpp>
#include <veach/platform/fs.hpp>

#include <memory>
#include <span>
#include <string_view>

namespace veach {

struct LoadContext {
    AssetId id;
    fs::Path path;  ///< Project-relative, '/'-separated.
    fs::Path absolute_path;
    std::string_view bytes;  ///< Whole file contents.
};

/// Identifies an asset type without RTTI.
using AssetTypeKey = const void*;

template <class T>
AssetTypeKey asset_type_key() {
    static const char tag = 0;
    return &tag;
}

class AssetLoader {
public:
    virtual ~AssetLoader() = default;

    /// Stable name, written to .meta files ("luau-script").
    [[nodiscard]] virtual std::string_view name() const = 0;
    /// Claimed extensions including the dot (".luau").
    [[nodiscard]] virtual std::span<const std::string_view> extensions() const = 0;
    [[nodiscard]] virtual AssetTypeKey asset_type() const = 0;
    [[nodiscard]] virtual Result<std::shared_ptr<const void>> load_erased(const LoadContext& ctx) const = 0;
};

/// Implement load() for one asset type T.
template <class T>
class TypedAssetLoader : public AssetLoader {
public:
    [[nodiscard]] virtual Result<T> load(const LoadContext& ctx) const = 0;

    [[nodiscard]] AssetTypeKey asset_type() const final { return asset_type_key<T>(); }
    [[nodiscard]] Result<std::shared_ptr<const void>> load_erased(const LoadContext& ctx) const final {
        auto result = load(ctx);
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }
        return std::shared_ptr<const void>(std::make_shared<const T>(std::move(*result)));
    }
};

}  // namespace veach
