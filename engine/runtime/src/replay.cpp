#include <veach/runtime/replay.hpp>

#include <veach/ecs/components.hpp>
#include <veach/ecs/json.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace veach {

namespace {

struct Fnv64 {
    std::uint64_t h = 1469598103934665603ull;
    void add(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            h = (h ^ bytes[i]) * 1099511628211ull;
        }
    }
    void add(std::string_view text) { add(text.data(), text.size()); }
};

std::unexpected<Error> invalid(const std::filesystem::path& path, std::string_view why) {
    return make_error(ErrorCode::replay_invalid,
                      std::format("'{}' is not a recording Veach can replay: {}", path.string(), why),
                      "Record it again with --record, with this build of the engine.");
}

nlohmann::json event_json(const PlatformEvent& e) {
    nlohmann::json out = {{"type", static_cast<int>(e.type)},
                          {"window", e.window},
                          {"key", static_cast<int>(e.key)},
                          {"width", e.width},
                          {"height", e.height}};
    if (e.type == PlatformEvent::Type::axis_moved) {
        out["axis"] = static_cast<int>(e.axis);
        out["value"] = e.value;  // fixed point (axis_one = 1.0): an integer, so it replays bit for bit
    }
    return out;
}

}  // namespace

StateHash simulation_hash(const World& world) {
    std::vector<std::pair<std::string, ecs::entity_t>> types;
    world.each([&](Entity e, const flecs::Component& c) {
        if (c.size > 0 && e.has<Scriptable>()) {
            types.emplace_back(e.path("::", "").c_str(), e.id());
        }
    });
    std::ranges::sort(types);
    StateHash out;
    Fnv64 total;
    for (const auto& [name, type] : types) {
        const auto size = static_cast<std::size_t>(ecs_get(world.c_ptr(), type, EcsComponent)->size);
        ecs_query_desc_t desc{};
        desc.terms[0].id = type;
        ecs_query_t* query = ecs_query_init(world.c_ptr(), &desc);
        std::vector<std::pair<ecs::entity_t, std::string>> rows;
        ecs_iter_t it = ecs_query_iter(world.c_ptr(), query);
        while (ecs_query_next(&it)) {
            const auto* base = static_cast<const char*>(ecs_field_w_size(&it, size, 0));
            for (int i = 0; i < it.count; ++i) {
                rows.emplace_back(it.entities[i],
                                  to_json(world, type, base + static_cast<std::size_t>(i) * size).dump());
            }
        }
        ecs_query_fini(query);
        std::ranges::sort(rows);
        Fnv64 h;
        for (const auto& [entity, value] : rows) {
            h.add(&entity, sizeof(entity));
            h.add(value);
        }
        out.components[name] = h.h;
        total.add(name);
        total.add(&h.h, sizeof(h.h));
    }
    out.total = total.h;
    return out;
}

Result<ReplayRecorder> ReplayRecorder::open(const std::filesystem::path& path, const ReplayHeader& header) {
    ReplayRecorder recorder;
    recorder.out_.open(path, std::ios::binary | std::ios::trunc);
    if (!recorder.out_) {
        return make_error(ErrorCode::replay_invalid, std::format("cannot write the recording '{}'", path.string()),
                          "Check the directory exists and is writable.");
    }
    const nlohmann::json line = {{"format", replay_format},   {"engine", header.engine_version},
                                 {"project", header.project}, {"script", header.script},
                                 {"seed", header.seed},       {"fixed_step", header.fixed_step}};
    recorder.out_ << line.dump() << '\n';
    return recorder;
}

void ReplayRecorder::write(const ReplayFrame& frame) {
    nlohmann::json events = nlohmann::json::array();
    for (const PlatformEvent& e : frame.events) {
        events.push_back(event_json(e));
    }
    nlohmann::json components = nlohmann::json::object();
    for (const auto& [name, hash] : frame.state.components) {
        components[name] = std::format("{:016x}", hash);
    }
    nlohmann::json line = {{"frame", frame.frame},
                           {"steps", frame.steps},
                           {"events", std::move(events)},
                           {"state", std::format("{:016x}", frame.state.total)},
                           {"components", std::move(components)}};
    if (!frame.edits.empty()) {
        nlohmann::json edits = nlohmann::json::array();
        for (const ReplayEdit& e : frame.edits) {
            edits.push_back({{"tool", e.tool}, {"arguments", nlohmann::json::parse(e.arguments)}});
        }
        line["edits"] = std::move(edits);
    }
    out_ << line.dump() << '\n';
    out_.flush();  // a crash still leaves the frames before it
}

Result<ReplayLog> read_replay(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return invalid(path, "it cannot be opened");
    }
    ReplayLog log;
    std::string text;
    std::size_t line_number = 0;
    const auto hex = [](const nlohmann::json& v) {
        return std::stoull(v.get<std::string>(), nullptr, 16);
    };
    try {
        while (std::getline(in, text)) {
            ++line_number;
            if (text.empty()) {
                continue;
            }
            const nlohmann::json line = nlohmann::json::parse(text);
            if (line_number == 1) {
                if (line.value("format", "") != replay_format) {
                    return invalid(
                        path, std::format("its format is '{}', not '{}'", line.value("format", "?"), replay_format));
                }
                log.header = {line.at("engine").get<std::string>(), line.at("project").get<std::string>(),
                              line.at("script").get<std::string>(), line.at("seed").get<std::uint64_t>(),
                              line.at("fixed_step").get<double>()};
                continue;
            }
            ReplayFrame frame;
            frame.frame = line.at("frame").get<std::uint64_t>();
            frame.steps = line.at("steps").get<std::uint32_t>();
            for (const nlohmann::json& e : line.at("events")) {
                PlatformEvent event;
                event.type = static_cast<PlatformEvent::Type>(e.at("type").get<int>());
                event.window = e.at("window").get<std::uint32_t>();
                event.key = static_cast<Key>(e.at("key").get<int>());
                event.width = e.at("width").get<std::uint32_t>();
                event.height = e.at("height").get<std::uint32_t>();
                event.axis = static_cast<Axis>(e.value("axis", 0));
                event.value = e.value("value", std::int32_t{0});
                frame.events.push_back(event);
            }
            if (line.contains("edits")) {
                for (const nlohmann::json& e : line.at("edits")) {
                    frame.edits.push_back({e.at("tool").get<std::string>(), e.at("arguments").dump()});
                }
            }
            frame.state.total = hex(line.at("state"));
            for (const auto& [name, hash] : line.at("components").items()) {
                frame.state.components[name] = hex(hash);
            }
            log.frames.push_back(std::move(frame));
        }
    } catch (const std::exception& e) {
        return invalid(path, std::format("line {}: {}", line_number, e.what()));
    }
    if (line_number == 0) {
        return invalid(path, "it is empty");
    }
    return log;
}

}  // namespace veach
