#pragma once

// Structured logging. Every record has a level, a category ("engine", "script",
// ...) and a message; errors also carry their stable code. Output is either
// human-readable lines or JSON lines (one object per line) for machines.
//
//   veach::log::info("engine", "loaded {} assets", count);
//   veach::log::error("script", error);   // logs Error with its code

#include <veach/core/error.hpp>

#include <cstdint>
#include <format>
#include <functional>
#include <string_view>

namespace veach::log {

enum class Level : std::uint8_t { trace, debug, info, warn, error };
enum class Format : std::uint8_t { human, json };

struct Record {
    Level level = Level::info;
    std::string_view category;
    std::string_view message;
    std::string_view code;  ///< Error slug, empty for non-errors.
    double seconds = 0.0;   ///< Wall-clock seconds since the log was configured.
};

struct Config {
    Format format = Format::human;
    Level min_level = Level::info;
    bool console = true;  ///< Write to stdout.
};

using Sink = std::function<void(const Record&)>;
using SinkId = std::uint32_t;

void configure(const Config& config);
[[nodiscard]] const Config& config();

/// Additional sinks receive every record at or above the configured level.
/// Tests use this to capture output; tools may forward logs elsewhere.
SinkId add_sink(Sink sink);
void remove_sink(SinkId id);

void write(Level level, std::string_view category, std::string_view message, std::string_view code = {});

/// Formats a record the way the console sink would (no trailing newline).
[[nodiscard]] std::string format_record(const Record& record, Format format);

[[nodiscard]] std::string_view to_string(Level level);

template <class... Args>
void trace(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    if (config().min_level <= Level::trace) {
        write(Level::trace, category, std::format(fmt, std::forward<Args>(args)...));
    }
}

template <class... Args>
void debug(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    if (config().min_level <= Level::debug) {
        write(Level::debug, category, std::format(fmt, std::forward<Args>(args)...));
    }
}

template <class... Args>
void info(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::info, category, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void warn(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::warn, category, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void error(std::string_view category, std::format_string<Args...> fmt, Args&&... args) {
    write(Level::error, category, std::format(fmt, std::forward<Args>(args)...));
}

inline void error(std::string_view category, const Error& err) {
    write(Level::error, category, err.hint.empty() ? err.message : err.message + " — " + err.hint, slug(err.code));
}

}  // namespace veach::log
