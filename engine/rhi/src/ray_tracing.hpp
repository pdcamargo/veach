#pragma once

// Hardware ray tracing is optional (ADR 0003): only photo mode uses it. VEACH_DISABLE_RAY_TRACING=1 makes every
// backend report none, so a machine that has it can verify that the real-time path never needs it.

namespace veach::rhi {

/// True when VEACH_DISABLE_RAY_TRACING=1: backends then report Capabilities::hardware_ray_tracing = false.
[[nodiscard]] bool ray_tracing_disabled();

}  // namespace veach::rhi
