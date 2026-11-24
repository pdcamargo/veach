#pragma once

// Shared helpers for the test suite. Tests map to spec acceptance criteria:
// each TEST_CASE name starts with the criterion id, e.g. "[lifecycle.AC1]".

#include <veach/veach.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace veach::test {

struct CapturedRecord {
    log::Level level;
    std::string category;
    std::string message;
    std::string code;
};

/// Captures every log record while alive.
class LogCapture {
public:
    LogCapture() {
        id_ = log::add_sink([this](const log::Record& r) {
            records.push_back({r.level, std::string(r.category), std::string(r.message), std::string(r.code)});
        });
    }
    ~LogCapture() { log::remove_sink(id_); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    LogCapture(LogCapture&&) = delete;
    LogCapture& operator=(LogCapture&&) = delete;

    /// Info-level messages of one category (what a user sees by default).
    [[nodiscard]] std::vector<std::string> messages(std::string_view category) const {
        std::vector<std::string> out;
        for (const auto& r : records) {
            if (r.category == category && r.level == log::Level::info) {
                out.push_back(r.message);
            }
        }
        return out;
    }

    [[nodiscard]] bool contains(std::string_view needle) const {
        for (const auto& r : records) {
            if (r.message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool has_code(std::string_view code) const {
        for (const auto& r : records) {
            if (r.code == code) {
                return true;
            }
        }
        return false;
    }

    std::vector<CapturedRecord> records;

private:
    log::SinkId id_ = 0;
};

/// A throwaway project directory, deleted on destruction.
class TempProject {
public:
    explicit TempProject(std::string_view name) {
        path_ = std::filesystem::temp_directory_path() /
                std::format("veach-test-{}-{}", name, AssetId::generate().to_string().substr(0, 8));
        std::filesystem::create_directories(path_);
    }
    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempProject(const TempProject&) = delete;
    TempProject& operator=(const TempProject&) = delete;
    TempProject(TempProject&&) = delete;
    TempProject& operator=(TempProject&&) = delete;

    void write(const std::string& relative, std::string_view contents) const {
        auto written = fs::write_file(path_ / relative, contents);
        VEACH_ASSERT(written.has_value(), "failed to write test file {}", relative);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

/// Headless, deterministic, quiet engine config.
inline EngineConfig headless_config(const std::filesystem::path& project_dir, std::string script = {},
                                    std::uint64_t frames = 3) {
    EngineConfig config;
    config.app.name = "veach-tests";
    config.project_dir = project_dir;
    config.project.headless = true;
    config.project.frames = frames;
    config.project.script = std::move(script);
    config.project.log_level = log::Level::debug;
    config.log_to_console = false;
    return config;
}

inline std::filesystem::path source_dir() {
    return VEACH_SOURCE_DIR;
}

}  // namespace veach::test
