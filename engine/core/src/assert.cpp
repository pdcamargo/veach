#include <veach/core/assert.hpp>
#include <veach/core/log.hpp>

#include <cstdlib>

namespace veach::detail {

void panic(std::string_view message, std::source_location where) {
    log::write(log::Level::error, "core", std::format("{} ({}:{})", message, where.file_name(), where.line()),
               slug(ErrorCode::internal));
    std::abort();
}

}  // namespace veach::detail
