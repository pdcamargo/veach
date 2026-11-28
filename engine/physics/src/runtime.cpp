// Jolt's process-wide state (ADR 0075 decision 5): initialized once, the first time any physics code asks, and torn
// down at exit.

#include "jolt.hpp"

#include <veach/core/log.hpp>

#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

#include <cstdarg>
#include <cstdio>

namespace veach::physics {
namespace {

void trace(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log::warn("physics", "jolt: {}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool assert_failed(const char* expression, const char* message, const char* file, JPH::uint line) {
    log::error("physics", "Jolt assert {}:{}: ({}) {}", file, line, expression, message != nullptr ? message : "");
    return true;  // break: a Jolt assert is an engine bug
}
#endif

struct Runtime {
    Runtime() {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = trace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = assert_failed;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        ring = new JPH::PhysicsMaterial();
    }
    ~Runtime() {
        ring = nullptr;
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    JPH::RefConst<JPH::PhysicsMaterial> ring;
};

const Runtime& runtime() {
    static const Runtime instance;  // thread-safe initialization
    return instance;
}

}  // namespace

void initialize() {
    static_cast<void>(runtime());
}

namespace detail {
const JPH::PhysicsMaterial* ring_material() {
    return runtime().ring.GetPtr();
}
}  // namespace detail

}  // namespace veach::physics
