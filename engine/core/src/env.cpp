#include <veach/core/env.hpp>

#include <cstdlib>

namespace veach {

std::optional<std::string> env(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t length = 0;
    std::optional<std::string> result;
    if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) {
        result = value;
    }
    std::free(value);  // NOLINT(cppcoreguidelines-no-malloc)
    return result;
#else
    const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read, never written by the engine
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
#endif
}

bool env_is_one(const char* name) {
    return env(name) == "1";
}

}  // namespace veach
