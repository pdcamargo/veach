--!strict
-- Terrain materials (docs/specs/terrain-materials/spec.md, terrain_materials.AC5): hills and cliffs of a flat voxel
-- world drawn with TerrainMaterial over the procedural layer library (no scans, so the golden never depends on local
-- assets): grass on the flats, sand where it is dry, rock on the cliffs, snow on the tops, and soil in the walls of a
-- dug hole. Streamed synchronously, so the image never depends on timing.
-- Golden image: tests/golden/terrain_materials.png (480x270).
-- Run: veach-lab --rhi metal --headless --frames 4 --script scripts/terrain_materials.luau --screenshot out.png
local veach = require("@veach")
local c = veach.components


local camera = veach.spawn("camera")
camera:set(c.Transform, {
	translation = veach.position(0, 14, 26),
	rotation = veach.quat.look_rotation(vector.create(0, -0.42, -1)),
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(60), near_plane = 0.1, active = true, ev100 = 14 })
veach.spawn("sky"):set(c.Sky, { time_of_day = 15, latitude = 45, day_of_year = 172 })
-- After the camera: the scene streams around the entity named "camera".
veach.spawn("terrain"):set(c.TerrainMaterialsDemo, { palette = false, dig = true, dig_x = 2, dig_z = 10 })

veach.system("TerrainMaterialsSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 480, height = 270, mode = w.mode, vsync = w.vsync })
end)
