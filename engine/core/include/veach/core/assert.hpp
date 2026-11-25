#pragma once

// VEACH_ASSERT checks engine invariants in debug builds. VEACH_PANIC always
// aborts. Both log through veach::log with code core.internal before aborting.
// Use Result<T> for anything a user or data file can cause; asserts are for bugs.

#include <format>
#include <source_location>
#include <string_view>

namespace veach::detail {

[[noreturn]] void panic(std::string_view message, std::source_location where);

}  // namespace veach::detail

#define VEACH_PANIC(...) ::veach::detail::panic(std::format(__VA_ARGS__), std::source_location::current())

#ifdef NDEBUG
#define VEACH_ASSERT(cond, ...) static_cast<void>(sizeof(!(cond)))  // unevaluated: keeps variables "used"
#else
#define VEACH_ASSERT(cond, ...)                                                                               \
    do {                                                                                                      \
        if (!(cond)) [[unlikely]] {                                                                           \
            ::veach::detail::panic(std::format("assertion '{}' failed: {}", #cond, std::format(__VA_ARGS__)), \
                                   std::source_location::current());                                          \
        }                                                                                                     \
    } while (false)
#endif
