// Jolt's process-wide state for the physics spikes (SP-11, SP-13): allocator hooks, the type factory and the
// collision dispatch tables, initialized once per process (docs/adr/0075-amendments-from-sp-11.md, decision 5).
// Include only from .cpp files that link Jolt; it is an inline function, so every spike in a binary shares one.
#pragma once

#include <Jolt/Jolt.h>  // Must come first.

#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

#include <cstdarg>
#include <cstdio>

namespace veach::test {

inline void jolt_trace(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

#ifdef JPH_ENABLE_ASSERTS
inline bool jolt_assert_failed(const char* expression, const char* message, const char* file, JPH::uint line) {
    std::fprintf(stderr, "Jolt assert %s:%u: (%s) %s\n", file, line, expression, message ? message : "");
    return true;  // Break.
}
#endif

struct JoltRuntime {
    JoltRuntime() {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = jolt_trace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = jolt_assert_failed;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    ~JoltRuntime() {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
    JoltRuntime(const JoltRuntime&) = delete;
    JoltRuntime& operator=(const JoltRuntime&) = delete;
    JoltRuntime(JoltRuntime&&) = delete;
    JoltRuntime& operator=(JoltRuntime&&) = delete;
};

/// Initializes Jolt the first time it is called, in any spike; torn down at exit.
inline void ensure_jolt() {
    static const JoltRuntime runtime;
}

}  // namespace veach::test
