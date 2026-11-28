#pragma once

// Generation stages (docs/adr/0051-staged-generation-pipeline.md, docs/adr/0052-procgen-runtime.md,
// docs/specs/procgen/spec.md; guide: docs/guides/procgen.md).
//
// A stage is a pure function from its declared inputs (earlier stages, at its own address or an ancestor's) and its
// context (universe seed, address, its own id and version) to one record. It has no world access and no scale factor.
// The registry holds stages by (id, version), several versions of one id side by side
// (docs/adr/0055-universe-identity-and-versioning.md); the runtime runs them on demand and memoizes each output under
// (stage, address, input hash, manifest fingerprint).
//
//   veach::procgen::StageRegistry registry;
//   auto system = registry.add<SystemRecord>({.id = "space:system", .version = 1}, &system_hash,
//       [](const StageContext& ctx, const UniverseRecord& u) { return generate(ctx.seed(), ctx.address, u); },
//       veach::procgen::from(universe, veach::procgen::root_address));
//   veach::procgen::StageRuntime runtime(registry, {.universe_seed = 42});
//   auto record = runtime.get(*system, address);   // Result<std::shared_ptr<const SystemRecord>>
//
// Addresses are 64-bit words whose meaning the caller defines (docs/adr/0050-universe-addresses-and-seeds.md is the
// toolbox's); the runtime only hashes and compares them.

#include <veach/core/dmath.hpp>
#include <veach/core/error.hpp>
#include <veach/core/hash.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace veach::procgen {

/// How a stage's output may change across generator versions (docs/adr/0055-universe-identity-and-versioning.md).
enum class Stability : std::uint8_t {
    frozen,      ///< The version current at genesis, forever
    pinned,      ///< Pinned per body when the body is anchored
    refreshable  ///< Always the newest
};

/// Determinism tier (docs/adr/0043-determinism-tiers-and-strict-floating-point.md). Data flows only down the list.
enum class Tier : std::uint8_t { canonical, replay, visual };

using Address = std::uint64_t;
/// Maps a stage's address to the address of one of its inputs (an ancestor).
using AddressMap = Address (*)(Address);

[[nodiscard]] constexpr Address same_address(Address a) {
    return a;
}
/// The universe's own address, for whole-universe stages.
[[nodiscard]] constexpr Address root_address(Address /*a*/) {
    return 0;
}

struct StageInfo {
    std::string id;  ///< "namespace:name": lowercase letters, digits, '_', '.', '-'
    std::uint32_t version = 1;
    Stability stability = Stability::frozen;
    Tier tier = Tier::canonical;
};

/// A stage's seed: hash(universe_seed, address, hash_id(stage_id), version)
/// (docs/adr/0050-universe-addresses-and-seeds.md).
[[nodiscard]] constexpr std::uint64_t stage_seed(std::uint64_t universe_seed, Address address,
                                                 std::string_view stage_id, std::uint32_t version) {
    return hash(universe_seed, address, hash_id(stage_id), std::uint64_t{version});
}

/// What a stage sees besides its inputs. There is no scale factor, on purpose (docs/adr/0049-scale-model.md).
struct StageContext {
    std::uint64_t universe_seed = 0;
    Address address = 0;
    std::string_view stage_id;
    std::uint32_t stage_version = 0;

    [[nodiscard]] std::uint64_t seed() const { return stage_seed(universe_seed, address, stage_id, stage_version); }
    /// A named random stream of this stage (docs/adr/0044-counter-based-hash-randomness.md).
    [[nodiscard]] HashStream stream(std::string_view substream, std::uint64_t index = 0) const {
        return HashStream::make(seed(), substream, index);
    }
};

/// Canonical 64-bit hash of a record, fed field by field in declaration order: integers as they are, doubles by their
/// bits (every NaN as one value, docs/adr/0067-dmath-on-llvm-libc.md), strings by hash_id. It starts from the stage id,
/// its version and dmath::version, so a version bump changes every hash. Integer-only: canonical anywhere.
class RecordHasher {
public:
    RecordHasher(std::string_view stage_id, std::uint32_t version)
        : h_(hash(hash_id(stage_id), std::uint64_t{version}, std::uint64_t{dmath::version})) {}

