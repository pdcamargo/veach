#pragma once

// The procedural space backdrop (docs/specs/space-backdrop/spec.md, ADR 0091): what lies beyond every body: a dense
// field of stars with black-body colors and a realistic spread of magnitudes, a galactic band with dust lanes and
// star clouds, and nebulae (emission, reflection, dark). Pure data: a module's extractor (or the SpaceBackdrop
// component's seed) fills ResolvedSky::backdrop, the renderer bakes the diffuse part once into a texture in galactic
// coordinates and draws the stars per pixel behind everything, with the creator's exposure (SpaceBackdrop).
//
//   const render::GalaxyView view = render::galaxy_view_from_seed(7);  // or a module's own, from where it is
//   render::Backdrop b = render::generate_backdrop(7, view);            // canonical: the same on every platform
//
// Directions in a Backdrop are in its galactic frame: +X toward the galactic center, +Y the north galactic pole.
// `orientation` puts that frame into render space; extractors set it every frame (render space turns with the
// camera's reference frame).

#include <veach/core/math.hpp>

#include <array>
#include <cstdint>

namespace veach::render {

enum class NebulaKind : std::uint32_t {
    emission,    ///< H II region: H-alpha red-pink filaments around an O III teal core
    reflection,  ///< dust lit by nearby hot stars: soft blue
    dark,        ///< a dust cloud: absorbs the light behind it (and hides the stars there)
};

/// One nebula of a backdrop.
struct BackdropNebula {
    math::vec3 direction{1.0f, 0.0f, 0.0f};  ///< its center, galactic frame, unit
    float radius = 0.1f;                     ///< angular radius, radians
    math::vec3 color{1.0f, 0.2f, 0.3f};      ///< linear RGB: emission's rim (H-alpha), reflection's color
    float brightness = 2.0f;  ///< emission, reflection: peak radiance / the band's; dark: peak optical depth
    math::vec3 core_color{0.2f, 0.8f, 0.8f};  ///< emission: the core (O III)
    float core = 0.4f;                        ///< emission: the core's share, 0..1
    NebulaKind kind = NebulaKind::emission;
    float elongation = 1.0f;  ///< the long axis over the short one, 1..3
    float angle = 0.0f;       ///< the long axis's position angle, radians
    std::uint32_t seed = 0;   ///< its noise
    bool operator==(const BackdropNebula&) const = default;
};

inline constexpr std::size_t max_backdrop_nebulae = 8;

/// What to draw beyond every body. Everything but `orientation` is baked (a change rebakes the texture).
struct Backdrop {
    std::uint32_t seed = 0;        ///< the band's noise and the stars
    math::mat3 orientation{1.0f};  ///< the galactic frame's axes in render space (columns): set every frame
    // The galactic band: integrated starlight along the galaxy's plane.
    float band_brightness = 4e-4f;  ///< nits, the band's brightest (toward the center); the Milky Way is ~5e-4
    float band_latitude = 0.0f;     ///< radians: the band's center line (an observer above the plane sees it below)
    float band_width = 0.12f;       ///< radians: the angular half-thickness of the disk's light
    float bulge = 1.0f;             ///< the central bulge's peak over the band's
    float anticenter = 0.3f;        ///< the band's brightness toward the anticenter over the center's
    float dust = 1.0f;              ///< the dust lanes' optical depth, scaled
    // Stars (drawn per pixel: never baked).
    float star_density = 1.0f;           ///< stars per cell, scaled (1: about 300,000 over the sky)
    float star_magnitude_scale = 0.85f;  ///< magnitudes are multiplied by this (1: the physical spread)
    float star_saturation = 1.15f;       ///< black-body colors pushed from white by this (1: physical)
    std::array<BackdropNebula, max_backdrop_nebulae> nebulae{};
    std::uint32_t nebula_count = 0;
    std::uint32_t resolution = 4096;  ///< the baked texture's width (height: half), texels

    bool operator==(const Backdrop&) const = default;
    /// The same baked texture: equal in everything but orientation and the star settings.
    [[nodiscard]] bool bakes_like(const Backdrop& other) const;
};

/// What the galaxy looks like from where the observer is, in the observer's frame: generate_backdrop's input.
struct GalaxyView {
    math::dvec3 north{0.0, 1.0, 0.0};   ///< the north galactic pole, unit
    math::dvec3 center{1.0, 0.0, 0.0};  ///< toward the galactic center, unit, perpendicular to north
    double band_brightness = 4e-4;      ///< Backdrop's fields of the same names
    double band_latitude = 0.0;
    double band_width = 0.12;
    double bulge = 1.0;
    double anticenter = 0.3;
    double dust = 1.0;
    double nebulae = 1.0;  ///< the expected number of nebulae, scaled (star-forming regions nearby)
};

/// An Earth-like view (a disk galaxy seen from 26,000 ly out, near its plane) turned at random by `seed`. Canonical.
[[nodiscard]] GalaxyView galaxy_view_from_seed(std::uint64_t seed);

/// A backdrop for `galaxy` with nebulae and noise from `seed`. Canonical (veach::hash and dmath, ADR 0043): the same
/// bits on every platform, so a system's sky is the same on every revisit.
[[nodiscard]] Backdrop generate_backdrop(std::uint64_t seed, const GalaxyView& galaxy);

/// A hash of every field's bits, for tests that pin a generator.
[[nodiscard]] std::uint64_t backdrop_hash(const Backdrop& backdrop);

/// The color of a black body at `kelvin` in linear Rec. 709, at unit luminance (Krystek's 1985 fit of the Planckian
/// locus, 1000 K to 15,000 K; clamped there). The star shader uses the same fit.
[[nodiscard]] math::vec3 blackbody_color(float kelvin);

}  // namespace veach::render
