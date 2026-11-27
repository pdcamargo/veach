--!strict
-- Bistro (CC BY 4.0, apps/lab/assets/bistro) under the physical sky at noon (12:30 solar time, June, Paris's latitude), through the
-- real-time renderer: the street and the sky above it (docs/specs/physical-sky/spec.md, sky.AC4).
-- Golden image: tests/golden/bistro_noon.png (640x360).
-- Run: veach-lab --rhi metal --window --script scripts/bistro_noon.luau
local veach = require("@veach")
local c = veach.components

veach.spawn("bistro"):set(c.SceneInstance, { path = "assets/bistro/BistroExterior.gltf" })

local camera = veach.spawn("camera")
camera:set(c.Transform, {
	translation = vector.create(-3, 2.5, -8),
	rotation = veach.quat.look_rotation(vector.create(-10, 4, -21)),
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(70), near_plane = 0.05, active = true, ev100 = 14 })
-- The sky drives the sun (the scene's own directional light gives way to it). North is +Z, so the noon sun comes
-- from -Z like the scene's own and the evening sun lights the facade.
veach.spawn("sky"):set(c.Sky, { time_of_day = 12.5, latitude = 48.9, day_of_year = 172, north = math.pi })

veach.system("BistroSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 640, height = 360, mode = w.mode, vsync = w.vsync })
end)
