--!strict
-- The Cornell box and its camera inside a reference frame 0.1 light-years out (docs/specs/frames/spec.md,
-- frames.AC8). Everything lives in the frame, so render space is the frame and the image is byte-identical to
-- tests/golden/cornell_box_realtime.png: how far the frame is never reaches the GPU.
-- Run: veach-lab --rhi metal --headless --frames 3 --script scripts/frames_far_frame.luau --screenshot out.png
local veach = require("@veach")
local c = veach.components

local ly = 9.4607304725808e15
local ship = veach.spawn("ship")
ship:set(c.ReferenceFrame, {
	kind = "inertial",
	up = "axis_y",
	position = veach.position(0.06 * ly, 0.048 * ly, 0.064 * ly),
	orientation = veach.quat.identity(),
	linear_velocity = veach.position(0, 0, 0),
	angular_velocity = veach.position(0, 0, 0),
})

veach.spawn({ name = "cornell box", parent = ship } :: veach.SpawnOptions)
	:set(c.SceneInstance, { path = "assets/cornell_box/cornell_box.gltf" })

local camera = veach.spawn({ name = "camera", parent = ship } :: veach.SpawnOptions)
camera:set(c.Transform, {
	translation = veach.position(0, 1, 3.9),
	rotation = veach.quat.identity(),
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(40), near_plane = 0.1, active = true, ev100 = 0 })
camera:set(c.Sky, { mode = "none" })

veach.system("FramesSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 256, height = 256, mode = w.mode, vsync = w.vsync })
end)