    void add(std::uint64_t word) { h_ = hash(h_, word); }
    void add_double(double x) { add(x != x ? 0x7FF8000000000000ULL : std::bit_cast<std::uint64_t>(x)); }
    void add_id(std::string_view text) { add(hash_id(text)); }
    [[nodiscard]] std::uint64_t value() const { return h_; }

private:
    std::uint64_t h_;
};

/// A registered stage producing T. Valid in the registry that made it.
template <class T>
struct Stage {
    std::uint32_t index = 0;
};

/// One declared input: a stage, read at the address `at` maps this stage's address to.
template <class T>
struct Input {
    Stage<T> stage;
    AddressMap at = &same_address;
};

template <class T>
[[nodiscard]] constexpr Input<T> from(Stage<T> stage, AddressMap at = &same_address) {
    return {stage, at};
}

namespace detail {

template <class T>
const void* stage_type_key() {
    static const char tag = 0;
    return &tag;
}

using ErasedRun = std::function<Result<std::shared_ptr<const void>>(const StageContext&, std::span<const void* const>)>;
using ErasedHash = std::function<std::uint64_t(const void*)>;

struct StageDef {
    StageInfo info;
    const void* type = nullptr;
    std::vector<std::pair<std::uint32_t, AddressMap>> inputs;
    ErasedRun run;
    ErasedHash hash;
};

template <class Out, class... In, class Fn, std::size_t... I>
Result<std::shared_ptr<const void>> invoke(const Fn& fn, const StageContext& ctx, std::span<const void* const> in,
                                           std::index_sequence<I...> /*unused*/) {
    using R = std::invoke_result_t<const Fn&, const StageContext&, const In&...>;
    if constexpr (std::is_same_v<std::remove_cvref_t<R>, Result<Out>>) {
        Result<Out> r = fn(ctx, *static_cast<const In*>(in[I])...);
        if (!r) {
            return std::unexpected(std::move(r.error()));
        }
        return std::shared_ptr<const void>(std::make_shared<const Out>(std::move(*r)));
    } else {
        static_assert(std::is_convertible_v<R, Out>, "a stage function returns its record or Result<record>");
        return std::shared_ptr<const void>(std::make_shared<const Out>(fn(ctx, *static_cast<const In*>(in[I])...)));
    }
}

}  // namespace detail

class StageRegistry {
public:
    /// Registers a stage producing Out from `inputs` (earlier stages of this registry). `hash` is the output's
    /// canonical hash (RecordHasher); `fn(const StageContext&, const In&...)` returns Out or Result<Out>. Fails with
    /// procgen.stage_invalid on a malformed id, a version below 1, an (id, version) already registered, or an input in
    /// a lower tier than the stage.
    template <class Out, class Hash, class Fn, class... In>
        requires std::is_invocable_r_v<std::uint64_t, const Hash&, const Out&> &&
                 std::is_invocable_v<const Fn&, const StageContext&, const In&...>
    [[nodiscard]] Result<Stage<Out>> add(StageInfo info, Hash hash, Fn fn, Input<In>... inputs) {
        detail::StageDef def;
        def.info = std::move(info);
        def.type = detail::stage_type_key<Out>();
        (def.inputs.emplace_back(inputs.stage.index, inputs.at), ...);
        const std::array<const void*, sizeof...(In)> types{detail::stage_type_key<In>()...};
        def.run = [fn = std::move(fn)](const StageContext& ctx, std::span<const void* const> in) {
            return detail::invoke<Out, In...>(fn, ctx, in, std::index_sequence_for<In...>{});
        };
        def.hash = [hash = std::move(hash)](const void* record) -> std::uint64_t {
            return hash(*static_cast<const Out*>(record));
        };
        auto index = add_erased(std::move(def), types);
        if (!index) {
            return std::unexpected(std::move(index.error()));
        }
        return Stage<Out>{*index};
    }

