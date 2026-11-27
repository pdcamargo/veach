#pragma once

// Wall-clock time. Only the main loop uses this, to decide how many fixed steps
// to run; simulation code reads veach::Time instead (ADR 0013).

namespace veach::clock {

/// Monotonic seconds since an arbitrary epoch.
[[nodiscard]] double now_seconds();

}  // namespace veach::clock
