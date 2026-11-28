--!strict
-- Temporal anti-aliasing in motion (docs/specs/temporal-aa/spec.md, taa.AC3): Khronos's FlightHelmet (CC0) turning in
-- front of the space backdrop while the camera pans, so the history is reprojected by both object and camera motion,
-- and the stars, drawn after TAA, stay points. Golden image: tests/golden/taa_motion.png, frame 24 with TAA on.
-- Run: veach-lab --rhi metal --headless --frames 24 --script scripts/taa_motion.luau --screenshot out.png
local veach = require("@veach")
local quat = veach.quat
local c = veach.components

local helmet = veach.spawn("helmet")
helmet:set(c.Transform, { translation = veach.position(0, 0, 0), rotation = quat.identity(), scale = vector.one })
helmet:set(c.SceneInstance, { path = "assets/flight_helmet/FlightHelmet.gltf" })

local eye = vector.create(0.25, 0.4, 1.35)
local camera = veach.spawn("camera")
camera:set(c.Camera, { fov_y = math.rad(35), near_plane = 0.05, active = true, ev100 = 0 })
camera:set(c.Sky, { mode = "none" })
camera:set(c.SpaceBackdrop, { seed = 7 })

local key = veach.spawn("key light")
key:set(c.Transform, {
	translation = veach.position(0, 0, 0),
	rotation = quat.look_rotation(vector.create(-0.5, -0.6, -0.6)),
	scale = vector.one,
})
key:set(c.DirectionalLight, { color = vector.create(1, 0.97, 0.92), illuminance = 3 })

-- Per frame, from the frame number (deterministic): the helmet turns 1.5 degrees, the camera pans 0.2 degrees.
local function pose(frame: number)
	helmet:set(c.Transform, {
		translation = veach.position(0, 0, 0),
		rotation = quat.from_euler(math.rad(1.5 * frame), 0),
		scale = vector.one,
	})
	camera:set(c.Transform, {
		translation = veach.position(eye),
		rotation = quat.mul(quat.from_euler(math.rad(0.2 * frame), 0), quat.look_rotation(vector.create(-0.25, -0.04, -1.35))),
		scale = vector.one,
	})
end
pose(0)
veach.system("TaaMotion", "Simulation", veach.query(c.Camera), function(e, _)
	pose(veach.time.frame())
end)

veach.system("TaaMotionSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 256, height = 256, mode = w.mode, vsync = w.vsync })
end)