    /// The stage (id, version), if registered with output type T.
    template <class T>
    [[nodiscard]] std::optional<Stage<T>> find(std::string_view id, std::uint32_t version) const {
        const std::optional<std::uint32_t> index = find_erased(id, version, detail::stage_type_key<T>());
        return index ? std::optional(Stage<T>{*index}) : std::nullopt;
    }
    /// Every registered version of `id`, ascending.
    [[nodiscard]] std::vector<std::uint32_t> versions(std::string_view id) const;
    template <class T>
    [[nodiscard]] const StageInfo& info(Stage<T> stage) const {
        return stages_.at(stage.index).info;
    }
    [[nodiscard]] std::size_t size() const { return stages_.size(); }

    [[nodiscard]] const detail::StageDef& def(std::uint32_t index) const { return stages_.at(index); }

private:
    Result<std::uint32_t> add_erased(detail::StageDef def, std::span<const void* const> input_types);
    std::optional<std::uint32_t> find_erased(std::string_view id, std::uint32_t version, const void* type) const;

    std::vector<detail::StageDef> stages_;
};

struct RuntimeOptions {
    std::uint64_t universe_seed = 0;
    /// The universe manifest's fingerprint (docs/adr/0055-universe-identity-and-versioning.md); part of every cache
    /// key.
    std::uint64_t manifest_fingerprint = 0;
    /// Most outputs kept, least recently used evicted first; 0 keeps everything. Overrides are never evicted.
    std::size_t cache_capacity = 0;
};

struct CacheStats {
    std::uint64_t hits = 0;
    std::uint64_t runs = 0;  ///< Stage functions run (misses)
    std::uint64_t evictions = 0;
    std::size_t entries = 0;
};

/// Runs stages on demand and memoizes them. Thread-safe: any job may call get() at once; a stage is pure, so two
/// jobs racing on one key compute the same record and either is kept. The registry must outlive the runtime.
class StageRuntime {
public:
    StageRuntime(const StageRegistry& registry, RuntimeOptions options);
    ~StageRuntime();
    StageRuntime(const StageRuntime&) = delete;
    StageRuntime& operator=(const StageRuntime&) = delete;

    /// The stage's record at `address`, running it (and its inputs) if not cached. A failing stage, or a failing input,
    /// is procgen.stage_failed naming the stage, version and address; nothing is cached.
    template <class T>
    [[nodiscard]] Result<std::shared_ptr<const T>> get(Stage<T> stage, Address address) {
        auto e = get_erased(stage.index, address);
        if (!e) {
            return std::unexpected(std::move(e.error()));
        }
        return std::static_pointer_cast<const T>(std::move(e->record));
    }
    /// The canonical hash of the stage's record at `address` (the registered hash function), computing it if needed.
    template <class T>
    [[nodiscard]] Result<std::uint64_t> hash(Stage<T> stage, Address address) {
        auto e = get_erased(stage.index, address);
        if (!e) {
            return std::unexpected(std::move(e.error()));
        }
        return e->hash;
    }
    /// Replaces the stage's record at `address` with an authored one (docs/adr/0051-staged-generation-pipeline.md
    /// overlays): get() returns it, and stages reading it see a new input hash and run again.
    template <class T>
    void override(Stage<T> stage, Address address, T record) {
        set_override(stage.index, address, std::make_shared<const T>(std::move(record)));
    }
    template <class T>
    void clear_override(Stage<T> stage, Address address) {
        set_override(stage.index, address, nullptr);
    }

    /// Drops every cached output (not the overrides).
    void clear();
    [[nodiscard]] CacheStats stats() const;
    [[nodiscard]] const RuntimeOptions& options() const;

private:
    struct Entry {
        std::shared_ptr<const void> record;
        std::uint64_t hash = 0;
    };
    struct Impl;

    Result<Entry> get_erased(std::uint32_t stage, Address address);
    void set_override(std::uint32_t stage, Address address, std::shared_ptr<const void> record);

    std::unique_ptr<Impl> impl_;
};

}  // namespace veach::procgen
