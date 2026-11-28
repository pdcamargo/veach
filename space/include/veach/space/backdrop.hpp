#pragma once

// A star system's sky beyond its bodies (docs/specs/space-backdrop/spec.md, ADR 0090): the engine's space backdrop
// (<veach/render/backdrop.hpp>) derived from the universe seed and where the system is in the galaxy (its address,
// ADR 0050). Canonical (veach::hash and dmath, ADRs 0043 and 0044): every system's sky differs, and a revisit, on
// any platform, sees the same one. Not a generation stage: it feeds pixels only, never gameplay.
//
//   const render::Backdrop sky = space::system_backdrop(config);   // in the system frame's axes
//
// The galaxy model is a stand-in until the galaxy query exists: a disk (scale length 30,000 ly, star formation in a
// ring around 30,000 ly) with a bulge at the center of the address space. Nearer the center the band is brighter,
// the bulge bigger and the anticenter as bright as the center; far above the plane the band is fainter and wider and
// lies below the system's horizon of the galaxy, and fewer nebulae are nearby.

#include <veach/render/backdrop.hpp>
#include <veach/space/realization.hpp>

namespace veach::space {

/// What the galaxy looks like from the system at `config.system`, in the system frame's axes (the galactic plane is
/// turned at random against the system's own: the hash of the seed and the address).
[[nodiscard]] render::GalaxyView galaxy_view(const UniverseConfig& config);

/// The system's backdrop: galaxy_view() and nebulae and noise from hash(seed, "space:backdrop", address).
/// `orientation` is in the system frame's axes; extractors turn it into render space every frame.
[[nodiscard]] render::Backdrop system_backdrop(const UniverseConfig& config);

}  // namespace veach::space
