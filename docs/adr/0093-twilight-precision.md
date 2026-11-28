# 0093: Atmosphere luminance tables in float32; a march step is lit for its part outside the planet's shadow

Date: 2026-11-28

## Status

Accepted. Amends [0038](0038-physical-sky.md) (the tables' format) and
[0092](0092-aerial-perspective-follows-the-air.md) (decision 1, the volume's format). Fixes the twilight bands noted in
VCH-136 ([spec](../specs/planet-atmosphere/spec.md) atmosphere.AC7,
[screenshots](../research/spikes/twilight/README.md)).

## Context

From the sun a few degrees below the horizon to the end of twilight, the slice drew the sky in hard concentric bands
with stepped edges, at both scales. Two causes, each confirmed by removing it alone:

1. **Precision.** The tables hold luminance per lux of sun at the top of the atmosphere (0038). Opposite the sun 10
   degrees into twilight that is a few 1e-7 (all of it multiple scattering: the air the camera sees there is in the
   planet's shadow), and it falls further as the sun sinks. rgba16_float's smallest normal number is 6.1e-5; below
   it, its denormals step by a fixed 6e-8, so the sky-view and multiple-scattering tables stored that sky in five or
   six levels, each channel on its own (hence the hue jumps between bands). The irradiance table and the
   aerial-perspective volume hold the same units. At 18 degrees down the stored sky was exactly zero.
2. **The planet's shadow in the march.** Each step of the sky's march (16 per ray, 0076) took the sun's light at its
   middle, lit or not. Where the shadow's edge crosses lit air near the camera (s = 0.01, the 10 km atmosphere on a
   64 km planet), a step switched on or off as a whole between neighboring rays: arcs with blotchy edges, gone at
   128 steps.

## Decision

1. **The tables that hold luminance are rgba32_float:** multiple scattering, the sky view, the sky irradiance and
   the aerial-perspective volume. Transmittance (0 to 1, and read most often) stays rgba16_float; making it float32
   too changed the twilight image by a mean FLIP of 0.002.
2. **A march step is lit for the part of it outside the planet's shadow.** Each ray finds, once per sun, where it is
   in the shadow (`sphere_shadow_span`: the cylinder of the planet's radius behind its center, the ray form of
   `sphere_blocks`, with a CPU twin and checked at 1e4 km like the other sphere helpers). A step's single scattering
   is weighted by its lit fraction and takes the sun's transmittance in the middle of its lit part, so the shadow's
   edge slides smoothly through the steps. Multiple scattering is unchanged. Both marches (the sky view and the
   per-pixel march, through `planet_march`), the aerial-perspective volume and the Sky component's flat-world march
   do it.

## Consequences

- Twilight is a gradient at both scales: deep blue overhead, the belt of pink above the planet's shadow rising
  opposite the sun, down to a faint sky at 18 degrees that auto-exposure leaves black next to the stars, as it should.
- With the same 16 steps the sky converges faster: at 128 steps the lit fraction is within a mean FLIP of 0.002 of
  512 steps, where the whole-step test was 0.012 off.
- GPU cost, release, M5 Max, 1080p, median of 3 x 40 frames: on the ground at noon the sky view goes from 0.026 to
  0.033 ms, the volume from 0.028 to 0.036 ms, shading from 0.092 to 0.098 ms (+0.02 ms in all); in orbit, where
  every pixel marches, shading goes from 0.205 to 0.254 ms and the volume from 0.038 to 0.046 ms (+0.06 ms). The
  volume takes 7.4 MB at 1440p (3.7 MB more), the tables 0.2 MB more.
- Golden tests pin it: `golden: planet sky at twilight (s = 1)` and `(s = 0.01)`, through SP-10's lab path. The old
  tables fail the first by a mean FLIP of 0.22, the whole-step march the second by 0.055.

## Alternatives considered

- **rgba16_float with a fixed scale:** the luminance to keep spans about 1e11, from a hazy aureole near the sun (of
  order 10 per lux) to the end of twilight (under 1e-9); half floats hold 1e9 before their denormals. Any scale
  either overflows near the sun or bands at night.
- **Storing logarithms in half floats:** keeps the range and interpolates well, but every reader pays an exp2 and
  every writer a log2 to save bandwidth the measurements show is small.
- **More march steps:** 128 steps hide the arcs, at eight times the cost of every march (0076 chose 16).
- **Linearizing the shadow test at each step** (lit = how far the step's middle is from the edge over the step's
  length): cheaper, but it goes back to all or nothing for rays that graze the shadow's edge.
