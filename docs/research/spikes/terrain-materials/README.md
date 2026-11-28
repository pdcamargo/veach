# Terrain materials (VCH-130): findings

What the slice looks like with terrain materials ([spec](../../../specs/terrain-materials/spec.md),
[ADR 0098](../../../adr/0098-terrain-materials.md), [guide](../../../guides/terrain-materials.md)): before (three flat
colors by voxel material id) and after (Quixel Megascans layers from `local-assets/quixel`, placed by the toolbox's
temperate biome), and the same flight with `surface_colors = "palette"`. Seed 42, system S:1000400100008000, M5 Max,
release, 2560x1440, from `VEACH_SLICE_AUTOPILOT=1 VEACH_SLICE_SHOTS=... veach-slice --script scripts/slice_s1.luau`
(and `slice_s0.01.luau`, `slice_alien_s1.luau`, `slice_alien_s0.01.luau`). Images downsized to 1280x720.

## Before and after

| View | s = 1 before | s = 1 after | s = 0.01 before | s = 0.01 after |
|---|---|---|---|---|
| Orbit | ![](s1_orbit_before.png) | ![](s1_orbit_after.png) | ![](s0.01_orbit_before.png) | ![](s0.01_orbit_after.png) |
| Half a radius up | ![](s1_high_before.png) | ![](s1_high_after.png) | ![](s0.01_high_before.png) | ![](s0.01_high_after.png) |
| 10 km | | ![](s1_10km_after.png) | | ![](s0.01_10km_after.png) |
| 200 m | ![](s1_200m_before.png) | ![](s1_200m_after.png) | ![](s0.01_200m_before.png) | ![](s0.01_200m_after.png) |
| Landed | ![](s1_landed_before.png) | ![](s1_landed_after.png) | ![](s0.01_landed_before.png) | ![](s0.01_landed_after.png) |
| In the dug hole | ![](s1_hole_before.png) | ![](s1_hole_after.png) | ![](s0.01_hole_before.png) | ![](s0.01_hole_after.png) |
| Cliff close-up | | ![](s1_cliff_after.png) | | ![](s0.01_cliff_after.png) |

## The alien palette (`surface_colors = "palette"`)

The same planet and flight; every layer keeps its scan's detail (normal, roughness, AO, height) and takes its color
from the planet's palette (vegetation crimson for this body's address, iron-red rock).

| | Orbit | Landed | In the hole | Cliff |
|---|---|---|---|---|
| s = 1 | ![](s1_alien_orbit.png) | ![](s1_alien_landed.png) | ![](s1_alien_hole.png) | ![](s1_alien_cliff.png) |
| s = 0.01 | ![](s0.01_alien_orbit.png) | ![](s0.01_alien_landed.png) | ![](s0.01_alien_hole.png) | ![](s0.01_alien_cliff.png) |

## What the images show

- **From orbit the biomes read**: green lowlands, snow toward the pole and on the heights, pale sand where it is dry,
  grey rocky ground high up. The orbit colors are the layers' means (no texture is read there), so they match what
  the ground shows on landing. Before, the planet was a patchwork of material-id cells with stair-stepped borders.
- **Landed**: grass scan at 2 m tiles, varied by a far tile (5.3 m) and a body-frame macro noise; no repetition
  visible at the craft's distance, none of the seams chunks would show (tiles of 16 / n m continue across them).
- **The hole** (2.5 m radius) shows soil in its walls and granite at the bottom-left where the dig reaches past 2 m
  below the generated surface; before it was meadow green.
- **Cliff close-up**: the slice's steepest ground near the site is 28 degrees at s = 1 and 32 at s = 0.01 (the
  planetary relief is gentle there), so the "cliff" is a rock-covered slope with grass on its flats; the borders
  follow the layers' heights. The granite scan reads as coarse gravel at 8 m tiles; a craggier scan for cliffs is a
  library choice (`apps/slice/terrain_layers.json`).

## What to look at critically

- **Coarse borders from orbit**: the weights live on vertices, so at the coarsest chunk levels (hundreds of meters to
  kilometers per triangle) a snow line or a beach border follows the triangles; the shader blends by weight alone
  there (height blending would sharpen them into stair steps). Finer at every level down.
- **Sunlit grass looks pale** next to shaded grass: AgX desaturates the bright greens (the flat material before did
  the same).
- **Granite** at close range is busy and dark; **beach sand** covers dry lowlands generously at s = 1.
- **Six layers per body**; the toolbox's climate is a stand-in (an equilibrium temperature plus Earth's greenhouse on
  temperate worlds).

## GPU time (M5 Max, 2560x1440, release, s = 1)

The `shade` pass (where TerrainMaterial runs, with every light, the sun's shadows, the sky's ambient light and aerial
perspective), one frame's GPU timestamps at each shot, one process at a time, TAA off in both runs. Single frames
vary by a few tenths of a millisecond.

| View | Before (flat colors) | After (terrain materials) |
|---|---|---|
| Orbit, half a radius up | 1.18 ms | 0.85 ms |
| 200 m | 1.58 ms | 1.75 ms |
| Landed (ground fills the screen) | 1.26 ms | 2.11 ms |
| Walking | 2.36 ms | 1.77 ms |
| In the dug hole (two scales, three layers at once) | 1.34 ms | 3.30 ms |
| Cliff close-up | | 2.56 ms |

Terrain materials cost about 0.8 to 2 ms of shading on a screen full of terrain at 1440p, nothing from orbit (the
means stand in). Measured after skipping the far tile's detail array where the near tile shows fully. The layer
library loads from its cache in 0.2 s; cooking all 15 layers at 2048 took 4 s.

## Cooking

15 Megascans sets at 2048x2048, two BC7 arrays (albedo and height; normal, roughness and AO), full mips: 4 s on the
M5 Max (release, 17 threads), 160 MB of arrays on the GPU. The cooker checks each normal map against its height
map's slopes and flips its red or green where they disagree, so sets in either convention shade the same way. The cache (`local-assets/cache/terrain-layers/`) is
keyed by the sources' names, sizes and times.
