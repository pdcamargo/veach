#include "support/test_support.hpp"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

using namespace veach;

namespace {

struct TextAsset {
    std::string text;
};

class TextLoader final : public TypedAssetLoader<TextAsset> {
public:
    [[nodiscard]] std::string_view name() const override { return "text"; }
    [[nodiscard]] std::span<const std::string_view> extensions() const override { return exts_; }
    [[nodiscard]] Result<TextAsset> load(const LoadContext& ctx) const override {
        return TextAsset{std::string(ctx.bytes)};
    }

private:
    static constexpr std::array<std::string_view, 1> exts_{".txt"};
};

struct AssetFixture {
    AssetFixture() : project("assets") {
        world.import<CoreModule>();
        world.import<AssetModule>();
    }
    test::TempProject project;
    World world;
};

}  // namespace

TEST_CASE("[asset.AC1] loading creates a .meta sidecar whose id is stable") {
    AssetFixture f;
    f.project.write("data/hello.txt", "hi");

    AssetId first_id;
    {
        AssetServer assets(f.world, f.project.path());
        assets.add_loader(std::make_unique<TextLoader>());
        const Handle<TextAsset> h = assets.load<TextAsset>("data/hello.txt");
        REQUIRE(assets.state(h.id) == AssetState::loaded);
        REQUIRE(assets.get(h) != nullptr);
        CHECK(assets.get(h)->text == "hi");
        CHECK(assets.load<TextAsset>("data/../data/hello.txt") == h);  // same asset, same handle
        first_id = h.id;
    }
    REQUIRE(fs::exists(f.project.path() / "data/hello.txt.meta"));
    const auto meta = nlohmann::json::parse(*fs::read_file(f.project.path() / "data/hello.txt.meta"));
    CHECK(meta["id"] == first_id.to_string());
    CHECK(meta["loader"] == "text");

    // A fresh server (next run) reads the same id back.
    AssetServer again(f.world, f.project.path());
    again.add_loader(std::make_unique<TextLoader>());
    CHECK(again.load<TextAsset>("data/hello.txt").id == first_id);
}

TEST_CASE("[asset.AC2] missing assets fail with a suggestion") {
    AssetFixture f;
    f.project.write("data/hello.txt", "hi");
    AssetServer assets(f.world, f.project.path());
    assets.add_loader(std::make_unique<TextLoader>());
    const auto h = assets.load<TextAsset>("data/helo.txt");
    CHECK(assets.state(h.id) == AssetState::failed);
    CHECK(assets.get(h) == nullptr);
    const auto waited = assets.wait(h);
    REQUIRE_FALSE(waited.has_value());
    CHECK(waited.error().code == ErrorCode::asset_not_found);
    CHECK(waited.error().hint.find("Did you mean 'data/hello.txt'?") != std::string::npos);
}

TEST_CASE("[asset.AC3] unknown extensions and corrupt .meta files are reported") {
    AssetFixture f;
    f.project.write("a.bin", "x");
    f.project.write("b.txt", "x");
    f.project.write("b.txt.meta", R"({"version": 1, "id": "nope", "loader": "text"})");
    AssetServer assets(f.world, f.project.path());
    assets.add_loader(std::make_unique<TextLoader>());
    CHECK(assets.error(assets.load<TextAsset>("a.bin").id)->code == ErrorCode::asset_no_loader);
    CHECK(assets.error(assets.load<TextAsset>("b.txt").id)->code == ErrorCode::asset_meta_invalid);
}
