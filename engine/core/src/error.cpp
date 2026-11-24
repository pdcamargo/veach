#include <veach/core/error.hpp>

#include <array>

namespace veach {

namespace {

struct ErrorInfo {
    std::string_view slug;
    std::string_view summary;
};

constexpr std::array error_infos{
#define VEACH_ERROR(name, slug, summary) ErrorInfo{slug, summary},
#include <veach/core/error_codes.inl>
#undef VEACH_ERROR
};

constexpr std::array error_codes{
#define VEACH_ERROR(name, slug, summary) ErrorCode::name,
#include <veach/core/error_codes.inl>
#undef VEACH_ERROR
};

}  // namespace

std::string_view slug(ErrorCode code) {
    return error_infos.at(static_cast<std::size_t>(code)).slug;
}

std::string_view summary(ErrorCode code) {
    return error_infos.at(static_cast<std::size_t>(code)).summary;
}

std::span<const ErrorCode> all_error_codes() {
    return error_codes;
}

std::string Error::to_string() const {
    if (hint.empty()) {
        return std::format("{} [{}]", message, slug(code));
    }
    return std::format("{} — {} [{}]", message, hint, slug(code));
}

}  // namespace veach
