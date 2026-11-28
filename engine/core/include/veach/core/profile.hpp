#pragma once

// Tracy instrumentation (ADR 0014). Compiles to nothing unless the build was
// configured with VEACH_TRACY=ON (the 'profile' preset). Zone names must be
// string literals except for VEACH_ZONE_TEXT, which copies a runtime name.
//
// How to capture a trace headlessly and read it as text: docs/guides/profiling.md.

#if defined(VEACH_TRACY)
#include <tracy/Tracy.hpp>

#define VEACH_ZONE(name) ZoneScopedN(name)
/// Zone with a static label plus a runtime name (e.g. which script system ran).
#define VEACH_ZONE_TEXT(name, text, size) \
    ZoneScopedN(name);                    \
    ZoneText(text, size)
#define VEACH_FRAME_MARK() FrameMark
/// Names the calling thread in Tracy (the job system's workers). Tracy copies the name.
#define VEACH_THREAD_NAME(name) tracy::SetThreadName(name)
#define VEACH_PROFILER_ENABLED 1
#else
#define VEACH_ZONE(name) static_cast<void>(0)
#define VEACH_ZONE_TEXT(name, text, size) static_cast<void>(0)
#define VEACH_FRAME_MARK() static_cast<void>(0)
#define VEACH_THREAD_NAME(name) static_cast<void>(sizeof(name))
#define VEACH_PROFILER_ENABLED 0
#endif
