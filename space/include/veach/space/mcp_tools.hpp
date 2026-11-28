#pragma once

// The space toolbox's MCP tools (VCH-100), registered through the engine's McpTools service
// (veach/runtime/mcp_tools.hpp). Import with engine.import<veach::space::SpaceMcpModule>(); `veach-lab --mcp` does.
//
//   orrery_state { system?, scale?, tick?, body?, latitude?, longitude? }
//       Every body's position, velocity, orbit and spin phase and the local solar time on its prime meridian, at a
//       tick, for an SP-04 preset (docs/research/spikes/sp-04-orrery-clock.md), computed on the spot: no paused clock
//       and no step_frames. With `body`, only that body, plus the star as seen from latitude/longitude on it.
//       Defaults: the world's Orrery and its UniverseClock tick if there is one, else Sol at J2000.0.
//   generate_system { seed, address, habitability_bias? }
//       SP-05's records for one system (docs/research/spikes/sp-05-system-statistics.md): S1 `space:system` and B1
//       `space:body.physics` of every body, with their stage hashes.
//
// 64-bit integers (ticks, seeds, hashes, the system index) are decimal strings in arguments and results (ADR 0046).
// Both tools are read-only: they never change the world.

#include <veach/veach.hpp>

namespace veach::space {

struct SpaceMcpModule {
    explicit SpaceMcpModule(World& world);
};

}  // namespace veach::space
