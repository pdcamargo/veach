#pragma once

// Environment variables, portably: MSVC deprecates std::getenv with warning C4996, an error under /WX, so every read
// goes through here. tools/scripts/check_repo.py rejects getenv anywhere else.

#include <optional>
#include <string>

namespace veach {

/// The value of the environment variable `name`, or nullopt when it is not set.
[[nodiscard]] std::optional<std::string> env(const char* name);

/// True if `name` is set to exactly "1" (the convention of every VEACH_* switch).
[[nodiscard]] bool env_is_one(const char* name);

}  // namespace veach
