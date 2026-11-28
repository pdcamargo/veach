// The stage registry and runtime (docs/specs/procgen/spec.md). Nothing here touches a record's contents: outputs are
// whatever the stage functions return, and hashes come from the registered hash functions.

#include <veach/procgen/stage.hpp>

#include <algorithm>
#include <format>
#include <list>
#include <map>
#include <mutex>

namespace veach::procgen {

namespace {

constexpr std::string_view tier_name(Tier t) {
    switch (t) {
        case Tier::canonical: return "canonical";
        case Tier::replay: return "replay";
        case Tier::visual: return "visual";
    }
    return "?";
}

bool id_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

/// "namespace:name", both parts non-empty, one colon.
bool valid_id(std::string_view id) {
    const std::size_t colon = id.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == id.size()) {
        return false;
    }
    return std::all_of(id.begin(), id.begin() + static_cast<std::ptrdiff_t>(colon), id_char) &&
           std::all_of(id.begin() + static_cast<std::ptrdiff_t>(colon) + 1, id.end(), id_char);
}

}  // namespace

// ---- registry -------------------------------------------------------------------------------------------------------

Result<std::uint32_t> StageRegistry::add_erased(detail::StageDef def, std::span<const void* const> input_types) {
    const StageInfo& info = def.info;
    if (!valid_id(info.id)) {
        return make_error(ErrorCode::procgen_stage_invalid, std::format("stage id '{}' is malformed", info.id),
                          "stage ids are \"namespace:name\" in lowercase letters, digits, '_', '.' and '-', like "
                          "\"space:body.physics\"");
    }
    if (info.version < 1) {
        return make_error(ErrorCode::procgen_stage_invalid, std::format("stage '{}' has version 0", info.id),
                          "versions start at 1 and go up by one whenever any output changes");
    }
    if (find_erased(info.id, info.version, nullptr)) {
        return make_error(ErrorCode::procgen_stage_invalid,
                          std::format("stage '{}'@{} is registered twice", info.id, info.version),
                          "a changed stage is a new version beside the old one (ADR 0055): bump `version`");
    }
    for (std::size_t i = 0; i < def.inputs.size(); ++i) {
        const std::uint32_t index = def.inputs[i].first;
        if (index >= stages_.size() || stages_[index].type != input_types[i] || def.inputs[i].second == nullptr) {
            return make_error(
                ErrorCode::procgen_stage_invalid,
                std::format("stage '{}'@{}: input {} is not a stage of this registry", info.id, info.version, i),
                "pass the Stage<T> handles this registry's add() returned, with a non-null address map");
        }
        const StageInfo& input = stages_[index].info;
        if (input.tier > info.tier) {
            return make_error(ErrorCode::procgen_stage_invalid,
                              std::format("stage '{}'@{} is {} but reads '{}'@{}, which is {}", info.id, info.version,
                                          tier_name(info.tier), input.id, input.version, tier_name(input.tier)),
                              "data flows only from canonical to replay to visual (ADR 0043); compute what this stage "
                              "needs in a stage of its own tier or a lower one");
        }
    }
    stages_.push_back(std::move(def));
    return static_cast<std::uint32_t>(stages_.size() - 1);
}

