--!strict
-- engine/voxel's terrain under a governed descent (docs/specs/voxel-terrain/spec.md voxel.AC10): the lab's terrain view
-- (apps/lab/src/terrain_view.hpp) flies the camera from orbit to 10 m over a planet the engine streams, and quits by
-- itself after the last skim. This script sets the scene:
--   VEACH_TERRAIN_OUT=build/terrain veach-lab --rhi metal --headless --frames 20000 --script scripts/terrain_descent.luau
local veach = require("@veach")
local c = veach.components

local camera = veach.spawn("camera")
camera:set(c.Transform, { translation = veach.position(0, 0, 0), rotation = veach.quat.identity(), scale = vector.one })
camera:set(c.Camera, { fov_y = math.rad(60), near_plane = 0.5, active = true, ev100 = 15 })
camera:set(c.TerrainDescent, {})

-- Space: a dim constant sky, so faces away from the sun are not black. The view spawns the sun.
veach.spawn("sky"):set(c.Sky, { mode = "constant", color = vector.create(1500, 1800, 2400) })

veach.system("TerrainSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 960, height = 540, mode = w.mode, vsync = w.vsync })
end)
