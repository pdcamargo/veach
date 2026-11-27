# 0036: Lights are components in photometric units

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (the lights part of old ADR 0034, with light sizes from its
soft-shadows work).

## Context

The real-time renderer and photo mode must mean the same thing by a light, or photo mode stops being ground truth.
Lights need units that a physical camera exposure ([ADR 0037](0037-exposure-and-agx.md)) and the physical sky
([ADR 0038](0038-physical-sky.md)) share. glTF's `KHR_lights_punctual` already defines them.

## Decision

**Lights are reflected, Scriptable components on entities** (`engine/render/include/veach/render/scene.hpp`),
oriented by the entity's `GlobalTransform`; directional and spot lights shine down its −Z. Units are photometric,
the glTF ones:

| Component | Fields | Units |
|---|---|---|
| `DirectionalLight` | `color` (linear RGB), `illuminance`, `angle` | lux; `angle` = apparent angular diameter in radians (the sun: 0.0093), 0 = hard shadows |
| `PointLight` | `color`, `intensity`, `radius` | candela; `radius` in meters (a sphere emitting the same intensity), 0 = punctual |
| `SpotLight` | `color`, `intensity`, `inner_angle`, `outer_angle`, `radius` | candela; cone angles in radians with KHR's smooth falloff; `radius` as `PointLight`'s |
| emissive surfaces | the material's `evalEmission` ([ADR 0035](0035-material-model.md)): glTF `emissiveFactor × emissiveStrength` | nits (cd/m²) |

- Extraction turns them into `render::RenderLight` (`radiance` = color × intensity or illuminance, `size` =
  radius or angular radius); renderers read only that.
- glTF lights imported with a scene become these components ([ADR 0011](0011-gltf-import-and-cooking.md)).
- **Light sizes are honored by photo mode** (soft shadows by sampling the sphere or the sun's disk). The real-time
  renderer may treat every light as punctual; how it shadows is a research-phase question.
- With the physical sky driving the sun (`Sky.drives_sun`), the sky's sun replaces the scene's directional lights.

## Consequences

- One meaning of a light for every renderer; values from photography and glTF transfer directly.
- Sunlit scenes are around 100,000 lux and lamps a few hundred candela, so exposure is mandatory
  ([ADR 0037](0037-exposure-and-agx.md)).
- Area lights other than emissive meshes, IBL and spectral lights are additions, not redesigns.

## Alternatives considered

- **Radiometric units (watts):** most engines and glTF use photometric units, and EV100 exposure is defined on them.
- **Unitless intensities:** every scene would need its own tuning, and photo mode could not be a reference.
