--!strict
-- Bistro (CC BY 4.0, apps/lab/assets/bistro) at noon, looking up at the underside of a café awning: thin fabric,
-- sunlit on top, lit only by bounced light and the sky from below. Candela's leak test (docs/specs/candela/spec.md,
-- candela.AC7): its radiance cache must not carry the sunlit top's light to the underside.
-- Run: veach-lab --rhi metal --window --script scripts/bistro_awning.luau
local veach = require("@veach")
local c = veach.components

veach.spawn("bistro"):set(c.SceneInstance, { path = "assets/bistro/BistroExterior.gltf" })

local camera = veach.spawn("camera")
camera:set(c.Transform, {
	translation = vector.create(-1.2, 2.2, -6.2),
	rotation = veach.quat.look_rotation(vector.create(-0.6, 1, -0.9)),
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(70), near_plane = 0.05, active = true, ev100 = 12 })
-- The sky drives the sun (the scene's own directional light gives way to it). North is +Z, so the noon sun comes
-- from -Z like the scene's own and the evening sun lights the facade.
veach.spawn("sky"):set(c.Sky, { time_of_day = 12.5, latitude = 48.9, day_of_year = 172, north = math.pi })

veach.system("BistroAwningSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 640, height = 360, mode = w.mode, vsync = w.vsync })
end)
