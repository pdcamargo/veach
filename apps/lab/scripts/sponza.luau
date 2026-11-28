--!strict
-- Intel's Sponza (local only until its license question is settled, VCH-055): the atrium from the scene's
-- first camera, lit by a sun through the open roof. 2560x1440, a large scene for the real-time renderer
-- (docs/specs/realtime-renderer/spec.md). Needs apps/lab/assets/sponza (not in git).
-- Run: veach-lab --rhi metal --window --script scripts/sponza.luau
local veach = require("@veach")
local c = veach.components

veach.spawn("sponza"):set(c.SceneInstance, { path = "assets/sponza/NewSponza_Main_glTF_003.gltf" })

local camera = veach.spawn("camera")
camera:set(c.Transform, {
	translation = veach.position(-8.81, 1.59, -0.86),
	rotation = { x = 0.0623, y = -0.7662, z = 0.0752, w = 0.6351 }, -- the scene's PhysCamera001
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(55), near_plane = 0.05, active = true, ev100 = 12 })

-- The sun, down the direction of the scene's own (disabled) directional light; daylight sky for the ambient.
local sun = veach.spawn("sun")
sun:set(c.Transform, {
	translation = veach.position(0, 0, 0),
	rotation = veach.quat.look_rotation(vector.create(0.551, -0.828, 0.105)),
	scale = vector.one,
})
sun:set(c.DirectionalLight, { color = vector.create(1, 0.95, 0.88), illuminance = 100000 })

veach.system("SponzaSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 2560, height = 1440, mode = w.mode, vsync = w.vsync })
end)