std::optional<std::uint32_t> StageRegistry::find_erased(std::string_view id, std::uint32_t version,
                                                        const void* type) const {
    for (std::size_t i = 0; i < stages_.size(); ++i) {
        const detail::StageDef& s = stages_[i];
        if (s.info.id == id && s.info.version == version && (type == nullptr || s.type == type)) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return std::nullopt;
}

std::vector<std::uint32_t> StageRegistry::versions(std::string_view id) const {
    std::vector<std::uint32_t> out;
    for (const detail::StageDef& s : stages_) {
        if (s.info.id == id) {
            out.push_back(s.info.version);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---- runtime --------------------------------------------------------------------------------------------------------

struct StageRuntime::Impl {
    struct Key {
        std::uint32_t stage = 0;
        Address address = 0;
        std::uint64_t input_hash = 0;
        auto operator<=>(const Key&) const = default;
    };
    struct Cached {
        Entry entry;
        std::list<Key>::iterator lru;
    };

    const StageRegistry& registry;
    RuntimeOptions options;
    mutable std::mutex mutex;
    std::map<Key, Cached> cache;
    std::list<Key> lru;  ///< most recently used first
    std::map<std::pair<std::uint32_t, Address>, Entry> overrides;
    CacheStats stats;

    Impl(const StageRegistry& r, RuntimeOptions o) : registry(r), options(o) {}

    void evict_locked() {
        while (options.cache_capacity > 0 && cache.size() > options.cache_capacity) {
            cache.erase(lru.back());
            lru.pop_back();
            ++stats.evictions;
        }
    }
};

StageRuntime::StageRuntime(const StageRegistry& registry, RuntimeOptions options)
    : impl_(std::make_unique<Impl>(registry, options)) {}
StageRuntime::~StageRuntime() = default;

Result<StageRuntime::Entry> StageRuntime::get_erased(std::uint32_t stage, Address address) {
    const detail::StageDef& def = impl_->registry.def(stage);
    {
        const std::scoped_lock lock(impl_->mutex);
        if (const auto it = impl_->overrides.find({stage, address}); it != impl_->overrides.end()) {
            return it->second;
        }
    }

    // Inputs first: their hashes are part of this output's key.
    std::vector<Entry> inputs;
    inputs.reserve(def.inputs.size());
    std::uint64_t input_hash = veach::hash(impl_->options.manifest_fingerprint, def.inputs.size());
    for (const auto& [index, at] : def.inputs) {
        auto in = get_erased(index, at(address));
        if (!in) {
            return std::unexpected(std::move(in.error()));  // already names the failing stage
        }
        input_hash = veach::hash(input_hash, in->hash);
        inputs.push_back(std::move(*in));
    }

    const Impl::Key key{stage, address, input_hash};
    {
        const std::scoped_lock lock(impl_->mutex);
        if (const auto it = impl_->cache.find(key); it != impl_->cache.end()) {
            impl_->lru.splice(impl_->lru.begin(), impl_->lru, it->second.lru);
            ++impl_->stats.hits;
            return it->second.entry;
        }
    }

    std::vector<const void*> records;
    records.reserve(inputs.size());
    for (const Entry& e : inputs) {
        records.push_back(e.record.get());
    }
    const StageContext ctx{impl_->options.universe_seed, address, def.info.id, def.info.version};
    auto record = def.run(ctx, records);
    if (!record) {
        const std::scoped_lock lock(impl_->mutex);
        ++impl_->stats.runs;
        return make_error(
            ErrorCode::procgen_stage_failed,
            std::format("stage '{}'@{} at address 0x{:016x}: {}", def.info.id, def.info.version, address,
                        record.error().message),
            record.error().hint.empty() ? std::string("nothing of the stage's output was kept") : record.error().hint);
    }
    Entry entry{std::move(*record), 0};
    entry.hash = def.hash(entry.record.get());

    const std::scoped_lock lock(impl_->mutex);
    ++impl_->stats.runs;
    if (const auto it = impl_->cache.find(key); it != impl_->cache.end()) {
        return it->second.entry;  // another job got there first; the records are equal
    }
    impl_->lru.push_front(key);
    impl_->cache.emplace(key, Impl::Cached{entry, impl_->lru.begin()});
    impl_->evict_locked();
    return entry;
}

void StageRuntime::set_override(std::uint32_t stage, Address address, std::shared_ptr<const void> record) {
    const detail::StageDef& def = impl_->registry.def(stage);
    const std::scoped_lock lock(impl_->mutex);
    if (record == nullptr) {
        impl_->overrides.erase({stage, address});
        return;
    }
    const std::uint64_t h = def.hash(record.get());
    impl_->overrides[{stage, address}] = Entry{std::move(record), h};
}

void StageRuntime::clear() {
    const std::scoped_lock lock(impl_->mutex);
    impl_->cache.clear();
    impl_->lru.clear();
}

CacheStats StageRuntime::stats() const {
    const std::scoped_lock lock(impl_->mutex);
    CacheStats s = impl_->stats;
    s.entries = impl_->cache.size();
    return s;
}

const RuntimeOptions& StageRuntime::options() const {
    return impl_->options;
}

}  // namespace veach::procgen
