#pragma once

// File access. Paths are std::filesystem paths; text is UTF-8.

#include <veach/core/error.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace veach::fs {

using Path = std::filesystem::path;

[[nodiscard]] Result<std::string> read_file(const Path& path);
/// Writes atomically enough for tooling: write to a temp file, then rename.
[[nodiscard]] Result<> write_file(const Path& path, std::string_view contents);
[[nodiscard]] bool exists(const Path& path);

/// What file watching compares: a file changed if its write time or size did (hot reload, VCH-004).
struct FileStamp {
    std::filesystem::file_time_type time;
    std::uintmax_t size = 0;
    bool operator==(const FileStamp&) const = default;
};
/// The file's stamp now, or nothing if it does not exist.
[[nodiscard]] std::optional<FileStamp> stamp(const Path& path);

}  // namespace veach::fs
