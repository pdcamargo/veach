#include <veach/core/log.hpp>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace veach::log {

namespace {

struct State {
    std::mutex mutex;
    Config config;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::vector<std::pair<SinkId, Sink>> sinks;
    SinkId next_sink = 1;
};

State& state() {
    static State s;
    return s;
}

void append_json_string(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

}  // namespace

std::string_view to_string(Level level) {
    switch (level) {
        case Level::trace: return "trace";
        case Level::debug: return "debug";
        case Level::info: return "info";
        case Level::warn: return "warn";
        case Level::error: return "error";
    }
    return "?";
}

void configure(const Config& config) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    s.config = config;
}

const Config& config() {
    return state().config;
}

SinkId add_sink(Sink sink) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    const SinkId id = s.next_sink++;
    s.sinks.emplace_back(id, std::move(sink));
    return id;
}

void remove_sink(SinkId id) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    std::erase_if(s.sinks, [id](const auto& entry) { return entry.first == id; });
}

std::string format_record(const Record& record, Format format) {
    std::string out;
    if (format == Format::json) {
        out = std::format(R"({{"t":{:.6f},"level":"{}","cat":)", record.seconds, to_string(record.level));
        append_json_string(out, record.category);
        out += R"(,"msg":)";
        append_json_string(out, record.message);
        if (!record.code.empty()) {
            out += R"(,"code":)";
            append_json_string(out, record.code);
        }
        out += '}';
    } else {
        out = std::format("[{:9.3f}] {:<5} {}: {}", record.seconds, to_string(record.level), record.category,
                          record.message);
        if (!record.code.empty()) {
            out += std::format(" [{}]", record.code);
        }
    }
    return out;
}

void write(Level level, std::string_view category, std::string_view message, std::string_view code) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    if (level < s.config.min_level) {
        return;
    }
    const Record record{
        .level = level,
        .category = category,
        .message = message,
        .code = code,
        .seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count(),
    };
    if (s.config.console) {
        const std::string line = format_record(record, s.config.format);
        std::FILE* out = s.config.console_stderr ? stderr : stdout;
        std::fwrite(line.data(), 1, line.size(), out);
        std::fputc('\n', out);
        std::fflush(out);
    }
    for (const auto& [id, sink] : s.sinks) {
        sink(record);
    }
}

}  // namespace veach::log
