#pragma once

// File access. Paths are std::filesystem paths; text is UTF-8.

#include <veach/core/error.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace veach::fs {

using Path = std::filesystem::path;

[[nodiscard]] Result<std::string> read_file(const Path& path);
/// Writes atomically enough for tooling: write to a temp file, then rename.
[[nodiscard]] Result<> write_file(const Path& path, std::string_view contents);
[[nodiscard]] bool exists(const Path& path);

}  // namespace veach::fs
