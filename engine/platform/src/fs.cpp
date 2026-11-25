#include <veach/platform/fs.hpp>

#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

namespace veach::fs {

Result<std::string> read_file(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return make_error(ErrorCode::io_failed, std::format("cannot open '{}' for reading", path.generic_string()),
                          "Check that the path exists and is relative to the project directory.");
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return std::move(contents).str();
}

Result<> write_file(const Path& path, std::string_view contents) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    const Path temp = Path(path).concat(".tmp");
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return make_error(ErrorCode::io_failed, std::format("cannot open '{}' for writing", temp.generic_string()));
        }
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!file) {
            return make_error(ErrorCode::io_failed, std::format("failed writing '{}'", temp.generic_string()));
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        return make_error(ErrorCode::io_failed,
                          std::format("cannot replace '{}': {}", path.generic_string(), ec.message()));
    }
    return {};
}

bool exists(const Path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::optional<FileStamp> stamp(const Path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::nullopt;
    }
    return FileStamp{time, size};
}

}  // namespace veach::fs
