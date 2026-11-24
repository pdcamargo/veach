#pragma once

// Errors that tell you the fix. Every error carries a stable code (see
// error_codes.inl and docs/reference/errors.md), a message saying what happened,
// and a hint saying what to do about it.

#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace veach {

enum class ErrorCode : std::uint16_t {
#define VEACH_ERROR(name, slug, summary) name,
#include <veach/core/error_codes.inl>
#undef VEACH_ERROR
};

/// The stable slug, e.g. "script.compile_failed". Used in logs and docs anchors.
[[nodiscard]] std::string_view slug(ErrorCode code);
/// One-line description of the error class.
[[nodiscard]] std::string_view summary(ErrorCode code);
[[nodiscard]] std::span<const ErrorCode> all_error_codes();

struct Error {
    ErrorCode code = ErrorCode::internal;
    std::string message;  ///< What happened, with the concrete values involved.
    std::string hint;     ///< How to fix it. Empty only when there is nothing useful to say.

    /// "message — hint [slug]"
    [[nodiscard]] std::string to_string() const;
};

template <class T = void>
using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error> make_error(ErrorCode code, std::string message, std::string hint = {}) {
    return std::unexpected(Error{code, std::move(message), std::move(hint)});
}

}  // namespace veach

template <>
struct std::formatter<veach::Error> : std::formatter<std::string_view> {
    auto format(const veach::Error& error, std::format_context& ctx) const {
        return std::formatter<std::string_view>::format(error.to_string(), ctx);
    }
};
