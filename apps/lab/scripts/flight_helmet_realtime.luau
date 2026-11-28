--!strict
-- Khronos's FlightHelmet (CC0) through the real-time renderer (docs/specs/realtime-renderer/spec.md, rt.AC3): textured
-- and normal-mapped, lit by a directional key light and a warm spot, unshadowed, plus the constant sky as ambient.
-- Golden image: tests/golden/flight_helmet_realtime.png (256x256).
-- Run: veach-lab --rhi metal --headless --frames 3 --script scripts/flight_helmet_realtime.luau --screenshot out.png
local veach = require("@veach")
local quat = veach.quat
local c = veach.components

veach.spawn("helmet"):set(c.SceneInstance, { path = "assets/flight_helmet/FlightHelmet.gltf" })

local camera = veach.spawn("camera")
camera:set(c.Transform, {
	translation = veach.position(0.25, 0.4, 1.35),
	rotation = quat.look_rotation(vector.create(-0.25, -0.04, -1.35)),
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(35), near_plane = 0.05, active = true, ev100 = 0 })
camera:set(c.Sky, { mode = "constant", color = vector.create(0.12, 0.13, 0.15) })

-- Lights shine down their -Z: look_rotation points it.
local key = veach.spawn("key light")
key:set(c.Transform, { translation = veach.position(0, 0, 0), rotation = quat.look_rotation(vector.create(-0.5, -0.6, -0.6)), scale = vector.one })
key:set(c.DirectionalLight, { color = vector.create(1, 0.97, 0.92), illuminance = 3 })

local spot = veach.spawn("spot light")
spot:set(c.Transform, {
	translation = veach.position(-0.8, 0.6, 0.6),
	rotation = quat.look_rotation(vector.create(0.8, -0.25, -0.6)),
	scale = vector.one,
})
spot:set(c.SpotLight, { color = vector.create(1, 0.8, 0.6), intensity = 1.5, inner_angle = 0.3, outer_angle = 0.5 })

veach.system("FlightHelmetSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 256, height = 256, mode = w.mode, vsync = w.vsync })
end)
