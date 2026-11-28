--!strict
-- The Cornell box inside a turned reference frame 1 AU out, seen by a camera in the root world (docs/specs/frames/spec.md,
-- frames.AC8): extraction composes the frame's pose into the camera's frame through their common ancestor, in double,
-- so the image matches tests/golden/cornell_box_realtime.png.
-- Run: veach-lab --rhi metal --headless --frames 3 --script scripts/frames_across.luau --screenshot out.png
local veach = require("@veach")
local c = veach.components
local frames = veach.frames

local au = 1.495978707e11
local turn = veach.quat.from_axis_angle(vector.create(0.2, 1, 0.1), 1)
local station = veach.spawn("station")
station:set(c.ReferenceFrame, {
	kind = "inertial",
	up = "axis_y",
	position = veach.position(0.6 * au, 0.48 * au, 0.64 * au),
	orientation = turn,
	linear_velocity = veach.position(0, 0, 0),
	angular_velocity = veach.position(0, 0, 0),
})
veach.spawn({ name = "cornell box", parent = station } :: veach.SpawnOptions)
	:set(c.SceneInstance, { path = "assets/cornell_box/cornell_box.gltf" })

-- The camera stands where cornell_box_realtime.luau's would inside the station, but in the root world. The frame
-- exists once this chunk's writes are applied, so place the camera at startup.
local camera = veach.spawn("camera")
camera:set(c.Camera, { fov_y = math.rad(40), near_plane = 0.1, active = true, ev100 = 0 })
camera:set(c.Sky, { mode = "none" })
veach.system("FramesPlaceCamera", "Startup", veach.query(c.Camera), function(e, _cam)
	e:set(c.Transform, {
		translation = frames.convert(veach.position(0, 1, 3.9), station, nil),
		rotation = turn,
		scale = vector.one,
	})
end)

veach.system("FramesSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 256, height = 256, mode = w.mode, vsync = w.vsync })
end)
